/*
 * cmd_stash.c - set the work tree and the index aside, and put them back.
 *
 * A stash is three commits, the same three git writes.  The first holds the
 * work tree, the second holds the index as it stood, and a third -- written
 * only with -u -- holds the untracked files.  The first names the others as
 * its parents, so a whole stash is reachable from refs/stash, and each message
 * says which state it is: "WIP on <branch>: ...", "index on <branch>: ..." and
 * "untracked files on <branch>: ...".
 *
 * None of that shape is gitprompt's invention, and neither is what it is for.
 * An entry is an ordinary commit named through the reflog, so `stash@{0}` is a
 * revision like any other and anything that takes one -- show, log, diff,
 * cherry-pick -- can be pointed at a stash.  Putting one back is the same
 * three-way merge a replay is, with the commit the stash was made on as the
 * base and HEAD as ours, which is why a conflict leaves behind the index
 * stages `merge` and `cherry-pick` leave.
 */
#include "gp.h"

#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define stash_rmdir(p) _rmdir(p)
#else
#include <unistd.h>
#define stash_rmdir(p) rmdir(p)
#endif

#define STASH_REF "refs/stash"

/* ------------------------------------------------------------------ */
/* small pieces                                                        */

struct strlist {
	char **v;
	size_t nr, alloc;
};

static void sl_push(struct strlist *l, const char *s)
{
	if (l->nr == l->alloc) {
		l->alloc = l->alloc ? l->alloc * 2 : 8;
		l->v = xrealloc(l->v, l->alloc * sizeof(*l->v));
	}
	l->v[l->nr++] = xstrdup(s);
}

static void sl_release(struct strlist *l)
{
	size_t i;

	for (i = 0; i < l->nr; i++)
		free(l->v[i]);
	free(l->v);
	l->v = NULL;
	l->nr = l->alloc = 0;
}

static int sl_has(const struct strlist *l, const char *s)
{
	size_t i;

	for (i = 0; i < l->nr; i++)
		if (!strcmp(l->v[i], s))
			return 1;
	return 0;
}

/* hash one work-tree file into the object store; -1 when it is not there */
static int hash_wt(struct repo *r, const char *relpath, oid_t *oid, u32 *mode)
{
	struct buf b;
	char *full = r->root ? xstrfmt("%s/%s", r->root, relpath)
			     : xstrdup(relpath);
	int rc;

	buf_init(&b);
	if (read_file(full, &b) < 0) {
		free(full);
		buf_release(&b);
		return -1;
	}
	*mode = MODE_BLOB;
#ifndef _WIN32
	{
		struct stat st;

		if (stat(full, &st) == 0 && (st.st_mode & 0111))
			*mode = MODE_EXEC;
	}
#endif
	free(full);
	rc = odb_write(&r->odb, OBJ_BLOB, b.b, b.len, oid);
	buf_release(&b);
	return rc;
}

/*
 * The index the work tree would make: every path the index knows, hashed
 * again, with the ones no longer in the work tree dropped.  What the index
 * holds is the set of paths that count as tracked; what the files hold is the
 * state.
 */
static void index_from_worktree(struct repo *r, const struct index_state *idx,
				struct index_state *out)
{
	size_t i;

	for (i = 0; i < idx->nr; i++) {
		struct index_entry e = idx->e[i];
		u32 mode;

		if (hash_wt(r, e.path, &e.oid, &mode) < 0)
			continue;
		e.mode = mode;
		index_add(out, &e);
	}
}

struct ut_ctx {
	struct repo *r;
	const struct index_state *idx;
	struct index_state *out;
	struct strlist *names;
};

/*
 * An untracked file is one the work tree walk finds and the index does not
 * know.  The walk has already left out the ignored ones, which is the same
 * line git draws: -u takes the untracked and leaves the ignored behind.
 */
static void ut_one(const char *relpath, void *ud)
{
	struct ut_ctx *c = ud;
	struct index_entry e;
	oid_t oid;
	u32 mode;
	char *full;

	if (index_get(c->idx, relpath))
		return;
	if (hash_wt(c->r, relpath, &oid, &mode) < 0)
		return;
	memset(&e, 0, sizeof e);
	full = xstrfmt("%s/%s", c->r->root, relpath);
	index_fill_stat(&e, full);
	free(full);
	e.mode = mode;
	e.oid = oid;
	e.path = (char *)relpath;
	index_add(c->out, &e);
	sl_push(c->names, relpath);
}

