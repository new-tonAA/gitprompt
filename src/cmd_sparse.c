/*
 * cmd_sparse.c - a work tree that holds only part of the index.
 *
 * Sparse checkout does not make the index smaller: every path is still tracked,
 * still committed, and still there for `diff`, `ls-tree` and a clone to read.
 * What changes is the work tree, and the marker for that is the index's
 * skip-worktree bit -- "this path is tracked, but this work tree is not expected
 * to have it".  A checkout leaves such a path out, `status` does not report it
 * missing, and nothing walks into it, so what is on disk is a subset of what is
 * in the index and the difference is not a change.
 *
 * The patterns are git's, in git's file: `<store>/info/sparse-checkout`, one
 * pattern to a line, blank lines and `#` comments ignored, in the same syntax an
 * ignore file uses.  They are read from the root of the work tree, which is the
 * one place this differs from `.gitignore`: a pattern without a leading slash is
 * still about the root, not about the name wherever it happens to appear.  A
 * line beginning with `!` excludes what it matches, a line ending in `/` is
 * about a directory only, and a pattern that names a directory covers everything
 * under it.  The last line that matches anything decides, and a path no line
 * matches is out -- which is why an empty pattern file means an empty work tree,
 * and why `init` writes one with something in it.
 */
#include "gp.h"

#include <stdlib.h>
#include <string.h>

struct splist {
	char **v;
	size_t nr, alloc;
};

static void spl_push(struct splist *l, const char *s, size_t n)
{
	if (l->nr == l->alloc) {
		l->alloc = l->alloc ? l->alloc * 2 : 16;
		l->v = xrealloc(l->v, l->alloc * sizeof(*l->v));
	}
	l->v[l->nr++] = xstrndup(s, n);
}

static void spl_release(struct splist *l)
{
	size_t i;

	for (i = 0; i < l->nr; i++)
		free(l->v[i]);
	free(l->v);
	l->v = NULL;
	l->nr = l->alloc = 0;
}

/* ------------------------------------------------------------------ */
/* the pattern list                                                    */

/*
 * The patterns are read once per repository per run, and remembered, because
 * every path a checkout or a status looks at asks the question and the answer
 * is the same file every time.  `set` and `disable` rewrite that file, so they
 * clear this before asking again.
 */
static char *cache_root;
static struct splist cache;
static int cache_enabled;

void sparse_forget(void)
{
	free(cache_root);
	cache_root = NULL;
	spl_release(&cache);
	cache_enabled = 0;
}

static char *sparse_file(struct repo *r)
{
	return xstrfmt("%s/info/sparse-checkout", r->gpdir);
}

static int config_truthy(const char *v)
{
	return v && (!strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "yes")
		     || !strcmp(v, "on"));
}

/*
 * Where the switch is read and written.  git treats sparse checkout as a
 * property of one work tree, so once `extensions.worktreeConfig` is on it keeps
 * `core.sparseCheckout` in `config.worktree` beside the work tree's index
 * rather than in the shared configuration -- and a repository git has run
 * `sparse-checkout` in has that turned on.  Writing to the shared file there
 * would leave git reading a switch nobody set, so the setting goes wherever git
 * would look for it.
 */
static char *sparse_config_file(struct repo *r)
{
	char *v = NULL;
	int per_worktree = 0;

	if (repo_config_get(r, "extensions.worktreeconfig", &v) == 0) {
		per_worktree = config_truthy(v);
		free(v);
	}
	if (per_worktree)
		return xstrfmt("%s/config.worktree", r->gpdir);
	return xstrfmt("%s/config", r->gpdir);
}

static void sparse_config_set(struct repo *r, int on)
{
	char *path = sparse_config_file(r);

	config_file_set(path, "core.sparsecheckout", on ? "true" : "false");
	free(path);
}

static void sparse_config_clear(struct repo *r)
{
	char *path = sparse_config_file(r);

	config_file_unset(path, "core.sparsecheckout");
	free(path);
}

