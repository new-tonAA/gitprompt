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
	static const char *const takes[] = { "-d", "--delete", "-m", "--move" };
	struct opts o;
	char *cur = current_branch(r);
	const char *del, *move;

	opts_init(&o, argc, argv, takes, 4);
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
		if (cur && !strcmp(cur, from))
			refs_set_head(&r->refs, new_ref);
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

static int switch_to(struct repo *r, const char *rev, int force)
{
	oid_t oid, tree;
	char *branch = NULL;
	int was_branch = 0;

	if (resolve_rev(r, rev, &oid) < 0)
		gp_die("not a valid object name: %s", rev);
	if (commit_peel(r, &oid, OBJ_COMMIT, &oid) < 0)
		gp_die("not a commit: %s", rev);

	{
		struct commit c = COMMIT_INIT;
		read_commit(r, &oid, &c);
		tree = c.tree;
		commit_release(&c);
	}

	if (guard_worktree(r, &tree, force) < 0)
		return 1;

	/* a branch of that name is switched to; anything else detaches */
	{
		char *ref = branch_ref(rev);
		if (refs_exists(&r->refs, ref)) {
			was_branch = 1;
			branch = xstrdup(ref);
		}
		free(ref);
	}

	if (was_branch) {
		refs_set_head(&r->refs, branch);
		printf("Switched to branch '%s'\n", rev);
	} else {
		char *short_oid = abbrev_oid(&oid);
		refs_set_head_detached(&r->refs, &oid);
		printf("HEAD is now at %s %s\n", short_oid, rev);
		free(short_oid);
	}

	checkout_tree(r, &tree, 1, 1);
	free(branch);
	return 0;
}

int cmd_checkout(struct repo *r, int argc, char **argv)
{
	static const char *const takes[] = { "-b", "-B", "--source" };
	struct opts o;
	int i;

	opts_init(&o, argc, argv, takes, 3);

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
		refs_write(&r->refs, ref, &head);
		refs_reflog(&r->refs, ref, &null_oid, &head, "branch: Created");
		refs_set_head(&r->refs, ref);
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
	static const char *const takes[] = { "-c", "--create", "-C",
					     "--force-create" };
	struct opts o;
	const char *create;

	opts_init(&o, argc, argv, takes, 4);
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
		refs_write(&r->refs, ref, &head);
		refs_set_head(&r->refs, ref);
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

struct merge_result {
	int conflicts;
	size_t files_changed;
	struct strlist conflict_paths;
};

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

static void merge_trees(struct repo *r, const oid_t *base, const oid_t *ours,
			const oid_t *theirs, struct merge_result *res,
			struct index_state *merged)
{
	struct index_state bi, oi, ti;
	size_t i;

	memset(&bi, 0, sizeof bi);
	memset(&oi, 0, sizeof oi);
	memset(&ti, 0, sizeof ti);

	if (base)
		read_tree_into_index(r, &bi, base, "");
	read_tree_into_index(r, &oi, ours, "");
	read_tree_into_index(r, &ti, theirs, "");

	/* start from our side and let their changes land on top of it */
	for (i = 0; i < oi.nr; i++)
		index_add(merged, &oi.e[i]);

	for (i = 0; i < ti.nr; i++) {
		struct index_entry *b = base ? index_get(&bi, ti.e[i].path) : NULL;
		struct index_entry *o = index_get(&oi, ti.e[i].path);

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
			struct buf theirs_only, mine, merged;

			buf_init(&theirs_only);
			buf_init(&merged);
			buf_init(&mine);
			odb_read(&r->odb, &ti.e[i].oid, NULL, &theirs_only);

			if (o) {
				odb_read(&r->odb, &o->oid, NULL, &mine);
				buf_addstr(&merged, "<<<<<<< ours\n");
				buf_add(&merged, mine.b, mine.len);
				if (merged.len && merged.b[merged.len - 1] != '\n')
					buf_addch(&merged, '\n');
				buf_addstr(&merged, "=======\n");
			} else {
				printf("CONFLICT (modify/delete): %s deleted by "
				       "us, modified by them\n", ti.e[i].path);
			}
			buf_add(&merged, theirs_only.b, theirs_only.len);
			if (merged.len && merged.b[merged.len - 1] != '\n')
				buf_addch(&merged, '\n');
			if (o)
				buf_addstr(&merged, ">>>>>>> theirs\n");

			merge_write(r, ti.e[i].path, merged.b, merged.len);
			if (o)
				printf("CONFLICT (content): %s\n", ti.e[i].path);
			res->conflicts++;
			strlist_push(&res->conflict_paths, ti.e[i].path);

			buf_release(&mine);
			buf_release(&theirs_only);
			buf_release(&merged);
		}
	}

	/* deletions on their side */
	for (i = 0; i < oi.nr; i++) {
		struct index_entry *b = base ? index_get(&bi, oi.e[i].path) : NULL;
		if (index_get(&ti, oi.e[i].path))
			continue;
		if (b && !oid_equal(&b->oid, &oi.e[i].oid)) {
			printf("CONFLICT (modify/delete): %s\n", oi.e[i].path);
			res->conflicts++;
			strlist_push(&res->conflict_paths, oi.e[i].path);
			continue;
		}
		if (b) {
			char *full = xstrfmt("%s/%s", r->root, oi.e[i].path);
			remove_file(full);
			free(full);
			index_remove(merged, oi.e[i].path);
			res->files_changed++;
		}
	}

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
	path = repo_git_path(r, "MERGE_CONFLICTS");
	remove(path);
	free(path);
}