/* the branch a stash message names, which a detached HEAD does not have */
static char *branch_label(struct repo *r)
{
	char *t = refs_head_target(&r->refs);
	char *out;

	if (t && !strncmp(t, "refs/heads/", 11))
		out = xstrdup(t + 11);
	else
		out = xstrdup("(no branch)");
	free(t);
	return out;
}

/* "<short id> <subject>" of the commit a stash is measured against */
static char *head_desc(struct repo *r, const oid_t *head)
{
	struct commit c = COMMIT_INIT;
	char *line, *sh, *out;

	read_commit(r, head, &c);
	line = commit_message_line(&c);
	sh = abbrev_oid(head);
	out = xstrfmt("%s %s", sh, line ? line : "");
	free(sh);
	free(line);
	commit_release(&c);
	return out;
}

/*
 * Write one of the three commits.  A stash commit is a commit like any other,
 * so it carries the prompts its tree brings in against the parent it has --
 * for the work-tree commit that is HEAD, and for the untracked one, which has
 * no parent, it is every prompt the untracked files hold.
 */
static int write_stash_commit(struct repo *r, const oid_t *tree,
			      const struct oid_array *parents,
			      const char *message,
			      const struct index_state *ist,
			      const oid_t *prev_tree, int have_prev,
			      oid_t *out)
{
	struct commit c = COMMIT_INIT;
	struct buf ident, body;
	size_t i;
	int rc;

	buf_init(&ident);
	buf_init(&body);
	repo_ident_with_time(r, &ident);

	c.tree = *tree;
	for (i = 0; i < parents->nr; i++)
		oid_array_append(&c.parents, &parents->oid[i]);
	c.author = xstrdup(buf_cstr(&ident));
	c.committer = xstrdup(buf_cstr(&ident));
	c.message = xstrdup(message);
	c.session = repo_current_session(r);
	if (ist)
		collect_commit_prompts(r, ist, prev_tree, have_prev, &c);

	commit_format(&c, &body);
	rc = odb_write(&r->odb, OBJ_COMMIT, body.b, body.len, out);

	commit_release(&c);
	buf_release(&ident);
	buf_release(&body);
	return rc;
}

/* the directories a removed file leaves empty go too, as git's do */
static void prune_dirs(struct repo *r, const char *relpath)
{
	char *dir = xstrdup(relpath);
	char *slash;

	while ((slash = strrchr(dir, '/')) != NULL) {
		char *full;

		*slash = '\0';
		full = xstrfmt("%s/%s", r->root, dir);
		if (stash_rmdir(full) != 0) {
			free(full);
			break;
		}
		free(full);
	}
	free(dir);
}

/* ------------------------------------------------------------------ */
/* naming an entry                                                     */

/*
 * Which entry `oid` is, counting from the newest, or -1 when the reflog does
 * not name it.  The number is what the messages say -- `stash@{2}` -- and the
 * only place it is written down is the reflog, so it is read from there rather
 * than parsed back out of whatever the caller typed: a raw id names the same
 * entry and gets the same number.
 */
static int stash_number(struct repo *r, const oid_t *oid)
{
	struct reflog_entry *e = NULL;
	int nr, i;

	nr = refs_reflog_read(&r->refs, STASH_REF, &e);
	if (nr <= 0) {
		reflog_entries_free(e, nr > 0 ? nr : 0);
		return -1;
	}
	for (i = nr - 1; i >= 0; i--) {
		if (oid_equal(&e[i].oid, oid)) {
			reflog_entries_free(e, nr);
			return nr - 1 - i;
		}
	}
	reflog_entries_free(e, nr);
	return -1;
}

/*
 * The stash a subcommand was pointed at: the revision named, or the newest
 * entry.  *n receives its number, or -1 when the revision is one the reflog
 * does not list.
 */
static int resolve_stash(struct repo *r, const char *rev, oid_t *out, int *n)
{
	if (rev) {
		if (resolve_rev(r, rev, out) < 0)
			return -1;
	} else if (refs_read(&r->refs, STASH_REF, out) < 0) {
		gp_error("No stash entries found.");
		return -1;
	}
	if (n)
		*n = stash_number(r, out);
	return 0;
}

