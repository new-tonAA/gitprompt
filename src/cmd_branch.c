/*
 * cmd_branch.c - branch, checkout, switch, merge, tag.
 *
 * These are the ordinary git operations, with the same names and much the
 * same behaviour.  They know nothing about prompts: a branch here is a
 * branch, a merge is a merge.
 */
#include "gp.h"

#include <ctype.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* shared                                                             */

struct strlist {
	char **v;
	size_t nr, alloc;
};

static void strlist_push(struct strlist *l, const char *s)
{
	if (l->nr == l->alloc) {
		l->alloc = l->alloc ? l->alloc * 2 : 16;
		l->v = xrealloc(l->v, l->alloc * sizeof(*l->v));
	}
	l->v[l->nr++] = xstrdup(s);
}

static void strlist_release(struct strlist *l)
{
	size_t i;
	for (i = 0; i < l->nr; i++)
		free(l->v[i]);
	free(l->v);
	l->v = NULL;
	l->nr = l->alloc = 0;
}

static char *branch_ref(const char *name)
{
	return xstrfmt("refs/heads/%s", name);
}

/* the branch HEAD is on, or NULL when detached */
static char *current_branch(struct repo *r)
{
	char *t = refs_head_target(&r->refs);
	char *name;

	if (!t)
		return NULL;
	if (strncmp(t, "refs/heads/", 11)) {
		free(t);
		return NULL;
	}
	name = xstrdup(t + 11);
	free(t);
	return name;
}

/*
 * How HEAD's position reads in a reflog message: the branch's short name, or
 * the abbreviated id when HEAD is detached.  git writes the same, so a reflog
 * reads alike whichever way it got there.
 */
static char *head_position(struct repo *r)
{
	char *b = current_branch(r);
	oid_t oid;

	if (b)
		return b;
	if (refs_head(&r->refs, &oid) < 0)
		return xstrdup("HEAD");
	return abbrev_oid(&oid);
}

/*
 * A switch rewrites HEAD rather than the branch, so the record of it belongs to
 * HEAD alone -- the branch's own log has nothing to say about the move.  `from`
 * is HEAD's position before the move, taken while it was still the old one.
 */
static void reflog_switch(struct repo *r, const char *from, const char *to,
			  const oid_t *old, int had, const oid_t *new)
{
	char *msg = xstrfmt("checkout: moving from %s to %s", from, to);

	refs_reflog_head(&r->refs, NULL, had ? old : &null_oid, new, msg);
	free(msg);
}

/* paths that differ between the index and the work tree */
static void dirty_paths(struct repo *r, struct strlist *out)
{
	struct index_state ist;
	size_t i;

	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));
	for (i = 0; i < ist.nr; i++) {
		char *full = xstrfmt("%s/%s", r->root, ist.e[i].path);
		struct buf b;
		oid_t oid;

		buf_init(&b);
		if (read_file(full, &b) < 0) {
			strlist_push(out, ist.e[i].path);
		} else {
			odb_hash(&r->odb, OBJ_BLOB, b.b, b.len, &oid, 0);
			if (!oid_equal(&oid, &ist.e[i].oid))
				strlist_push(out, ist.e[i].path);
		}
		buf_release(&b);
		free(full);
	}
	index_release(&ist);
}

static int strlist_has(const struct strlist *l, const char *s)
{
	size_t i;
	for (i = 0; i < l->nr; i++)
		if (!strcmp(l->v[i], s))
			return 1;
	return 0;
}

/*
 * dirty_paths for a caller outside this file, which has no strlist to hand it:
 * the same list as a NULL-terminated array the caller frees with
 * path_list_free.  Returns how many there are.
 */
int worktree_dirty_paths(struct repo *r, char ***paths)
{
	struct strlist l = { NULL, 0, 0 };
	size_t i, n;

	dirty_paths(r, &l);
	*paths = NULL;
	n = l.nr;
	if (!n) {
		strlist_release(&l);
		return 0;
	}
	*paths = xmalloc((n + 1) * sizeof(**paths));
	for (i = 0; i < n; i++)
		(*paths)[i] = xstrdup(l.v[i]);
	(*paths)[n] = NULL;
	strlist_release(&l);
	return (int)n;
}

void path_list_free(char **paths)
{
	size_t i;

	if (!paths)
		return;
	for (i = 0; paths[i]; i++)
		free(paths[i]);
	free(paths);
}

/* ------------------------------------------------------------------ */
/* branch                                                             */

struct branch_list_ctx {
	struct repo *r;
	const char *cur;
	int verbose;
};

static void branch_list_cb(const char *name, const oid_t *oid, void *ud)
{
	struct branch_list_ctx *c = ud;
	const char *shortname = name;
	char hex[GP_SHA1_HEXSZ + 1];

	if (!strncmp(shortname, "refs/heads/", 11))
		shortname += 11;
	else if (!strncmp(shortname, "refs/remotes/", 13))
		shortname += 13;

	oid_hex(oid, hex);
	hex[7] = '\0';
	if (c->verbose)
		printf("%s%s %s\n",
		       c->cur && !strcmp(c->cur, shortname) ? "* " : "  ",
		       shortname, hex);
	else
		printf("%s%s\n",
		       c->cur && !strcmp(c->cur, shortname) ? "* " : "  ",
		       shortname);
}

