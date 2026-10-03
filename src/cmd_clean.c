/*
 * clean.c - take out of the work tree what the index does not know about.
 *
 * Two things decide what may go: the index, and the ignore rules -- the ones
 * src/ignore.c reads out of .gitignore and .gitpromptignore.  A path the
 * index holds is never touched -- this is the one command here whose whole job
 * is deletion, and the thing it must never delete is somebody's work -- while
 * a path it does not hold is a candidate, and which candidates are taken is
 * what the three "which files" options choose between:
 *
 *   (nothing)  the untracked ones; ignored files are left where they are
 *   -x         all of them, ignored or not
 *   -X         only the ignored ones
 *
 * `-d` brings untracked directories into it.  Without it a directory is only
 * walked into when it holds something the index knows, or when -X is looking
 * for ignored files inside it -- an ignored file can live under a directory
 * that is not itself ignored, and there is no other way to reach it.
 *
 * A directory is reported whole when everything under it is going, which is
 * why `clean -ndx` says "Would remove allign/" rather than naming the files
 * one at a time: the removal is recursive either way, and the shorter line is
 * the truer one.  A directory the index holds is never collapsed, so only its
 * untracked contents are listed.
 *
 * Nothing is removed without -f.  Every other command here acts on what it was
 * given; this one destroys what it was pointed at, so it refuses by default
 * and says why rather than printing a list and then acting on it.
 */
#include "gp.h"

#include <string.h>
#include <dirent.h>

struct victim {
	char *display;          /* what the line says, a directory ending in / */
	char *path;             /* what is removed */
	int is_dir;
};

struct clean_ctx {
	struct repo *r;
	const struct index_state *ist;
	int dirs;               /* -d */
	int also_ignored;       /* -x */
	int only_ignored;       /* -X */
	int dry_run;            /* -n */
	int quiet;              /* -q */
	struct slist excl;      /* -e patterns, as written */
	struct slist specs;     /* pathspecs, normalised */
	struct victim *v;
	size_t nv, alloc;
	int failed;
};

/* ------------------------------------------------------------------ */
/* what a pattern means                                                */

/*
 * One -e pattern against one path: a trailing slash says the pattern is about a
 * directory, and a pattern with no slash in it matches a basename at any depth.
 * Narrower than a pattern in an ignore file -- no globs -- which is a
 * divergence from git, and it lives here rather than in src/ignore.c because
 * that file's matcher answers a different, wider question.
 */
static int pat_match(const char *pat, const char *rel, int is_dir)
{
	const char *base = strrchr(rel, '/');
	size_t n = strlen(pat);
	int dir_only = 0;

	base = base ? base + 1 : rel;
	if (n && pat[n - 1] == '/') {
		dir_only = 1;
		n--;
	}
	if (!n)
		return 0;
	if (dir_only && !is_dir)
		return 0;
	if (memchr(pat, '/', n))
		return strlen(rel) == n && !strncmp(rel, pat, n);
	return strlen(base) == n && !strncmp(base, pat, n);
}

static int excluded(const struct clean_ctx *c, const char *rel, int is_dir)
{
	size_t i;

	for (i = 0; i < c->excl.nr; i++)
		if (pat_match(c->excl.v[i], rel, is_dir))
			return 1;
	return 0;
}

/*
 * Is this candidate one the run wants?
 *
 * `-e` is what makes this less obvious than it reads.  It is an exclude rule,
 * and the three modes are three attitudes to exclude rules: plain leaves them
 * alone, -x takes everything but them, and -X takes exactly them.  So an -e
 * pattern adds to what -X is after rather than subtracting from it, which is
 * why `clean -ndX -e plain.txt` names a file nothing had called ignored.
 */
static int wanted(const struct clean_ctx *c, const char *rel, int ign)
{
	int e = excluded(c, rel, 0);

	if (c->only_ignored)
		return ign || e;
	if (e)
		return 0;
	if (c->also_ignored)
		return 1;
	return !ign;
}

/* ------------------------------------------------------------------ */
/* the walk                                                            */

struct victim *victim_new(struct clean_ctx *c, const char *rel, int is_dir)
{
	struct victim *v;

	if (c->nv == c->alloc) {
		c->alloc = c->alloc ? c->alloc * 2 : 16;
		c->v = xrealloc(c->v, c->alloc * sizeof(*c->v));
	}
	v = &c->v[c->nv++];
	v->path = xstrdup(rel);
	v->display = is_dir ? xstrfmt("%s/", rel) : xstrdup(rel);
	v->is_dir = is_dir;
	return v;
}