/*
 * Drop one entry and, when it was the last, the ref with it: a stash that is
 * not there is not a stash with an empty history, and leaving the ref behind
 * would make `stash@{0}` name a commit nothing points at.
 */
static void stash_forget(struct repo *r, int n)
{
	struct reflog_entry *e = NULL;
	int left;

	refs_reflog_drop(&r->refs, STASH_REF, n);
	left = refs_reflog_read(&r->refs, STASH_REF, &e);
	reflog_entries_free(e, left > 0 ? left : 0);
	if (left <= 0)
		refs_delete(&r->refs, STASH_REF);
}

/* ------------------------------------------------------------------ */
/* push                                                                */

static int stash_push(struct repo *r, const char *message, int untracked,
		      int keep_index)
{
	struct index_state idx, wt, ut;
	struct oid_array par = OID_ARRAY_INIT;
	struct strlist removed = { NULL, 0, 0 };
	struct commit hc = COMMIT_INIT;
	oid_t head, head_tree, index_tree, wt_tree, ut_tree;
	oid_t index_oid, ut_oid, stash_oid, old_stash = null_oid;
	char *br = NULL, *desc = NULL, *msg = NULL;
	size_t i;
	int had_old, rc = 1;

	memset(&idx, 0, sizeof idx);
	memset(&wt, 0, sizeof wt);
	memset(&ut, 0, sizeof ut);

	/*
	 * A stash is measured against HEAD, so there has to be one.  There is
	 * nothing to hang the index commit's parent on otherwise, and git
	 * refuses here for the same reason.
	 */
	if (refs_head(&r->refs, &head) < 0) {
		gp_error("You do not have the initial commit yet");
		return 1;
	}
	index_read(&idx, repo_index_path(r));

	/*
	 * An unmerged path has no one content a tree could hold -- that is what
	 * being unmerged means -- so neither tree can be written while one is
	 * in the index.  git stops here too, naming the paths.
	 */
	if (index_has_unmerged(&idx)) {
		size_t n = 0, k;
		char **paths = index_unmerged_paths(&idx, &n);

		gp_error("could not write index");
		for (k = 0; k < n; k++)
			fprintf(stderr, "%s: needs merge\n", paths[k]);
		index_paths_free(paths);
		index_release(&idx);
		return 1;
	}

	read_commit(r, &head, &hc);
	head_tree = hc.tree;

	if (write_tree_from_index(r, &idx, &index_tree) < 0)
		goto out;
	index_from_worktree(r, &idx, &wt);
	if (write_tree_from_index(r, &wt, &wt_tree) < 0)
		goto out;
	if (untracked) {
		struct ut_ctx uc;

		uc.r = r;
		uc.idx = &idx;
		uc.out = &ut;
		uc.names = &removed;
		walk_worktree(r, ut_one, &uc);
		if (write_tree_from_index(r, &ut, &ut_tree) < 0)
			goto out;
	}

	/*
	 * Nothing to save is not an error, and it is not an empty stash either:
	 * git writes no commit and says so.  Both trees have to equal HEAD's
	 * for it to be true, since a staged change and an unstaged one are
	 * each on their own enough to be worth keeping.
	 */
	if (oid_equal(&wt_tree, &head_tree) &&
	    oid_equal(&index_tree, &head_tree) &&
	    (!untracked || ut.nr == 0)) {
		printf("No local changes to save\n");
		rc = 0;
		goto out;
	}

	br = branch_label(r);
	desc = head_desc(r, &head);
	/*
	 * The one line a stash is known by, and the one the reflog repeats.  git
	 * writes no trailing newline into the commit, so neither does this: the
	 * message a reader gets back is the message that went in.
	 */
	msg = message ? xstrfmt("On %s: %s", br, message)
		      : xstrfmt("WIP on %s: %s", br, desc);

	{
		struct oid_array ip = OID_ARRAY_INIT;
		char *im = xstrfmt("index on %s: %s", br, desc);

		oid_array_append(&ip, &head);
		if (write_stash_commit(r, &index_tree, &ip, im, &idx, &head_tree,
				       1, &index_oid) < 0)
			gp_die("cannot write the stash");
		oid_array_clear(&ip);
		free(im);
	}
	if (untracked) {
		struct oid_array np = OID_ARRAY_INIT;
		char *nm = xstrfmt("untracked files on %s: %s", br, desc);

		if (write_stash_commit(r, &ut_tree, &np, nm, &ut, NULL, 0,
				       &ut_oid) < 0)
			gp_die("cannot write the stash");
		oid_array_clear(&np);
		free(nm);
	}

	oid_array_append(&par, &head);
	oid_array_append(&par, &index_oid);
	if (untracked)
		oid_array_append(&par, &ut_oid);
	if (write_stash_commit(r, &wt_tree, &par, msg, &wt, &head_tree, 1,
			       &stash_oid) < 0)
		gp_die("cannot write the stash");

	had_old = refs_read(&r->refs, STASH_REF, &old_stash) == 0;
	refs_write(&r->refs, STASH_REF, &stash_oid);
	refs_reflog(&r->refs, STASH_REF, had_old ? &old_stash : &null_oid,
		    &stash_oid, msg);
	printf("Saved working directory and index state %s\n", msg);

	/*
	 * Put the work tree back.  What it goes back to is the difference
	 * between the two modes: an ordinary stash leaves it on HEAD, and
	 * --keep-index leaves it on the index, which is what makes the staged
	 * change still there and still staged afterwards.
	 */
	if (keep_index)
		checkout_tree(r, &index_tree, 1, 1);
	else
		checkout_tree(r, &head_tree, 1, 1);

	/*
	 * The untracked files were in no index, so the checkout above had no
	 * reason to touch them and did not.  They went into the stash, so they
	 * go out of the work tree here, and the directories they leave empty
	 * go with them.
	 */
	for (i = 0; i < removed.nr; i++) {
		char *full = xstrfmt("%s/%s", r->root, removed.v[i]);

		remove_file(full);
		free(full);
		prune_dirs(r, removed.v[i]);
	}
	rc = 0;

out:
	free(br);
	free(desc);
	free(msg);
	oid_array_clear(&par);
	index_release(&idx);
	index_release(&wt);
	index_release(&ut);
	commit_release(&hc);
	sl_release(&removed);
	return rc;
}