int cmd_branch(struct repo *r, int argc, char **argv)
{
	struct opts o;
	char *cur = current_branch(r);
	const char *del, *move;

	opts_init(&o, argc, argv, (const char *const[]){
		"-d=", "--delete=", "-m=", "--move=", "-l", "--list",
		"-v", "--verbose", "-a", "--all", NULL });
	del = opts_value(&o, "-d") ? opts_value(&o, "-d")
				   : opts_value(&o, "--delete");
	move = opts_value(&o, "-m") ? opts_value(&o, "-m")
				    : opts_value(&o, "--move");

	if (opts_flag(&o, "-d") || opts_flag(&o, "--delete")) {
		const char *name = del ? del : opts_arg(&o, 0);
		char *ref;
		if (!name)
			gp_die("branch -d: expected a branch name");
		if (cur && !strcmp(cur, name))
			gp_die("Cannot delete branch '%s' checked out at HEAD", name);
		ref = branch_ref(name);
		if (!refs_exists(&r->refs, ref))
			gp_die("branch '%s' not found", name);
		refs_delete(&r->refs, ref);
		printf("Deleted branch %s\n", name);
		free(ref);
		free(cur);
		return 0;
	}

	if (opts_flag(&o, "-m") || opts_flag(&o, "--move")) {
		const char *from = opts_arg(&o, 0);
		const char *to = move ? move : opts_arg(&o, 1);
		char *old_ref, *new_ref;
		oid_t oid;

		if (move) {
			from = cur;
			to = move;
		} else if (!to) {
			to = from;
			from = cur;
		}
		if (!from || !to)
			gp_die("branch -m: expected <old> <new>");
		if (refs_exists(&r->refs, branch_ref(to)))
			gp_die("a branch named '%s' already exists", to);
		old_ref = branch_ref(from);
		if (refs_read(&r->refs, old_ref, &oid) < 0)
			gp_die("branch '%s' not found", from);
		new_ref = branch_ref(to);
		refs_write(&r->refs, new_ref, &oid);
		refs_delete(&r->refs, old_ref);
		if (cur && !strcmp(cur, from)) {
			char *msg = xstrfmt("Branch: renamed %s to %s",
					    old_ref, new_ref);

			refs_set_head(&r->refs, new_ref);
			refs_reflog_head(&r->refs, new_ref, &oid, &oid, msg);
			free(msg);
		}
		free(old_ref);
		free(new_ref);
		free(cur);
		return 0;
	}

	if (opts_flag(&o, "-l") || opts_flag(&o, "--list") || !opts_arg(&o, 0)) {
		struct branch_list_ctx c;
		c.r = r;
		c.cur = cur;
		c.verbose = opts_flag(&o, "-v") || opts_flag(&o, "--verbose");
		refs_list(&r->refs, "refs/heads/", branch_list_cb, &c);
		if (opts_flag(&o, "-a") || opts_flag(&o, "--all"))
			refs_list(&r->refs, "refs/remotes/", branch_list_cb, &c);
		free(cur);
		return 0;
	}

	/* create */
	{
		const char *name = opts_arg(&o, 0);
		const char *start = opts_arg(&o, 1);
		oid_t oid;
		char *ref;

		if (refs_exists(&r->refs, branch_ref(name)))
			gp_die("a branch named '%s' already exists", name);
		if (refs_check_name(branch_ref(name)) < 0)
			gp_die("'%s' is not a valid branch name", name);

		if (start) {
			if (resolve_rev(r, start, &oid) < 0)
				gp_die("not a valid object name: %s", start);
			if (commit_peel(r, &oid, OBJ_COMMIT, &oid) < 0)
				gp_die("not a commit: %s", start);
		} else if (refs_head(&r->refs, &oid) < 0) {
			gp_die("Not a valid object name: 'HEAD'.\n"
			       "hint: there are no commits yet; record one first");
		}

		ref = branch_ref(name);
		refs_write(&r->refs, ref, &oid);
		refs_reflog(&r->refs, ref, &null_oid, &oid, "branch: Created");
		free(ref);
	}
	free(cur);
	return 0;
}

/* ------------------------------------------------------------------ */
/* checkout / switch                                                  */

/* refuse to lose work: any local modification, or an untracked file that
 * the target tree would overwrite */
static int guard_worktree(struct repo *r, const oid_t *target_tree, int force)
{
	struct strlist dirty = { NULL, 0, 0 };
	struct index_state target, ist;
	size_t i;
	int rc = 0;

	if (force || !r->root)
		return 0;

	dirty_paths(r, &dirty);
	if (dirty.nr) {
		gp_error("Your local changes to the following files would be "
			 "overwritten:");
		for (i = 0; i < dirty.nr; i++)
			fprintf(stderr, "\t%s\n", dirty.v[i]);
		fprintf(stderr, "hint: commit them or pass -f to discard them\n");
		strlist_release(&dirty);
		return -1;
	}

	memset(&target, 0, sizeof target);
	memset(&ist, 0, sizeof ist);
	read_tree_into_index(r, &target, target_tree, "");
	index_read(&ist, repo_index_path(r));

	for (i = 0; i < target.nr; i++) {
		char *full = xstrfmt("%s/%s", r->root, target.e[i].path);
		if (is_file(full) && !index_get(&ist, target.e[i].path) &&
		    !strlist_has(&dirty, target.e[i].path)) {
			gp_error("The following untracked working tree file would "
				 "be overwritten by checkout:\n\t%s\n"
				 "hint: move it aside or pass -f",
				 target.e[i].path);
			free(full);
			rc = -1;
			break;
		}
		free(full);
	}

	index_release(&ist);
	index_release(&target);
	strlist_release(&dirty);
	return rc;
}

/*
 * git's DWIM: for a bare branch name with no local branch of that name, a
 * remote-tracking ref refs/remotes/<remote>/<name> is what the new branch is
 * made from, and it is set up to track it.  Returns the remote that has one,
 * or NULL.  "origin" is preferred and then the rest, which is the order git
 * searches in and the one that matters after a clone, where origin is the
 * only remote there is.
 */
static char *tracking_remote_for(struct repo *r, const char *name)
{
	struct remote_list rl = REMOTE_LIST_INIT;
	char *found = NULL;
	size_t i;
	int pass;

	remotes_of(r, &rl);
	for (pass = 0; pass < 2 && !found; pass++) {
		for (i = 0; i < rl.nr && !found; i++) {
			int is_origin = !strcmp(rl.e[i].name, "origin");
			char *ref;

			if ((pass == 0) != is_origin)
				continue;
			ref = xstrfmt("refs/remotes/%s/%s", rl.e[i].name, name);
			if (refs_exists(&r->refs, ref))
				found = xstrdup(rl.e[i].name);
			free(ref);
		}
	}
	remote_list_release(&rl);
	return found;
}