/*
 * The paths a merge left in conflict, one per line.  git records this in the
 * index by giving each conflicted path three entries; gitprompt's index is a
 * plain path-to-object map, so the list lives beside MERGE_HEAD instead.  A
 * path leaves the list when it is staged, which is the same thing `git add`
 * does to resolve it.
 */
static void merge_conflicts_read(struct repo *r, struct strlist *out)
{
	char *path = repo_git_path(r, "MERGE_CONFLICTS");
	struct buf b;
	size_t i = 0;

	buf_init(&b);
	if (read_file(path, &b) == 0)
		while (i < b.len) {
			size_t start = i;
			while (i < b.len && b.b[i] != '\n')
				i++;
			if (i > start)
				strlist_push(out, xstrndup((const char *)b.b + start,
							   i - start));
			if (i < b.len)
				i++;
		}
	buf_release(&b);
	free(path);
}

static void merge_conflicts_write(struct repo *r, const struct strlist *paths)
{
	char *path;
	struct buf b;
	size_t i;

	path = repo_git_path(r, "MERGE_CONFLICTS");
	if (!paths->nr) {
		remove(path);
		free(path);
		return;
	}
	buf_init(&b);
	for (i = 0; i < paths->nr; i++)
		buf_addf(&b, "%s\n", paths->v[i]);
	write_file(path, b.b, b.len);
	buf_release(&b);
	free(path);
}

/* the conflicted paths, as a NULL-terminated array safe to hand to another file */
char **merge_conflicts_list(struct repo *r, size_t *nr)
{
	struct strlist l = { NULL, 0, 0 };
	char **out;
	size_t i;

	merge_conflicts_read(r, &l);
	*nr = l.nr;
	if (!l.nr) {
		free(l.v);
		return NULL;
	}
	out = xcalloc(l.nr + 1, sizeof(*out));
	for (i = 0; i < l.nr; i++)
		out[i] = l.v[i];       /* the strings move into the new array */
	free(l.v);
	return out;
}

void merge_conflicts_free(char **v)
{
	size_t i;

	if (!v)
		return;
	for (i = 0; v[i]; i++)
		free(v[i]);
	free(v);
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

/* staging a path is how a conflict is declared resolved, exactly as in git */
void merge_conflicts_resolve(struct repo *r, const char *path)
{
	struct strlist keep = { NULL, 0, 0 };
	struct strlist cur = { NULL, 0, 0 };
	size_t i;

	merge_conflicts_read(r, &cur);
	for (i = 0; i < cur.nr; i++)
		if (strcmp(cur.v[i], path))
			strlist_push(&keep, cur.v[i]);
	/* only rewrite the file when something actually changed, so that
	 * staging an unrelated path does not disturb an unfinished merge.
	 * MERGE_HEAD stays: the commit that concludes the merge needs it to
	 * record the second parent, and --abort needs it to undo the merge. */
	if (keep.nr != cur.nr)
		merge_conflicts_write(r, &keep);
	strlist_release(&keep);
	strlist_release(&cur);
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
	static const char *const takes[] = { "-m", "--message" };
	struct opts o;
	oid_t target, head, head_tree, target_tree, base, base_tree;
	struct commit tc = COMMIT_INIT;

	opts_init(&o, argc, argv, takes, 2);

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

	if (refs_head(&r->refs, &head) < 0)
		gp_die("merge: HEAD has no commits yet");
	if (resolve_rev(r, opts_arg(&o, 0), &target) < 0)
		gp_die("merge: unknown revision: %s", opts_arg(&o, 0));
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

	/* fast-forward: move the ref and check the tree out, no new commit */
	if (is_ancestor(r, &head, &target)) {
		char *ref = refs_head_target(&r->refs);
		if (ref) {
			refs_write(&r->refs, ref, &target);
			refs_reflog(&r->refs, ref, &head, &target,
				    "merge: fast-forward");
			free(ref);
		} else {
			refs_set_head_detached(&r->refs, &target);
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
		merge_trees(r, &base_tree, &head_tree, &target_tree, &res, &mindex);

		/*
		 * The merged result belongs in the index whether or not the
		 * merge is finished now: the half-merged paths stay as ours, so
		 * a `gitprompt add` of a resolved file has something to replace.
		 */
		index_write(&mindex, repo_index_path(r));

		if (res.conflicts) {
			/* leave MERGE_HEAD behind so --abort can undo this, and
			 * so the concluding commit records both parents */
			merge_state_write(r, &target, merge_subject(&o));
			merge_conflicts_write(r, &res.conflict_paths);
			gp_error("Automatic merge failed; %d conflict(s) left in "
				 "the work tree.\n"
				 "hint: resolve them, then 'gitprompt add' and "
				 "'gitprompt commit', or throw the merge away with "
				 "'gitprompt merge --abort'", res.conflicts);
			strlist_release(&res.conflict_paths);
			index_release(&mindex);
			commit_release(&tc);
			return 1;
		}
		strlist_release(&res.conflict_paths);
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
			index_release(&mindex);

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
				free(ref);
			} else {
				refs_set_head_detached(&r->refs, &out);
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
	static const char *const takes[] = { "-d", "--delete", "-m" };
	struct opts o;
	const char *del;

	opts_init(&o, argc, argv, takes, 3);
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