/* ------------------------------------------------------------------ */
/* apply                                                               */

struct ut_restore_ctx {
	struct repo *r;
	int failed;
};

/*
 * An untracked file the stash carried is written back only where nothing has
 * taken its name: a file that is there now is somebody's, and the stash does
 * not get to write over it.
 */
static void ut_restore_one(const char *path, u32 mode, const oid_t *oid,
			   void *ud)
{
	struct ut_restore_ctx *c = ud;
	char *full = xstrfmt("%s/%s", c->r->root, path);

	(void)mode;
	if (is_file(full)) {
		fprintf(stderr, "%s already exists, no checkout\n", path);
		c->failed = 1;
	} else {
		write_blob_to_worktree(c->r, path, oid);
	}
	free(full);
}

/* the paths that differ between two trees, which is what an apply would touch */
static void touched_paths(struct repo *r, const oid_t *base,
			  const oid_t *side, struct strlist *out)
{
	struct index_state bi, si;
	size_t i;

	memset(&bi, 0, sizeof bi);
	memset(&si, 0, sizeof si);
	read_tree_into_index(r, &bi, base, "");
	read_tree_into_index(r, &si, side, "");

	for (i = 0; i < bi.nr; i++) {
		struct index_entry *s = index_get(&si, bi.e[i].path);

		if (!s || !oid_equal(&s->oid, &bi.e[i].oid) ||
		    s->mode != bi.e[i].mode)
			sl_push(out, bi.e[i].path);
	}
	for (i = 0; i < si.nr; i++)
		if (!index_get(&bi, si.e[i].path))
			sl_push(out, si.e[i].path);

	index_release(&bi);
	index_release(&si);
}

/*
 * Put a stash back.  This is the merge a replay performs, read with the commit
 * the stash was made on as the base: the state that was stashed is the far
 * side.  The near side is the index rather than HEAD, which is what makes an
 * entry taken with -k -- whose index is still ahead of HEAD -- meet the work
 * it was taken from as a conflict rather than as an overwrite, and it is the
 * side git merges into as well.  A clean result leaves the work tree holding
 * it and the index back at HEAD, so the stash reads as changes not yet staged,
 * which is what git does when it is not asked for --index.
 */