static int switch_to(struct repo *r, const char *rev, int force)
{
	oid_t oid, tree;
	char *branch = NULL;
	int was_branch = 0;
	int created = 0;                /* the branch came from the DWIM */
	char *tracking = NULL;
	char *prev;                     /* where HEAD was, for the record */
	oid_t old_head;
	int had_head;

	/*
	 * The DWIM has to run before the revision lookup, because `x` is not
	 * a revision: there is no refs/heads/x and no refs/remotes/x, only
	 * refs/remotes/<remote>/x, so resolving first would die with "not a
	 * valid object name".  Bare names only -- `switch origin/x` names a
	 * remote-tracking ref and detaches, as it does in git.
	 */
	if (!strchr(rev, '/')) {
		char *ref = branch_ref(rev);

		if (!refs_exists(&r->refs, ref))
			tracking = tracking_remote_for(r, rev);
		free(ref);
	}

	if (tracking) {
		char *remote_ref = xstrfmt("refs/remotes/%s/%s", tracking, rev);

		if (refs_read(&r->refs, remote_ref, &oid) < 0) {
			free(tracking);
			tracking = NULL;        /* gone: fall back to a revision */
		}
		free(remote_ref);
	}

	if (!tracking && resolve_rev(r, rev, &oid) < 0)
		gp_die("not a valid object name: %s", rev);
	if (commit_peel(r, &oid, OBJ_COMMIT, &oid) < 0)
		gp_die("not a commit: %s", rev);

	{
		struct commit c = COMMIT_INIT;
		read_commit(r, &oid, &c);
		tree = c.tree;
		commit_release(&c);
	}

	/*
	 * The tree is checked before the branch is created, so a switch that a
	 * dirty work tree refuses leaves no half-made branch behind -- the same
	 * order git does it in.
	 */
	if (guard_worktree(r, &tree, force) < 0) {
		free(tracking);
		return 1;
	}

	prev = head_position(r);
	had_head = refs_head(&r->refs, &old_head) == 0;

	if (tracking) {
		char *ref = branch_ref(rev);
		char *msg = xstrfmt("branch: Created from %s/%s", tracking, rev);
		char *k = xstrfmt("branch.%s.remote", rev);
		char *m = xstrfmt("branch.%s.merge", rev);
		char *mv = xstrfmt("refs/heads/%s", rev);

		refs_write(&r->refs, ref, &oid);
		refs_reflog(&r->refs, ref, &null_oid, &oid, msg);
		repo_config_set(r, k, tracking, 0);
		repo_config_set(r, m, mv, 0);
		printf("branch '%s' set up to track '%s/%s'.\n", rev, tracking, rev);
		free(msg);
		free(k);
		free(m);
		free(mv);
		branch = ref;
		was_branch = 1;
		created = 1;
		free(tracking);
	}

	/* a branch of that name is switched to; anything else detaches */
	if (!was_branch) {
		char *ref = branch_ref(rev);
		if (refs_exists(&r->refs, ref)) {
			was_branch = 1;
			branch = xstrdup(ref);
		}
		free(ref);
	}

	if (was_branch) {
		refs_set_head(&r->refs, branch);
		printf(created ? "Switched to a new branch '%s'\n"
			       : "Switched to branch '%s'\n", rev);
	} else {
		char *short_oid = abbrev_oid(&oid);
		refs_set_head_detached(&r->refs, &oid);
		printf("HEAD is now at %s %s\n", short_oid, rev);
		free(short_oid);
	}
	reflog_switch(r, prev, rev, &old_head, had_head, &oid);
	free(prev);

	checkout_tree(r, &tree, 1, 1);
	free(branch);
	return 0;
}

int cmd_checkout(struct repo *r, int argc, char **argv)
{
	struct opts o;
	int i;

	opts_init(&o, argc, argv,
		   (const char *const[]){ "-b=", "-B=", "-f", NULL });

	/* -b <name>: create the branch at HEAD and switch to it */
	if (opts_value(&o, "-b") || opts_value(&o, "-B")) {
		const char *name = opts_value(&o, "-b") ? opts_value(&o, "-b")
						       : opts_value(&o, "-B");
		oid_t head;
		char *ref;

		if (refs_head(&r->refs, &head) < 0)
			gp_die("checkout -b: there is nothing to branch from yet");
		ref = branch_ref(name);
		if (refs_exists(&r->refs, ref)) {
			free(ref);
			/* the branch is already there: switch to it properly,
			 * which means checking its tree out and guarding the
			 * work tree first */
			return switch_to(r, name, opts_flag(&o, "-f"));
		}
		{
			oid_t old;
			int had = refs_head(&r->refs, &old) == 0;
			char *prev = head_position(r);

			refs_write(&r->refs, ref, &head);
			refs_reflog(&r->refs, ref, &null_oid, &head, "branch: Created");
			refs_set_head(&r->refs, ref);
			reflog_switch(r, prev, name, &old, had, &head);
			free(prev);
		}
		printf("Switched to a new branch '%s'\n", name);
		free(ref);
		return 0;
	}

	/* checkout -- <path>... restores from the index */
	{
		int ddash = -1;
		for (i = 0; i < argc; i++)
			if (!strcmp(argv[i], "--")) {
				ddash = i;
				break;
			}
		if (ddash >= 0) {
			oid_t src_tree;
			int have_tree = 0;
			struct index_state src;
			int j;

			if (ddash > 0) {
				if (resolve_rev_tree(r, argv[ddash - 1], &src_tree) < 0)
					gp_die("not a tree: %s", argv[ddash - 1]);
				have_tree = 1;
			}

			memset(&src, 0, sizeof src);
			if (have_tree) {
				read_tree_into_index(r, &src, &src_tree, "");
			} else {
				index_read(&src, repo_index_path(r));
			}

			if (ddash + 1 >= argc) {
				/* no paths: restore the whole thing */
				restore_all_from_index(r, &src);
			} else {
				for (j = ddash + 1; j < argc; j++) {
					struct buf n;
					struct index_entry *e;
					buf_init(&n);
					path_normalize(argv[j], &n);
					e = index_get(&src, buf_cstr(&n));
					if (!e) {
						gp_error("pathspec '%s' did not "
							 "match any file", argv[j]);
						buf_release(&n);
						continue;
					}
					write_blob_to_worktree(r, buf_cstr(&n), &e->oid);
					printf("restored %s\n", buf_cstr(&n));
					buf_release(&n);
				}
			}
			index_release(&src);
			return 0;
		}
	}

	if (!opts_arg(&o, 0)) {
		gp_error("checkout: expected a branch, a commit, or -- <path>");
		return 1;
	}
	return switch_to(r, opts_arg(&o, 0), opts_flag(&o, "-f"));
}

/* write one blob out to the work tree, creating directories */
void write_blob_to_worktree(struct repo *r, const char *relpath, const oid_t *oid)
{
	struct buf b;
	char *full, *dir, *slash;

	buf_init(&b);
	if (odb_read(&r->odb, oid, NULL, &b) < 0) {
		buf_release(&b);
		return;
	}
	full = xstrfmt("%s/%s", r->root, relpath);
	dir = xstrdup(full);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir_p(dir);
	}
	free(dir);
	write_file(full, b.b, b.len);
	free(full);
	buf_release(&b);
}

void restore_all_from_index(struct repo *r, const struct index_state *ist)
{
	size_t i;

	for (i = 0; i < ist->nr; i++)
		write_blob_to_worktree(r, ist->e[i].path, &ist->e[i].oid);
	printf("restored %lu file(s) from the index\n", (unsigned long)ist->nr);
}