/* is any index path below this directory? */
static int holds_tracked(const struct clean_ctx *c, const char *rel)
{
	size_t n = strlen(rel), i;

	for (i = 0; i < c->ist->nr; i++) {
		const char *p = c->ist->e[i].path;
		if (!strncmp(p, rel, n) && p[n] == '/')
			return 1;
	}
	return 0;
}

/*
 * A store is a directory holding HEAD, objects/ and refs/.  The three together
 * and not just the name: a stray directory called .git with a config file in
 * it is not a repository, and git removes it like any other directory.
 */
static int looks_like_store(const char *path)
{
	char *p;
	int yes;

	p = xstrfmt("%s/HEAD", path);
	yes = is_file(p);
	free(p);
	if (!yes)
		return 0;
	p = xstrfmt("%s/objects", path);
	yes = is_directory(p);
	free(p);
	if (!yes)
		return 0;
	p = xstrfmt("%s/refs", path);
	yes = is_directory(p);
	free(p);
	return yes;
}

/*
 * A directory that is a repository in its own right is left alone.  git does
 * the same, and it does it however many -f are given; deleting into a nested
 * .git is the kind of mistake the command that made it cannot undo.
 * gitprompt's own store gets the same treatment under its own name.
 */
static int is_nested_repo(const char *abs)
{
	char *p = xstrfmt("%s/.git", abs);
	int yes = looks_like_store(p);

	free(p);
	if (yes)
		return 1;
	p = xstrfmt("%s/.gitprompt", abs);
	yes = looks_like_store(p);
	free(p);
	return yes;
}

/*
 * How many files are under the directory, and how many of them this run
 * wants.  The two numbers are what the collapse is decided on.
 */
static void count_dir(struct clean_ctx *c, const char *rel, const char *abs,
		      long *total, long *want)
{
	DIR *d = opendir(abs);
	struct dirent *de;

	if (!d)
		return;
	while ((de = readdir(d))) {
		char *sr, *sa;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		sr = xstrfmt("%s/%s", rel, de->d_name);
		sa = xstrfmt("%s/%s", abs, de->d_name);
		if (is_directory(sa)) {
			if (!is_nested_repo(sa) && !excluded(c, sr, 1))
				count_dir(c, sr, sa, total, want);
		} else {
			int ign = path_is_ignored(c->r, sr);

			(*total)++;
			if (wanted(c, sr, ign) && slist_matches(&c->specs, sr))
				(*want)++;
		}
		free(sr);
		free(sa);
	}
	closedir(d);
}

static void clean_dir(struct clean_ctx *c, const char *rel, const char *abs);

static void clean_file(struct clean_ctx *c, const char *rel)
{
	if (index_get(c->ist, rel))
		return;
	if (!slist_matches(&c->specs, rel))
		return;
	if (!wanted(c, rel, path_is_ignored(c->r, rel)))
		return;
	victim_new(c, rel, 0);
}

/*
 * Could a pathspec name the directory itself, or anything under it?  A spec
 * that reaches below the directory is why `clean -n -- newdir/c.txt` descends
 * into newdir with no -d: the path that was asked about is in there.  A spec
 * that reaches nowhere near it is why `clean -ndx -- two` says nothing about
 * emptyd: -d opens up directories the run was pointed at, not all of them.
 */
static int spec_selected(const struct clean_ctx *c, const char *rel)
{
	size_t n = strlen(rel), i;

	if (!c->specs.nr)
		return 1;
	if (slist_matches(&c->specs, rel))
		return 1;
	for (i = 0; i < c->specs.nr; i++)
		if (!strncmp(c->specs.v[i], rel, n) && c->specs.v[i][n] == '/')
			return 1;
	return 0;
}

static void clean_subdir(struct clean_ctx *c, const char *rel, const char *abs)
{
	int selected, may_take;
	long total = 0, want = 0;

	/*
	 * An -e pattern on the directory takes the whole directory out of the
	 * walk.  Under -X that is what makes it a candidate -- it counts as an
	 * ignored path, and an ignored directory is one line or nothing at all,
	 * never a walk into what is inside it.
	 */
	if (excluded(c, rel, 1)) {
		if (c->only_ignored && c->dirs)
			victim_new(c, rel, 1);
		return;
	}

	/* the index knows something in here: only its untracked side is ours */
	if (holds_tracked(c, rel)) {
		clean_dir(c, rel, abs);
		return;
	}
	if (is_nested_repo(abs))
		return;

	selected = spec_selected(c, rel);
	if (!selected)
		return;

	/*
	 * A pathspec names the directory it stops at, so that is where the
	 * collapse happens: `clean -n -- a/b` says a/b/, not a/, even though
	 * the only thing under a/ is b/ and the only thing under b/ is going
	 * too.  With no pathspec it is -d that decides.
	 */
	may_take = c->specs.nr ? slist_matches(&c->specs, rel) : c->dirs;

	count_dir(c, rel, abs, &total, &want);

	if (may_take) {
		if (!total) {
			/* an empty directory is not an ignored one */
			if (!c->only_ignored)
				victim_new(c, rel, 1);
			return;
		}
		if (want == total) {
			victim_new(c, rel, 1);
			return;
		}
		if (!want)
			return;
		clean_dir(c, rel, abs);
		return;
	}

	/*
	 * The directory is not a candidate whole.  It is still walked into
	 * when a pathspec reaches below it, when -d was asked for, or -- in
	 * -X alone -- when it is mixed: a directory holding nothing but
	 * ignored files is the whole story, but a mixed one has ignored files
	 * that are reachable no other way.
	 */
	if (c->specs.nr || c->dirs || (c->only_ignored && want && want < total))
		clean_dir(c, rel, abs);
}