static int stash_apply(struct repo *r, const char *rev, int drop)
{
	struct index_state merged, ist;
	struct commit sc = COMMIT_INIT, bc = COMMIT_INIT, hc = COMMIT_INIT;
	struct merge_result res;
	struct strlist touched = { NULL, 0, 0 };
	oid_t stash_oid, head, head_tree, base_tree, ours_tree;
	char **dirty = NULL;
	char hex[GP_SHA1_HEXSZ + 1];
	int n = 0, k, rc = 1;

	memset(&merged, 0, sizeof merged);
	memset(&ist, 0, sizeof ist);
	memset(&res, 0, sizeof res);

	if (resolve_stash(r, rev, &stash_oid, &n) < 0)
		return 1;
	read_commit(r, &stash_oid, &sc);
	if (sc.parents.nr == 0) {
		gp_error("'%s' is not a stash", rev ? rev : "stash");
		goto out;
	}
	read_commit(r, &sc.parents.oid[0], &bc);
	base_tree = bc.tree;

	if (refs_head(&r->refs, &head) < 0) {
		gp_error("You do not have the initial commit yet");
		goto out;
	}
	read_commit(r, &head, &hc);
	head_tree = hc.tree;

	index_read(&ist, repo_index_path(r));
	if (write_tree_from_index(r, &ist, &ours_tree) < 0)
		ours_tree = head_tree;

	/*
	 * A file the merge would write is a file whose uncommitted content is
	 * about to be lost, so those are the ones checked: a change somewhere
	 * the stash does not reach is none of its business and is left alone.
	 */
	touched_paths(r, &base_tree, &sc.tree, &touched);
	k = worktree_dirty_paths(r, &dirty);
	{
		int blocked = 0;

		for (k = 0; dirty && dirty[k]; k++)
			if (sl_has(&touched, dirty[k]))
				blocked = 1;
		if (blocked) {
			gp_error("Your local changes to the following files "
				 "would be overwritten by merge:");
			for (k = 0; dirty[k]; k++)
				if (sl_has(&touched, dirty[k]))
					fprintf(stderr, "\t%s\n", dirty[k]);
			fprintf(stderr, "Please commit your changes or stash "
					"them before you merge.\nAborting\n");
			path_list_free(dirty);
			cmd_status(r, 0, NULL);
			goto out;
		}
	}
	path_list_free(dirty);
	dirty = NULL;

	merge_trees_labeled(r, &base_tree, &ours_tree, &sc.tree, &res, &merged,
			    MERGE_FAVOR_NONE, "Updated upstream", "Stashed changes");

	if (res.conflicts) {
		/*
		 * The conflicted stages are the merge's own record of what is
		 * left to settle, and the entry stays where it was: a half
		 * applied stash is still the only copy of the rest.
		 */
		index_write(&merged, repo_index_path(r));
		cmd_status(r, 0, NULL);
		if (drop)
			printf("The stash entry is kept in case you need it "
			       "again.\n");
		goto out;
	}

	{
		struct index_state back;

		memset(&back, 0, sizeof back);
		read_tree_into_index(r, &back, &head_tree, "");
		index_write(&back, repo_index_path(r));
		index_release(&back);
	}

	/*
	 * A stash taken with -u carries a third parent, and the untracked files
	 * in it are not in the stash commit's tree -- they were never in the
	 * index -- so the merge above did not write them and this does.
	 */
	if (sc.parents.nr >= 3) {
		struct commit uc = COMMIT_INIT;
		struct ut_restore_ctx uc_ctx;

		read_commit(r, &sc.parents.oid[2], &uc);
		uc_ctx.r = r;
		uc_ctx.failed = 0;
		load_tree_flat(r, &uc.tree, "", ut_restore_one, &uc_ctx);
		commit_release(&uc);
		if (uc_ctx.failed) {
			fprintf(stderr, "error: could not restore untracked "
					"files from stash\n");
			cmd_status(r, 0, NULL);
			if (drop)
				printf("The stash entry is kept in case you "
				       "need it again.\n");
			goto out;
		}
	}

	cmd_status(r, 0, NULL);

	if (drop) {
		oid_hex(&stash_oid, hex);
		stash_forget(r, n);
		printf("Dropped refs/stash@{%d} (%s)\n", n, hex);
	}
	rc = 0;

out:
	commit_release(&sc);
	commit_release(&bc);
	commit_release(&hc);
	index_release(&merged);
	index_release(&ist);
	sl_release(&touched);
	return rc;
}