int cmd_switch(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *create;

	opts_init(&o, argc, argv, (const char *const[]){
		"-c=", "--create=", "-f", NULL });
	create = opts_value(&o, "-c") ? opts_value(&o, "-c")
				      : opts_value(&o, "--create");

	if (opts_flag(&o, "-c") || opts_flag(&o, "--create")) {
		const char *name = create ? create : opts_arg(&o, 0);
		oid_t head;
		char *ref;

		if (!name)
			gp_die("switch -c: expected a branch name");
		if (refs_head(&r->refs, &head) < 0)
			gp_die("switch -c: there is nothing to branch from yet");
		ref = branch_ref(name);
		if (refs_exists(&r->refs, ref))
			gp_die("a branch named '%s' already exists", name);
		{
			oid_t old;
			int had = refs_head(&r->refs, &old) == 0;
			char *prev = head_position(r);

			refs_write(&r->refs, ref, &head);
			/* the same birth line `checkout -b` writes: a branch
			 * made here is in the reflog afterwards like any other */
			refs_reflog(&r->refs, ref, &null_oid, &head,
				    "branch: Created");
			refs_set_head(&r->refs, ref);
			reflog_switch(r, prev, name, &old, had, &head);
			free(prev);
		}
		printf("Switched to a new branch '%s'\n", name);
		free(ref);
		return 0;
	}

	if (!opts_arg(&o, 0))
		gp_die("switch: expected a branch");
	return switch_to(r, opts_arg(&o, 0), opts_flag(&o, "-f"));
}

/* ------------------------------------------------------------------ */
/* merge                                                              */

/* depth of each commit from `tip`, recorded in an oid_array-parallel list */
static void collect_ancestors(struct repo *r, const oid_t *tip,
			      struct oid_array *out)
{
	struct oid_array frontier = OID_ARRAY_INIT;
	oid_array_append(&frontier, tip);
	while (frontier.nr) {
		oid_t cur = frontier.oid[--frontier.nr];
		struct commit c = COMMIT_INIT;
		size_t i;

		if (oid_array_contains(out, &cur))
			continue;
		oid_array_append(out, &cur);
		read_commit(r, &cur, &c);
		for (i = 0; i < c.parents.nr; i++)
			oid_array_append(&frontier, &c.parents.oid[i]);
		commit_release(&c);
	}
	oid_array_clear(&frontier);
}

/*
 * The best common ancestor available without a full generation-number
 * computation: collect both sides' ancestor sets and take a commit that
 * appears in both.  For the histories gitprompt is used on -- prompts
 * recorded one after another -- this is the right answer.
 */
static int merge_base(struct repo *r, const oid_t *a, const oid_t *b, oid_t *out)
{
	struct oid_array sa = OID_ARRAY_INIT, sb = OID_ARRAY_INIT;
	size_t i;
	int found = 0;

	collect_ancestors(r, a, &sa);
	collect_ancestors(r, b, &sb);

	for (i = 0; i < sa.nr; i++) {
		if (oid_array_contains(&sb, &sa.oid[i])) {
			*out = sa.oid[i];
			found = 1;
			break;
		}
	}
	oid_array_clear(&sa);
	oid_array_clear(&sb);
	return found ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* three-way merge of two trees against their base                     */

/*
 * Record a conflicted path the way git does: as index entries above stage 0
 * -- 1 for the merge base, 2 for our side, 3 for theirs.  A stage is left out
 * when that side has no version of the file, so a path one side deleted gets
 * two entries rather than three.  Any stage-0 entry the path had goes: an
 * unmerged path has stages and nothing else.
 */
static void merge_mark_unmerged(struct index_state *merged, const char *path,
				const struct index_entry *base,
				const struct index_entry *ours,
				const struct index_entry *theirs)
{
	const struct index_entry *src[4];
	unsigned stage;

	index_remove(merged, path);
	src[1] = base;
	src[2] = ours;
	src[3] = theirs;
	for (stage = 1; stage <= 3; stage++) {
		struct index_entry e;

		if (!src[stage])
			continue;
		e = *src[stage];
		e.path = (char *)path;
		e.stage = (u16)stage;
		index_add(merged, &e);
	}
}

static void merge_write(struct repo *r, const char *path, const void *data,
			size_t len)
{
	char *full = xstrfmt("%s/%s", r->root, path);
	char *dir = xstrdup(full);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir_p(dir);
	}
	free(dir);
	write_file(full, data, len);
	free(full);
}

/*
 * A path the rename handling below has already left in conflict.  The loops
 * that follow would otherwise read such a path as an ordinary change -- theirs
 * arriving at the new name, ours deleted at the old one -- and quietly undo
 * the conflict by adding it or removing it.
 */
static int merge_is_unmerged(const struct index_state *merged, const char *path)
{
	unsigned stage;

	for (stage = 1; stage <= 3; stage++)
		if (index_get_stage(merged, path, stage))
			return 1;
	return 0;
}

static void worktree_remove(struct repo *r, const char *path)
{
	char *full = xstrfmt("%s/%s", r->root, path);

	remove_file(full);
	free(full);
}

/*
 * Record a view's entry for a path under the name it moved to.  Nothing moves
 * when the view never had the path -- the move was the other side's, or this
 * side made it too -- or when the view already has something at the new name,
 * which is how a path renamed to two different places stays two paths.
 */
static int rename_follow(struct index_state *view, const char *from,
			 const char *to)
{
	struct index_entry *e = index_get(view, from);
	struct index_entry moved;

	if (!e || index_get(view, to))
		return 0;
	moved = *e;
	moved.path = (char *)to;
	index_remove(view, from);
	index_add(view, &moved);
	return 1;
}

/*
 * True when both sides moved one path, and to different places.  Such a rename
 * is not followed at all: the two names both survive as a delete and an add,
 * which is what a rename was before it was detected, and neither side's file
 * is lost to the other's name for it.
 */
static int renamed_two_ways(const struct rename_list *a,
			    const struct rename_list *b, const char *from)
{
	const struct rename_pair *pa = rename_by_from(a, from);
	const struct rename_pair *pb = rename_by_from(b, from);

	return pa && pb && strcmp(pa->to, pb->to) != 0;
}