static void sparse_load(struct repo *r)
{
	struct buf b;
	char *path;
	size_t i;

	if (cache_root && !strcmp(cache_root, r->gpdir))
		return;
	sparse_forget();

	cache_root = xstrdup(r->gpdir);
	{
		char *v = NULL;

		if (repo_config_get(r, "core.sparsecheckout", &v) != 0) {
			/*
			 * Not in the shared configuration, so look where a work
			 * tree keeps its own -- git's own `sparse-checkout` writes
			 * there once `extensions.worktreeConfig` is on.
			 */
			char *wt = xstrfmt("%s/config.worktree", r->gpdir);

			if (config_file_get(wt, "core.sparsecheckout", &v) != 0)
				v = NULL;
			free(wt);
		}
		cache_enabled = config_truthy(v);
		free(v);
	}
	if (!cache_enabled)
		return;

	path = sparse_file(r);
	buf_init(&b);
	if (read_file(path, &b) < 0) {
		buf_release(&b);
		free(path);
		return;
	}
	i = 0;
	while (i < b.len) {
		size_t start = i, n;

		while (i < b.len && b.b[i] != '\n')
			i++;
		n = i - start;
		if (i < b.len)
			i++;
		if (n && b.b[start + n - 1] == '\r')
			n--;
		if (!n || b.b[start] == '#')
			continue;
		spl_push(&cache, (const char *)b.b + start, n);
	}
	buf_release(&b);
	free(path);
}

/*
 * One pattern against one path, where `is_dir` says whether the path itself is
 * a directory -- the same path is asked twice, once as the file it is and once
 * as a directory on the way down to it, and a pattern ending in `/` only
 * matches in the second case.
 */
static int match_one(const char *pat, const char *rel, int is_dir)
{
	size_t n = strlen(pat);
	char *body;
	int dir_only = 0;
	int hit;

	/*
	 * A trailing slash says "a directory", and it is a statement about the path
	 * rather than part of the pattern: `/docs/` has to be matched as `docs` or
	 * the slash it ends in would have to appear in a path that cannot end in
	 * one.  The slash is stripped here and never reaches the matcher.
	 */
	if (n && pat[n - 1] == '/') {
		dir_only = 1;
		n--;
	}
	if (!n || (dir_only && !is_dir))
		return 0;
	/* a leading slash only says the pattern is about the root, which is the
	 * only place these are ever matched from */
	if (pat[0] == '/')
		body = xstrndup(pat + 1, n - 1);
	else
		body = xstrndup(pat, n);
	hit = path_match_root(body, rel);
	free(body);
	return hit;
}

static int included(const struct splist *sp, const char *rel)
{
	/*
	 * Excluded until a line says otherwise, and that is the whole default: a
	 * path no pattern names is not in the work tree, whether the list is empty,
	 * all negations, or has positives that simply miss it.  git reads an empty
	 * pattern file the same way, which is why `init` writes one that opens by
	 * including everything rather than leaving the file with no lines at all.
	 */
	int result = 0;
	size_t i;

	for (i = 0; i < sp->nr; i++) {
		const char *pat = sp->v[i];
		int neg = pat[0] == '!';
		const char *body = neg ? pat + 1 : pat;
		char *dir = xstrdup(rel);
		int hit = 0;

		if (match_one(body, rel, 0))
			hit = 1;
		/* every directory the path goes through counts as a match too */
		while (!hit) {
			char *slash = strrchr(dir, '/');

			if (!slash)
				break;
			*slash = '\0';
			if (!*dir)
				break;
			if (match_one(body, dir, 1))
				hit = 1;
		}
		free(dir);
		if (hit)
			result = !neg;
	}
	return result;
}

/*
 * Whether the work tree is meant to hold this path.  A repository that has not
 * turned sparse checkout on holds everything, so the answer is no without
 * reading anything.
 */
int sparse_skips(struct repo *r, const char *relpath)
{
	sparse_load(r);
	if (!cache_enabled)
		return 0;
	return !included(&cache, relpath);
}

/* ------------------------------------------------------------------ */
/* the patterns file                                                   */

/*
 * Replace the pattern file with this text.  Whatever was there goes: `set`
 * names the whole list, it does not add to one, so a pattern left over from
 * before cannot quietly keep something in the work tree.
 */
static int write_patterns(struct repo *r, const void *text, size_t len)
{
	char *path = sparse_file(r);
	char *dir = xstrdup(path);
	char *slash = strrchr(dir, '/');
	int rc;

	if (slash) {
		*slash = '\0';
		mkdir_p(dir);
	}
	free(dir);
	rc = write_file(path, text, len);
	if (rc < 0)
		gp_error("cannot write %s", path);
	free(path);
	sparse_forget();
	return rc;
}

static int patterns_write(struct repo *r, struct opts *o, int from)
{
	struct buf b;
	int i, rc;

	buf_init(&b);
	for (i = from; opts_arg(o, i); i++) {
		buf_addstr(&b, opts_arg(o, i));
		buf_addch(&b, '\n');
	}
	rc = write_patterns(r, b.b, b.len);
	buf_release(&b);
	return rc;
}