/* ------------------------------------------------------------------ */
/* the subcommands                                                     */

static int stash_list(struct repo *r)
{
	struct reflog_entry *e = NULL;
	int nr, i;

	nr = refs_reflog_read(&r->refs, STASH_REF, &e);
	if (nr <= 0) {
		reflog_entries_free(e, nr > 0 ? nr : 0);
		return 0;
	}
	for (i = nr - 1; i >= 0; i--)
		printf("stash@{%d}: %s\n", nr - 1 - i, e[i].msg);
	reflog_entries_free(e, nr);
	return 0;
}

static int stash_show(struct repo *r, const char *rev, int patch)
{
	struct commit sc = COMMIT_INIT, bc = COMMIT_INIT;
	struct buf out;
	oid_t stash_oid;
	int rc = 0;

	buf_init(&out);
	if (resolve_stash(r, rev, &stash_oid, NULL) < 0) {
		buf_release(&out);
		return 1;
	}
	read_commit(r, &stash_oid, &sc);
	if (sc.parents.nr == 0) {
		gp_error("'%s' is not a stash", rev ? rev : "stash");
		rc = 1;
		goto out;
	}
	/*
	 * What a stash shows is what it would put back, so the far side of the
	 * diff is the commit it was made on, not the index it left behind.
	 */
	read_commit(r, &sc.parents.oid[0], &bc);
	diff_trees(r, &bc.tree, &sc.tree, &out, !patch);
	if (out.len)
		fwrite(out.b, 1, out.len, stdout);

out:
	commit_release(&sc);
	commit_release(&bc);
	buf_release(&out);
	return rc;
}

static int stash_drop(struct repo *r, const char *rev)
{
	struct reflog_entry *e = NULL;
	oid_t stash_oid;
	char hex[GP_SHA1_HEXSZ + 1];
	int n, nr;

	if (resolve_stash(r, rev, &stash_oid, &n) < 0)
		return 1;
	nr = refs_reflog_read(&r->refs, STASH_REF, &e);
	reflog_entries_free(e, nr > 0 ? nr : 0);
	if (n < 0 || n >= nr) {
		gp_error("'%s' is not a stash reference",
			 rev ? rev : "stash");
		return 1;
	}
	oid_hex(&stash_oid, hex);
	stash_forget(r, n);
	printf("Dropped refs/stash@{%d} (%s)\n", n, hex);
	return 0;
}

static int stash_clear(struct repo *r)
{
	struct reflog_entry *e = NULL;
	int nr;

	/*
	 * All of it goes at once, log included: dropping the entries one by
	 * one would say the same thing more slowly and leave the ref behind
	 * until the last one.
	 */
	nr = refs_reflog_read(&r->refs, STASH_REF, &e);
	reflog_entries_free(e, nr > 0 ? nr : 0);
	refs_reflog_delete(&r->refs, STASH_REF);
	refs_delete(&r->refs, STASH_REF);
	return 0;
}

/*
 * Make a branch where the stash was made and put the stash back on it.  The
 * work tree has to be clean first: moving it somewhere else while it holds
 * uncommitted work is the one thing this command exists to avoid, and the
 * apply below would refuse anyway.
 */
static int stash_branch(struct repo *r, const char *name, const char *rev)
{
	struct commit sc = COMMIT_INIT, bc = COMMIT_INIT;
	oid_t stash_oid, base, old_head, base_tree;
	char *ref, *from;
	char **dirty = NULL;
	int had, k, rc = 1;

	if (!name)
		gp_die("stash branch: expected a branch name");
	ref = xstrfmt("refs/heads/%s", name);
	if (refs_check_name(ref) < 0)
		gp_die("'%s' is not a valid branch name", name);
	if (refs_exists(&r->refs, ref))
		gp_die("a branch named '%s' already exists", name);

	if (resolve_stash(r, rev, &stash_oid, NULL) < 0) {
		free(ref);
		return 1;
	}
	read_commit(r, &stash_oid, &sc);
	if (sc.parents.nr == 0) {
		gp_error("'%s' is not a stash", rev ? rev : "stash");
		goto out;
	}
	base = sc.parents.oid[0];
	read_commit(r, &base, &bc);
	base_tree = bc.tree;

	if (worktree_dirty_paths(r, &dirty) > 0) {
		gp_error("Your local changes to the following files would be "
			 "overwritten by checkout:");
		for (k = 0; dirty[k]; k++)
			fprintf(stderr, "\t%s\n", dirty[k]);
		path_list_free(dirty);
		goto out;
	}

	from = branch_label(r);
	had = refs_head(&r->refs, &old_head) == 0;
	checkout_tree(r, &base_tree, 1, 1);
	refs_write(&r->refs, ref, &base);
	refs_reflog(&r->refs, ref, &null_oid, &base, "branch: Created");
	{
		char *msg = xstrfmt("checkout: moving from %s to %s", from, name);

		refs_set_head(&r->refs, ref);
		refs_reflog_head(&r->refs, NULL, had ? &old_head : &null_oid,
				 &base, msg);
		free(msg);
	}
	free(from);
	printf("Switched to a new branch '%s'\n", name);

	rc = stash_apply(r, rev, 1);

out:
	commit_release(&sc);
	commit_release(&bc);
	free(ref);
	return rc;
}