void merge_trees(struct repo *r, const oid_t *base, const oid_t *ours,
		 const oid_t *theirs, struct merge_result *res,
		 struct index_state *merged, enum merge_favor favor,
		 const char *label)
{
	struct index_state bi, oi, ti;
	struct rename_list rts, rus;
	size_t i;

	memset(&bi, 0, sizeof bi);
	memset(&oi, 0, sizeof oi);
	memset(&ti, 0, sizeof ti);

	if (base)
		read_tree_into_index(r, &bi, base, "");
	read_tree_into_index(r, &oi, ours, "");
	read_tree_into_index(r, &ti, theirs, "");

	/*
	 * Follow the renames before merging anything, by recording the base's
	 * entry for a path that moved under the name the moving side gave it,
	 * inside the views the loops below read.  After the move, base, ours
	 * and theirs all name the file the same way, and merging its contents
	 * is the merge that was already there: an edit on the far side of a
	 * rename lands in the renamed file instead of looking like a
	 * modification of a file this side deleted.
	 *
	 * A file both sides moved to the same name needs nothing special --
	 * the second move finds nothing left to move -- so the three-way merge
	 * at the new name happens as it would have without the rename.  A move
	 * that met a deletion or a second name does not follow, and is left in
	 * conflict by the pass below; the moves made here are what that pass
	 * needs to record the stages.
	 */
	renames_between(&r->odb, &bi, &ti, &rts);
	renames_between(&r->odb, &bi, &oi, &rus);

	for (i = 0; i < rts.nr; i++) {
		const char *from = rts.e[i].from, *to = rts.e[i].to;

		/* a detected rename is a path the moving side lost and this one
		 * still has, so the base entry is always there to move */
		rename_follow(&bi, from, to);
		if (renamed_two_ways(&rts, &rus, from))
			continue;
		if (rename_follow(&oi, from, to)) {
			/*
			 * The work tree has to follow as well, and the loops
			 * below cannot do it: they write a file only when their
			 * side has something to put there, and a rename with no
			 * edit has nothing.  What is written here is what a merge
			 * of the contents then overwrites, when there is one.
			 */
			write_blob_to_worktree(r, to, &index_get(&oi, to)->oid);
			worktree_remove(r, from);
			res->files_changed++;
		} else if (!index_get(&oi, to)) {
			/* we deleted the old path: the pass below leaves the
			 * rename and the deletion to be settled by hand */
			continue;
		}
		printf("Renamed %s -> %s\n", from, to);
	}

	for (i = 0; i < rus.nr; i++) {
		const char *from = rus.e[i].from, *to = rus.e[i].to;

		if (renamed_two_ways(&rus, &rts, from))
			continue;
		if (!rename_follow(&bi, from, to))
			continue;       /* the other side moved it too */
		if (!index_get(&ti, from))
			continue;       /* they deleted it: the pass below */
		/*
		 * Their copy of the old path follows ours, so an edit they made
		 * to it is merged under the new name rather than read as a
		 * modification of a file we deleted.
		 */
		rename_follow(&ti, from, to);
		printf("Renamed %s -> %s\n", from, to);
	}

	/* start from our side and let their changes land on top of it */
	for (i = 0; i < oi.nr; i++)
		index_add(merged, &oi.e[i]);

	/*
	 * The two ways a move does not follow, both of which git also refuses
	 * to settle: it met a deletion, or it met a different name for the same
	 * file, and either way choosing would throw away a change somebody
	 * meant to make.  The paths come back unmerged, one stage each -- which
	 * is where the DU/UD/DD and AU/UA letters in `status` come from -- with
	 * the versions both sides need left in the work tree.
	 */
	for (i = 0; i < rts.nr; i++) {
		const char *from = rts.e[i].from, *to = rts.e[i].to;
		struct index_entry *b = index_get(&bi, to);

		if (renamed_two_ways(&rts, &rus, from)) {
			const struct rename_pair *ours = rename_by_from(&rus, from);

			printf("CONFLICT (rename/rename): %s is %s in HEAD and %s "
			       "in %s.\n", from, ours->to, to, label);
			/* the old path with a stage of its own, so that naming
			 * the file once is what resolving it means */
			merge_mark_unmerged(merged, from, b, NULL, NULL);
			merge_mark_unmerged(merged, ours->to, NULL,
					    index_get(&oi, ours->to), NULL);
			merge_mark_unmerged(merged, to, NULL, NULL,
					    index_get(&ti, to));
			write_blob_to_worktree(r, to, &index_get(&ti, to)->oid);
			res->conflicts++;
			continue;
		}
		if (index_get(&oi, to))
			continue;       /* followed above, or merged below */
		printf("CONFLICT (rename/delete): %s renamed to %s in %s, but "
		       "deleted in HEAD.  Version %s of %s left in tree.\n",
		       from, to, label, label, to);
		write_blob_to_worktree(r, to, &index_get(&ti, to)->oid);
		merge_mark_unmerged(merged, to, b, NULL, index_get(&ti, to));
		res->conflicts++;
	}

	for (i = 0; i < rus.nr; i++) {
		const char *from = rus.e[i].from, *to = rus.e[i].to;

		if (renamed_two_ways(&rus, &rts, from) || index_get(&ti, to))
			continue;       /* reported above, or merged below */
		printf("CONFLICT (rename/delete): %s renamed to %s in HEAD, but "
		       "deleted in %s.  Version HEAD of %s left in tree.\n",
		       from, to, label, to);
		merge_mark_unmerged(merged, to, index_get(&bi, to),
				    index_get(&oi, to), NULL);
		res->conflicts++;
	}

	for (i = 0; i < ti.nr; i++) {
		struct index_entry *b, *o;

		if (merge_is_unmerged(merged, ti.e[i].path))
			continue;
		b = base ? index_get(&bi, ti.e[i].path) : NULL;
		o = index_get(&oi, ti.e[i].path);

		/*
		 * Whose change this is decides whether it can be taken
		 * quietly.  With no common base the file was added -- by them
		 * alone, or by both of us, in which case identical contents are
		 * the same addition and differing contents are a real conflict.
		 * A path only one side touched is never a conflict: taking it
		 * is the whole point of merging.
		 */
		if (!b) {
			if (!o || oid_equal(&o->oid, &ti.e[i].oid)) {
				write_blob_to_worktree(r, ti.e[i].path,
						       &ti.e[i].oid);
				index_add(merged, &ti.e[i]);
				res->files_changed++;
				continue;
			}
		} else {
			int theirs_changed = !oid_equal(&b->oid, &ti.e[i].oid);
			int ours_changed = !o || !oid_equal(&b->oid, &o->oid);

			if (!theirs_changed)
				continue;
			if (!ours_changed) {
				write_blob_to_worktree(r, ti.e[i].path,
						       &ti.e[i].oid);
				index_add(merged, &ti.e[i]);
				res->files_changed++;
				continue;
			}
		}

		{
			struct buf ours_text, theirs_text, base_text, content;
			int conflicted;

			buf_init(&ours_text);
			buf_init(&theirs_text);
			buf_init(&base_text);
			buf_init(&content);
			odb_read(&r->odb, &ti.e[i].oid, NULL, &theirs_text);

			if (o) {
				/* both sides changed the file: merge the contents,
				 * which is a conflict only where they overlap */
				if (b)
					odb_read(&r->odb, &b->oid, NULL, &base_text);
				odb_read(&r->odb, &o->oid, NULL, &ours_text);
				printf("Auto-merging %s\n", ti.e[i].path);
				conflicted = merge3(&base_text, &ours_text,
						    &theirs_text, favor, "HEAD", label,
						    &content);
				if (conflicted)
					printf("CONFLICT (%s): Merge conflict in %s\n",
					       b ? "content" : "add/add",
					       ti.e[i].path);
			} else {
				printf("CONFLICT (modify/delete): %s deleted in "
				       "HEAD and modified in %s.  Version %s of "
				       "%s left in tree.\n", ti.e[i].path, label,
				       label, ti.e[i].path);
				buf_add(&content, theirs_text.b, theirs_text.len);
				conflicted = 1;
			}

			buf_release(&ours_text);
			buf_release(&base_text);
			buf_release(&theirs_text);

			merge_write(r, ti.e[i].path, content.b, content.len);
			if (!conflicted) {
				/*
				 * The merged text is a new object; the entry that
				 * records it is ours, so the mode and the stat block
				 * stay ours and only the id moves.
				 */
				struct index_entry e = *o;

				odb_write(&r->odb, OBJ_BLOB, content.b, content.len,
					  &e.oid);
				e.path = ti.e[i].path;
				e.stage = 0;
				index_add(merged, &e);
				res->files_changed++;
			} else {
				res->conflicts++;
				merge_mark_unmerged(merged, ti.e[i].path, b, o,
						    &ti.e[i]);
			}
			buf_release(&content);
		}
	}

	/* deletions on their side */
	for (i = 0; i < oi.nr; i++) {
		struct index_entry *b;

		/* a path already left unmerged is a conflict decided above,
		 * and this loop's reading of it -- a file we have and they do
		 * not -- is exactly the deletion that must not be taken */
		if (merge_is_unmerged(merged, oi.e[i].path))
			continue;
		b = base ? index_get(&bi, oi.e[i].path) : NULL;
		if (index_get(&ti, oi.e[i].path))
			continue;
		if (b && !oid_equal(&b->oid, &oi.e[i].oid)) {
			printf("CONFLICT (modify/delete): %s deleted in %s and "
			       "modified in HEAD.  Version HEAD of %s left in "
			       "tree.\n", oi.e[i].path, label, oi.e[i].path);
			res->conflicts++;
			merge_mark_unmerged(merged, oi.e[i].path, b, &oi.e[i], NULL);
			continue;
		}
		if (b) {
			worktree_remove(r, oi.e[i].path);
			index_remove(merged, oi.e[i].path);
			res->files_changed++;
		}
	}

	rename_list_release(&rts);
	rename_list_release(&rus);
	index_release(&bi);
	index_release(&oi);
	index_release(&ti);
}