static void clean_dir(struct clean_ctx *c, const char *rel, const char *abs)
{
	DIR *d = opendir(abs);
	struct dirent *de;

	if (!d)
		return;
	while ((de = readdir(d))) {
		char *sr, *sa;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		/* the store itself, at the top of the tree and nowhere else */
		if (!rel[0] && (!strcmp(de->d_name, ".gitprompt") ||
				!strcmp(de->d_name, ".git")))
			continue;
		sr = rel[0] ? xstrfmt("%s/%s", rel, de->d_name)
			    : xstrdup(de->d_name);
		sa = xstrfmt("%s/%s", abs, de->d_name);
		if (is_directory(sa))
			clean_subdir(c, sr, sa);
		else
			clean_file(c, sr);
		free(sr);
		free(sa);
	}
	closedir(d);
}

/* ------------------------------------------------------------------ */

static int victim_cmp(const void *a, const void *b)
{
	const struct victim *x = a, *y = b;
	return strcmp(x->display, y->display);
}

int cmd_clean(struct repo *r, int argc, char **argv)
{
	static const char *const allows[] = {
		"-n", "--dry-run",
		"-f", "--force",
		"-d",
		"-x",
		"-X",
		"-q", "--quiet",
		"-e=", "--exclude=",
		NULL
	};
	struct opts o;
	struct clean_ctx c;
	struct index_state ist = INDEX_INIT;
	size_t i;
	int fi;

	opts_init(&o, argc, argv, allows);

	memset(&c, 0, sizeof c);
	c.r = r;
	c.ist = &ist;
	c.dirs = opts_flag(&o, "-d");
	c.also_ignored = opts_flag(&o, "-x");
	c.only_ignored = opts_flag(&o, "-X");
	c.dry_run = opts_flag(&o, "-n") || opts_flag(&o, "--dry-run");
	c.quiet = opts_flag(&o, "-q") || opts_flag(&o, "--quiet");
	if (!r->root)
		gp_die("clean: this operation must be run in a work tree");

	if (c.also_ignored && c.only_ignored)
		gp_die("options '-x' and '-X' cannot be used together");

	if (!c.dry_run && !opts_flag(&o, "-f") && !opts_flag(&o, "--force"))
		gp_die("clean.requireForce is true and -f not given:"
		       " refusing to clean");

	/* -e may be given more than once, and opts_value keeps only the last */
	for (fi = 0; fi < o.nf; fi++) {
		const char *nm = o.flags[fi].name;
		if (strcmp(nm, "-e") && strcmp(nm, "--exclude"))
			continue;
		if (!o.flags[fi].value)
			gp_die("clean: -e needs a pattern");
		slist_push(&c.excl, o.flags[fi].value);
	}
	for (i = 0; i < (size_t)o.nargs; i++) {
		struct buf n;
		buf_init(&n);
		path_normalize(o.args[i], &n);
		if (n.len)
			slist_push(&c.specs, buf_cstr(&n));
		buf_release(&n);
	}

	index_read(&ist, repo_index_path(r));
	clean_dir(&c, "", r->root);

	qsort(c.v, c.nv, sizeof(*c.v), victim_cmp);
	for (i = 0; i < c.nv; i++) {
		char *abs = xstrfmt("%s/%s", r->root, c.v[i].path);
		int rc;

		if (!c.quiet)
			printf("%s %s\n", c.dry_run ? "Would remove"
						    : "Removing",
			       c.v[i].display);
		rc = c.dry_run ? 0
		     : c.v[i].is_dir ? remove_dir_recursive(abs)
				     : remove_file(abs);
		if (rc < 0) {
			gp_error("clean: failed to remove %s", c.v[i].path);
			c.failed = 1;
		}
		free(abs);
		free(c.v[i].path);
		free(c.v[i].display);
	}

	free(c.v);
	slist_release(&c.excl);
	slist_release(&c.specs);
	index_release(&ist);
	return c.failed;
}