/* ------------------------------------------------------------------ */

/*
 * An option the command takes but the subcommand asked for does not.  The
 * parser sees one option list for the whole command, so a name that belongs to
 * one subcommand parses under all of them; the split is made here, where which
 * subcommand it is has been decided, rather than letting `stash show -u` look
 * as though -u had done something.
 */
static int wrong_subcommand(const char *sub, const char *opt, const char *owner)
{
	gp_error("the %s option belongs to 'stash %s', not 'stash %s'",
		 opt, owner, sub);
	return 1;
}

static int seen(struct opts *o, const char *a, const char *b)
{
	return opts_flag(o, a) || opts_flag(o, b);
}

/* every option but the message and the patch is push's */
static int no_push_options(struct opts *o, const char *sub)
{
	if (seen(o, "-m", "--message"))
		return wrong_subcommand(sub, "-m", "push");
	if (seen(o, "-u", "--include-untracked"))
		return wrong_subcommand(sub, "-u", "push");
	if (seen(o, "-k", "--keep-index"))
		return wrong_subcommand(sub, "-k", "push");
	if (seen(o, "-p", "--patch"))
		return wrong_subcommand(sub, "-p", "show");
	return 0;
}

int cmd_stash(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *sub, *msg;

	opts_init(&o, argc, argv, (const char *const[]){
		"-m=", "--message=", "-u", "--include-untracked",
		"-k", "--keep-index", "-p", "--patch", NULL });

	sub = opts_arg(&o, 0);
	if (!sub || !strcmp(sub, "push") || !strcmp(sub, "save")) {
		msg = opts_value(&o, "-m");
		if (!msg)
			msg = opts_value(&o, "--message");
		if (seen(&o, "-p", "--patch"))
			return wrong_subcommand(sub ? sub : "push", "-p",
						"show");
		return stash_push(r, msg,
				  seen(&o, "-u", "--include-untracked"),
				  seen(&o, "-k", "--keep-index"));
	}
	if (!strcmp(sub, "clear") || !strcmp(sub, "list")) {
		if (no_push_options(&o, sub))
			return 1;
		return sub[0] == 'c' ? stash_clear(r) : stash_list(r);
	}
	if (!strcmp(sub, "show")) {
		if (seen(&o, "-m", "--message"))
			return wrong_subcommand(sub, "-m", "push");
		if (seen(&o, "-u", "--include-untracked"))
			return wrong_subcommand(sub, "-u", "push");
		if (seen(&o, "-k", "--keep-index"))
			return wrong_subcommand(sub, "-k", "push");
		return stash_show(r, opts_arg(&o, 1),
				  seen(&o, "-p", "--patch"));
	}
	if (!strcmp(sub, "apply") || !strcmp(sub, "pop")) {
		if (no_push_options(&o, sub))
			return 1;
		return stash_apply(r, opts_arg(&o, 1), sub[0] == 'p');
	}
	if (!strcmp(sub, "drop")) {
		if (no_push_options(&o, sub))
			return 1;
		return stash_drop(r, opts_arg(&o, 1));
	}
	if (!strcmp(sub, "branch")) {
		if (no_push_options(&o, sub))
			return 1;
		return stash_branch(r, opts_arg(&o, 1), opts_arg(&o, 2));
	}

	gp_error("'%s' is not a stash subcommand", sub);
	return 1;
}