/* ------------------------------------------------------------------ */
/* merge state                                                        */
/*
 * A merge that stops on a conflict has to leave something behind saying so.
 * Without it, three things go wrong: --abort cannot put the tree back, the
 * commit that concludes the merge is recorded with one parent (so the join
 * vanishes from the history), and a second merge quietly starts on top of the
 * half-finished first one.  git keeps MERGE_HEAD for this; so does gitprompt.
 */

int merge_in_progress(struct repo *r, oid_t *other)
{
	char *path = repo_git_path(r, "MERGE_HEAD");
	struct buf b;
	char *hex;
	int rc = 0;

	buf_init(&b);
	if (read_file(path, &b) < 0)
		rc = -1;
	else {
		size_t n = 0;
		while (n < b.len && !isspace((unsigned char)b.b[n]))
			n++;
		if (n != GP_SHA1_HEXSZ) {
			rc = -1;
		} else {
			hex = xstrndup((const char *)b.b, n);
			if (oid_parse(other, hex) < 0)
				rc = -1;
			free(hex);
		}
	}
	buf_release(&b);
	free(path);
	return rc;
}

void merge_state_write(struct repo *r, const oid_t *other, const char *subject)
{
	char *path = repo_git_path(r, "MERGE_HEAD");
	char hex[GP_SHA1_HEXSZ + 1];
	struct buf b;

	oid_hex(other, hex);
	buf_init(&b);
	buf_addf(&b, "%s\n", hex);
	write_file(path, b.b, b.len);
	buf_release(&b);
	free(path);

	if (subject) {
		path = repo_git_path(r, "MERGE_MSG");
		buf_init(&b);
		buf_addf(&b, "Merge %s\n", subject);
		write_file(path, b.b, b.len);
		buf_release(&b);
		free(path);
	}
}

void merge_state_clear(struct repo *r)
{
	char *path = repo_git_path(r, "MERGE_HEAD");
	remove(path);
	free(path);
	path = repo_git_path(r, "MERGE_MSG");
	remove(path);
	free(path);
}

/* the message a merge left behind, so a bare `commit` can conclude it */
int merge_message(struct repo *r, struct buf *out)
{
	char *path = repo_git_path(r, "MERGE_MSG");
	int rc;

	rc = read_file(path, out);
	free(path);
	return rc == 0 && out->len;
}

/* the text a merge is named by: an explicit -m wins, else the revision the
 * user named, which for the common `merge <branch>` case is the branch */
static const char *merge_subject(const struct opts *o)
{
	const char *m = opts_value(o, "-m");

	if (m)
		return m;
	m = opts_value(o, "--message");
	if (m)
		return m;
	return opts_arg(o, 0);
}