/* ------------------------------------------------------------------ */
/* putting the work tree in step with the patterns                     */

/*
 * Walk the index and make the work tree match: an excluded path the index does
 * not already mark is taken off disk and marked, and an included path that was
 * marked is put back and unmarked.  The index is written when anything moved.
 */
static int sparse_apply(struct repo *r)
{
	struct index_state ist;
	size_t i;
	int changed = 0;

	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));
	sparse_forget();

	for (i = 0; i < ist.nr; i++) {
		struct index_entry *e = &ist.e[i];
		char *full;
		int skip;

		if (e->stage)
			continue;
		if ((e->mode & 0170000) == 0160000)
			continue;      /* a submodule's directory is its own */
		skip = sparse_skips(r, e->path);
		full = xstrfmt("%s/%s", r->root, e->path);

		if (skip) {
			if (is_file(full)) {
				remove(full);
				changed = 1;
			}
			if (!(e->flags & IDX_FLAG_SKIP_WORKTREE)) {
				e->flags |= IDX_FLAG_SKIP_WORKTREE;
				changed = 1;
			}
		} else {
			if (e->flags & IDX_FLAG_SKIP_WORKTREE) {
				e->flags &= (u16)~IDX_FLAG_SKIP_WORKTREE;
				checkout_path(r, e->path, e->mode, &e->oid);
				changed = 1;
			}
		}
		free(full);
	}
	if (changed)
		index_write(&ist, repo_index_path(r));
	index_release(&ist);
	return 0;
}

/* ------------------------------------------------------------------ */
/* subcommands                                                         */

static int sparse_init(struct repo *r)
{
	/*
	 * The same two lines git's `init` writes, in either mode: the top-level
	 * files, and no directories.  It has to be written rather than left empty,
	 * because an empty list excludes everything -- so `init` on its own takes
	 * the directories away, as it does in git.
	 */
	static const char def[] = "/*\n!/*/\n";

	if (write_patterns(r, def, sizeof def - 1) < 0)
		return 1;
	sparse_config_set(r, 1);
	printf("Sparse checkout is on; the patterns are in info/sparse-checkout, "
	       "and `sparse-checkout list` shows them.\n");
	return sparse_apply(r);
}

static int sparse_set(struct repo *r, struct opts *o)
{
	if (!opts_arg(o, 1)) {
		gp_error("sparse-checkout set: expected at least one pattern\n"
			 "usage: gitprompt sparse-checkout set <pattern>...");
		return 1;
	}
	if (patterns_write(r, o, 1) < 0)
		return 1;
	sparse_config_set(r, 1);
	return sparse_apply(r);
}

static int sparse_list(struct repo *r)
{
	size_t i;

	sparse_load(r);
	for (i = 0; i < cache.nr; i++)
		printf("%s\n", cache.v[i]);
	return 0;
}

static int sparse_disable(struct repo *r)
{
	char *path = sparse_file(r);
	struct index_state ist;
	size_t i;

	/* the bits come off first: whether a path is in the work tree is asked
	 * of the patterns, and the patterns are about to be taken away */
	sparse_config_clear(r);
	sparse_forget();

	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));
	for (i = 0; i < ist.nr; i++) {
		struct index_entry *e = &ist.e[i];

		if (e->stage || !(e->flags & IDX_FLAG_SKIP_WORKTREE))
			continue;
		e->flags &= (u16)~IDX_FLAG_SKIP_WORKTREE;
		if ((e->mode & 0170000) != 0160000)
			checkout_path(r, e->path, e->mode, &e->oid);
	}
	index_write(&ist, repo_index_path(r));
	index_release(&ist);

	remove(path);
	free(path);
	return 0;
}

static void sparse_usage(void)
{
	gp_error("usage: gitprompt sparse-checkout init\n"
		 "   sparse-checkout set <pattern>...\n"
		 "   sparse-checkout list\n"
		 "   sparse-checkout disable");
}

int cmd_sparse_checkout(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *sub;

	opts_init(&o, argc, argv, NULL);

	if (!r->root) {
		gp_error("sparse-checkout: this needs a work tree to work in");
		return 1;
	}
	sub = opts_arg(&o, 0);
	if (!sub) {
		sparse_usage();
		return 1;
	}
	if (!strcmp(sub, "init"))
		return sparse_init(r);
	if (!strcmp(sub, "set"))
		return sparse_set(r, &o);
	if (!strcmp(sub, "list"))
		return sparse_list(r);
	if (!strcmp(sub, "disable"))
		return sparse_disable(r);

	sparse_usage();
	return 1;
}