int cmd_merge(struct repo *r, int argc, char **argv)
{
	struct opts o;
	oid_t target, head, head_tree, target_tree, base, base_tree;
	struct commit tc = COMMIT_INIT;
	enum merge_favor favor = MERGE_FAVOR_NONE;
	const char *label;
	int squash, no_ff;

	opts_init(&o, argc, argv, (const char *const[]){
		"-m=", "--message=", "-X=", "--strategy-option=", "--squash",
		"--no-ff", "--ff-only", "--no-commit", "--abort", NULL });

	squash = opts_flag(&o, "--squash");
	no_ff = opts_flag(&o, "--no-ff");
	{
		const char *x = opts_value(&o, "-X") ? opts_value(&o, "-X")
				     : opts_value(&o, "--strategy-option");

		if (x) {
			if (!strcmp(x, "ours"))
				favor = MERGE_FAVOR_OURS;
			else if (!strcmp(x, "theirs"))
				favor = MERGE_FAVOR_THEIRS;
			else {
				gp_error("merge: unknown strategy option '%s'\n"
					 "hint: the ones there are are 'ours' and "
					 "'theirs'", x);
				return 1;
			}
		}
	}
	if (no_ff && opts_flag(&o, "--ff-only"))
		gp_die("You cannot combine --no-ff with --ff-only.");

	if (opts_flag(&o, "--abort")) {
		oid_t other;

		if (merge_in_progress(r, &other) < 0) {
			gp_error("merge --abort: there is no merge to abort");
			return 1;
		}
		/* put the tree back the way HEAD has it, then drop the state */
		if (refs_head(&r->refs, &head) == 0) {
			struct commit hc = COMMIT_INIT;
			read_commit(r, &head, &hc);
			checkout_tree(r, &hc.tree, 1, 1);
			commit_release(&hc);
		}
		merge_state_clear(r);
		printf("Merge aborted.\n");
		return 0;
	}

	if (!opts_arg(&o, 0)) {
		/* merging the configured upstream would go here */
		gp_error("merge: expected a revision to merge");
		return 1;
	}

	{
		oid_t pending;
		if (merge_in_progress(r, &pending) == 0)
			gp_die("You have not concluded your merge (MERGE_HEAD exists)\n"
			       "hint: resolve the conflicts and commit, or run "
			       "'gitprompt merge --abort'");
	}

	/*
	 * A merge onto an unmerged index has no side to merge from: the stages
	 * are a question the last merge asked and nobody has answered.  git
	 * refuses for the same reason, and so that `--squash` -- which leaves
	 * no MERGE_HEAD to notice -- is not a way to pile one merge on another.
	 */
	{
		struct index_state ist;

		memset(&ist, 0, sizeof ist);
		index_read(&ist, repo_index_path(r));
		if (index_has_unmerged(&ist)) {
			gp_error("Merging is not possible because you have unmerged "
				 "files.\n"
				 "hint: fix them up in the work tree, and then use "
				 "'gitprompt add/rm <file>'\n"
				 "hint: as appropriate to mark resolution and make "
				 "a commit.");
			index_release(&ist);
			gp_die("Exiting because of an unresolved conflict.");
		}
		index_release(&ist);
	}

	if (refs_head(&r->refs, &head) < 0)
		gp_die("merge: HEAD has no commits yet");
	label = opts_arg(&o, 0);
	if (resolve_rev(r, label, &target) < 0)
		gp_die("merge: unknown revision: %s", label);
	if (commit_peel(r, &target, OBJ_COMMIT, &target) < 0)
		gp_die("merge: not a commit");

	/* --ff-only is checked below, once the two up-to-date cases are out of
	 * the way: an already-merged branch is not "not possible to
	 * fast-forward", it is simply up to date */

	read_commit(r, &target, &tc);
	target_tree = tc.tree;
	{
		struct commit hc = COMMIT_INIT;
		read_commit(r, &head, &hc);
		head_tree = hc.tree;
		commit_release(&hc);
	}

	if (oid_equal(&head, &target)) {
		printf("Already up to date.\n");
		commit_release(&tc);
		return 0;
	}

	if (is_ancestor(r, &target, &head)) {
		printf("Already up to date.\n");
		commit_release(&tc);
		return 0;
	}

	/*
	 * Fast-forward: move the ref and check the tree out, no new commit.
	 * --no-ff asks for a merge commit anyway, and --squash wants the work
	 * staged without HEAD moving, so both go on to the three-way merge.
	 */
	if (!no_ff && !squash && is_ancestor(r, &head, &target)) {
		char *ref = refs_head_target(&r->refs);
		if (ref) {
			refs_write(&r->refs, ref, &target);
			refs_reflog(&r->refs, ref, &head, &target,
				    "merge: fast-forward");
			refs_reflog_head(&r->refs, ref, &head, &target,
					 "merge: fast-forward");
			free(ref);
		} else {
			refs_set_head_detached(&r->refs, &target);
			refs_reflog_head(&r->refs, NULL, &head, &target,
					 "merge: fast-forward");
		}
		checkout_tree(r, &target_tree, 1, 1);
		printf("Updating %s..%s\nFast-forward\n", abbrev_oid(&head),
		       abbrev_oid(&target));
		commit_release(&tc);
		return 0;
	}

	/* the histories have diverged: this would be a real merge commit */
	if (opts_flag(&o, "--ff-only")) {
		gp_error("not possible to fast-forward, aborting");
		commit_release(&tc);
		return 1;
	}

	if (merge_base(r, &head, &target, &base) < 0)
		gp_die("merge: no common ancestor");

	{
		/*
		 * The base comes back as a commit, and the three-way merge is
		 * defined over trees: the two sides were peeled already, so the
		 * base has to be peeled here too or the merge would try to read
		 * a commit as a tree.
		 */
		struct commit bc = COMMIT_INIT;
		read_commit(r, &base, &bc);
		base_tree = bc.tree;
		commit_release(&bc);
	}

	{
		struct merge_result res;
		struct index_state mindex;
		memset(&res, 0, sizeof res);
		memset(&mindex, 0, sizeof mindex);
		merge_trees(r, &base_tree, &head_tree, &target_tree, &res, &mindex,
			    favor, label);

		/*
		 * The merged result belongs in the index whether or not the
		 * merge is finished now, and a conflicted path is in it as its
		 * unmerged stages.
		 */
		index_write(&mindex, repo_index_path(r));

		if (res.conflicts) {
			/*
			 * Leave MERGE_HEAD behind so --abort can undo this, and
			 * so the concluding commit records both parents.  A
			 * squashed merge leaves none: it is not a merge to
			 * finish, only its changes.
			 */
			if (squash)
				printf("Squash commit -- not updating HEAD\n");
			else
				merge_state_write(r, &target, merge_subject(&o));
			gp_error("Automatic merge failed; fix conflicts and then "
				 "commit the result.\n"
				 "hint: resolve them, then 'gitprompt add' and "
				 "'gitprompt commit'%s", squash ? "" :
				 ", or throw the merge away with "
				 "'gitprompt merge --abort'");
			index_release(&mindex);
			commit_release(&tc);
			return 1;
		}
		if (squash) {
			/*
			 * Staged and not recorded: no MERGE_HEAD, so the commit
			 * that follows has one parent, and HEAD has not moved.
			 */
			printf("Squash commit -- not updating HEAD\n"
			       "Automatic merge went well; stopped before "
			       "committing as requested\n");
			index_release(&mindex);
			commit_release(&tc);
			return 0;
		}
		if (opts_flag(&o, "--no-commit")) {
			/* staged but not recorded: MERGE_HEAD is what tells the
			 * next `commit` to give the result two parents */
			merge_state_write(r, &target, merge_subject(&o));
			printf("Merge prepared; commit it yourself.\n");
			index_release(&mindex);
			commit_release(&tc);
			return 0;
		}
		/* write the merged tree and record a two-parent commit */

		{
			struct commit c = COMMIT_INIT;
			struct buf ident, body;
			oid_t tree, out;
			char hex[GP_SHA1_HEXSZ + 1];
			char *ref;

			write_tree_from_index(r, &mindex, &tree);

			/*
			 * A merge commit carries the prompts it brings in, the
			 * same way any commit carries what it changed: against
			 * its first parent, which is the branch it was made on.
			 */
			collect_commit_prompts(r, &mindex, &head_tree, 1, &c);
			index_release(&mindex);
			{
				char *sess = repo_current_session(r);

				if (sess)
					c.session = sess;
			}

			buf_init(&ident);
			buf_init(&body);
			repo_ident_with_time(r, &ident);
			c.tree = tree;
			oid_array_append(&c.parents, &head);
			oid_array_append(&c.parents, &target);
			c.author = xstrdup(buf_cstr(&ident));
			c.committer = xstrdup(buf_cstr(&ident));
			c.message = xstrfmt("Merge %s\n", merge_subject(&o));
			commit_format(&c, &body);
			odb_write(&r->odb, OBJ_COMMIT, body.b, body.len, &out);

			ref = refs_head_target(&r->refs);
			if (ref) {
				refs_write(&r->refs, ref, &out);
				refs_reflog(&r->refs, ref, &head, &out, "merge");
				refs_reflog_head(&r->refs, ref, &head, &out,
						 "merge");
				free(ref);
			} else {
				refs_set_head_detached(&r->refs, &out);
				refs_reflog_head(&r->refs, NULL, &head, &out,
						 "merge");
			}
			oid_hex(&out, hex);
			hex[7] = '\0';
			printf("Merge made by the three-way merge: %s (%lu file(s) "
			       "updated)\n", hex,
			       (unsigned long)res.files_changed);
			commit_release(&c);
			buf_release(&ident);
			buf_release(&body);
		}
	}

	commit_release(&tc);
	return 0;
}

/* ------------------------------------------------------------------ */
/* tag                                                                */

static void tag_list_cb(const char *name, const oid_t *oid, void *ud)
{
	const char *shortname = name;
	char hex[GP_SHA1_HEXSZ + 1];
	(void)ud;
	if (!strncmp(shortname, "refs/tags/", 10))
		shortname += 10;
	oid_hex(oid, hex);
	hex[7] = '\0';
	printf("  %s %s\n", shortname, hex);
}

int cmd_tag(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *del;

	opts_init(&o, argc, argv, (const char *const[]){
		"-d=", "--delete=", "-m=", "-l", "--list", NULL });
	del = opts_value(&o, "-d") ? opts_value(&o, "-d")
				   : opts_value(&o, "--delete");

	if (opts_flag(&o, "-d") || opts_flag(&o, "--delete")) {
		const char *name = del ? del : opts_arg(&o, 0);
		char *ref;
		if (!name)
			gp_die("tag -d: expected a tag name");
		ref = xstrfmt("refs/tags/%s", name);
		if (!refs_exists(&r->refs, ref))
			gp_die("tag '%s' not found", name);
		refs_delete(&r->refs, ref);
		printf("Deleted tag '%s'\n", name);
		free(ref);
		return 0;
	}

	if (opts_flag(&o, "-l") || opts_flag(&o, "--list") || !opts_arg(&o, 0)) {
		refs_list(&r->refs, "refs/tags/", tag_list_cb, NULL);
		return 0;
	}

	{
		const char *name = opts_arg(&o, 0);
		const char *rev = opts_arg(&o, 1);
		oid_t target;
		char *ref;

		if (rev) {
			if (resolve_rev(r, rev, &target) < 0)
				gp_die("Failed to resolve '%s' as a valid revision.",
				       rev);
		} else if (refs_head(&r->refs, &target) < 0) {
			gp_die("Failed to resolve 'HEAD' -- no commits yet");
		}
		if (commit_peel(r, &target, OBJ_COMMIT, &target) < 0)
			gp_die("not a commit: %s", rev ? rev : "HEAD");

		ref = xstrfmt("refs/tags/%s", name);
		if (refs_exists(&r->refs, ref))
			gp_die("tag '%s' already exists", name);
		if (refs_check_name(ref) < 0)
			gp_die("'%s' is not a valid tag name", name);

		if (opts_value(&o, "-m")) {
			/* an annotated tag: a real tag object, which
			 * `cat-file -p` and `rev-parse ^{commit}` can peel */
			struct buf ident, body;
			oid_t tag_oid;
			char hex[GP_SHA1_HEXSZ + 1];

			buf_init(&ident);
			buf_init(&body);
			repo_ident_with_time(r, &ident);
			oid_hex(&target, hex);
			buf_addf(&body, "object %s\n", hex);
			buf_addstr(&body, "type commit\n");
			buf_addf(&body, "tag %s\n", name);
			buf_addf(&body, "tagger %s\n", buf_cstr(&ident));
			buf_addch(&body, '\n');
			buf_addstr(&body, opts_value(&o, "-m"));
			buf_addch(&body, '\n');

			if (odb_write(&r->odb, OBJ_TAG, body.b, body.len,
				      &tag_oid) < 0)
				gp_die("tag: cannot write the tag object");
			refs_write(&r->refs, ref, &tag_oid);
			printf("Tagged %s as %s (annotated)\n", rev ? rev : "HEAD",
			       name);
			buf_release(&ident);
			buf_release(&body);
		} else {
			refs_write(&r->refs, ref, &target);
			printf("Tagged %s as %s\n", rev ? rev : "HEAD", name);
		}
		free(ref);
	}
	return 0;
}
