/*
 * ignore.c - which of the work tree's files nobody asked to record.
 *
 * A prompt repository collects things nobody means to keep: editor droppings,
 * build output, a virtual environment.  What those are is written down in
 * .gitpromptignore, and -- because a gitprompt repository is an ordinary git
 * repository too, and may have arrived with one -- in .gitignore.  Both are
 * read, with git's rules: the file in a directory is read after the ones above
 * it, so what it says beats theirs, and .gitpromptignore is read after
 * .gitignore, so the name this project uses beats that one inside a single
 * directory.
 *
 * The rules are git's, and the parts of them that are not obvious are the ones
 * that matter:
 *
 *   - a pattern with a slash in it, leading or in the middle, is relative to
 *     the directory its file is in; one without a slash matches a name at any
 *     depth
 *   - a trailing slash means a directory, and a pattern naming a directory
 *     takes everything under it along
 *   - * ? and [...] are globs, and a double star as a whole path segment stands
 *     for any number of segments
 *   - a leading ! puts a path back, but only if nothing above it is already
 *     excluded -- once a directory is out, everything under it is out
 *   - a path in the index is not ignored at all, whatever the rules say:
 *     ignoring is about what has not been recorded
 *
 * The last of those is why the index is read here rather than left to each
 * caller.  A file can be committed and only afterwards matched by a rule -- a
 * .o file tracked before anyone thought to ignore build output -- and a caller
 * that asked the rules alone would then stop seeing it.  Asking the index once,
 * here, is what git does: `git check-ignore` does not name a tracked path
 * either.
 */
#include "gp.h"

#include <string.h>

struct ignore_rule {
	char *pattern;          /* with a leading / and a trailing / taken off */
	int dir_only;           /* there was a trailing slash */
	int negated;            /* there was a leading ! */
	int anchored;           /* a slash in it, so relative to its directory */
};

struct ignore_file {
	char *dir;              /* "" is the root of the work tree */
	struct ignore_rule *r;
	size_t nr, alloc;
};

static struct ignore_file *files;
static size_t nfiles, files_alloc;
static char *files_root;        /* the root the cache above belongs to */

static struct slist tracked;    /* index paths, sorted, for the search below */
static char *tracked_root;

/* ------------------------------------------------------------------ */
/* one pattern against one name                                        */

static const char *base_of(const char *rel)
{
	const char *s = strrchr(rel, '/');
	return s ? s + 1 : rel;
}

/*
 * Does c fall in the set that begins at *pp, which points at the '['?  A match
 * leaves *pp past the closing bracket, and -1 means there is no closing bracket
 * at all -- a pattern like "a[b" has no set in it and the '[' is then an
 * ordinary byte.
 *
 * A ']' as the first thing in the set is an ordinary byte, so "[]]" names the
 * bracket; "a-z" is the range; and a backslash makes the next byte ordinary.
 */
static int class_match(const char **pp, const char *pe, unsigned char c)
{
	const char *p = *pp + 1;
	int neg = 0, first = 1, hit = 0;

	if (p < pe && (*p == '!' || *p == '^')) {
		neg = 1;
		p++;
	}
	while (p < pe) {
		unsigned char lo, hi;

		if (*p == ']' && !first)
			break;
		first = 0;
		if (*p == '\\' && p + 1 < pe)
			p++;
		lo = (unsigned char)*p++;
		if (p + 1 < pe && *p == '-' && p[1] != ']') {
			p++;
			if (*p == '\\' && p + 1 < pe)
				p++;
			hi = (unsigned char)*p++;
		} else {
			hi = lo;
		}
		if (lo <= c && c <= hi)
			hit = 1;
	}
	if (p >= pe)
		return -1;
	*pp = p + 1;
	return neg ? !hit : hit;
}

/*
 * One segment of a path against one segment of a pattern.  Neither can hold a
 * slash -- the caller has already cut at them -- and that is what makes a star
 * here a star that does not step over a directory.
 */
static int seg_match(const char *p, const char *pe,
		     const char *s, const char *se)
{
	while (p < pe) {
		if (*p == '*') {
			const char *q = p;

			while (q < pe && *q == '*')
				q++;
			if (q == pe)
				return 1;
			for (;; s++) {
				if (seg_match(q, pe, s, se))
					return 1;
				if (s == se)
					return 0;
			}
		}
		if (s == se)
			return 0;
		if (*p == '?') {
			p++;
			s++;
			continue;
		}
		if (*p == '[') {
			int m = class_match(&p, pe, (unsigned char)*s);

			if (m >= 0) {
				if (!m)
					return 0;
				s++;
				continue;
			}
		}
		if (*p == '\\' && p + 1 < pe)
			p++;
		if (*p != *s)
			return 0;
		p++;
		s++;
	}
	return s == se;
}

/*
 * The same matcher, over a whole name rather than a path: for the few places a
 * pattern is about one field, as `replace -l` matching the name of a replace
 * ref.  Neither side holds a slash, so a star here steps over nothing, and the
 * match runs to the end of the name.
 */
int glob_match_name(const char *pattern, const char *name)
{
	return seg_match(pattern, pattern + strlen(pattern),
			 name, name + strlen(name));
}

/*
 * A whole pattern against a whole path, a segment at a time, with a double star
 * standing for any number of segments wherever it is a segment of its own --
 * first, last, or in the middle.  Anywhere else it is no different from one
 * star, which is what git's own matcher does with it.
 */
static int wm(const char *pat, const char *s)
{
	const char *ps = strchr(pat, '/');
	const char *ss = strchr(s, '/');
	const char *pe = ps ? ps : pat + strlen(pat);
	const char *se = ss ? ss : s + strlen(s);

	if (pe - pat == 2 && pat[0] == '*' && pat[1] == '*') {
		if (!ps)
			return 1;
		for (;;) {
			if (wm(ps + 1, s))
				return 1;
			if (!ss)
				return 0;
			s = ss + 1;
			ss = strchr(s, '/');
		}
	}
	if (!seg_match(pat, pe, s, se))
		return 0;
	if (!ps)
		return ss == NULL;
	if (!ss)
		return 0;
	return wm(ps + 1, ss + 1);
}

/*
 * One pattern against one whole path from the root of the work tree.  This is
 * the matcher above, exposed because a pattern does not have to come from an
 * ignore file to be written in this syntax: the sparse-checkout pattern list is
 * the same language, read from the root (cmd_sparse.c).
 */
int path_match_root(const char *pattern, const char *relpath)
{
	return wm(pattern, relpath);
}

/*
 * One rule against one path, the path given relative to the directory the rule
 * came from.  A rule with a slash in it is about that whole path; one without
 * is about a name, and is matched against the last segment of the path
 * wherever it happens to be.
 */
static int rule_matches(const struct ignore_rule *rule, const char *rel,
			int is_dir)
{
	const char *base;

	if (rule->dir_only && !is_dir)
		return 0;
	if (rule->anchored)
		return wm(rule->pattern, rel);
	base = base_of(rel);
	return seg_match(rule->pattern, rule->pattern + strlen(rule->pattern),
			 base, base + strlen(base));
}

/* ------------------------------------------------------------------ */
/* reading the files                                                   */

/* is the byte at s[i] escaped -- an odd number of backslashes in front of it? */
static int is_escaped(const char *s, size_t i)
{
	size_t b = 0;

	while (i >= 1 + b && s[i - 1 - b] == '\\')
		b++;
	return b % 2;
}

static void add_rule(struct ignore_file *f, const char *line, size_t n)
{
	struct ignore_rule *rule;

	if (f->nr == f->alloc) {
		f->alloc = f->alloc ? f->alloc * 2 : 16;
		f->r = xrealloc(f->r, f->alloc * sizeof(*f->r));
	}
	rule = &f->r[f->nr++];
	memset(rule, 0, sizeof *rule);

	/* "build/" is about the directory; "build" is about either */
	if (n && line[n - 1] == '/' && !is_escaped(line, n - 1)) {
		rule->dir_only = 1;
		rule->pattern = xstrndup(line, n - 1);
	} else {
		rule->pattern = xstrndup(line, n);
	}

	/* a slash anywhere, the first byte included, anchors it to this file */
	if (strchr(rule->pattern, '/'))
		rule->anchored = 1;
	if (rule->pattern[0] == '/')
		memmove(rule->pattern, rule->pattern + 1,
			strlen(rule->pattern));
}

static void read_into(struct ignore_file *f, struct repo *r, const char *name)
{
	char *path;
	struct buf b;
	const char *p;

	if (!f->dir[0])
		path = xstrfmt("%s%s", r->root, name);
	else
		path = xstrfmt("%s/%s%s", r->root, f->dir, name);
	buf_init(&b);
	if (read_file(path, &b) < 0) {
		free(path);
		buf_release(&b);
		return;
	}
	free(path);

	p = (const char *)b.b;
	while (p && *p) {
		const char *eol = strchr(p, '\n');
		size_t n = eol ? (size_t)(eol - p) : strlen(p);
		char *line = xstrndup(p, n);
		size_t ln = strlen(line);

		/*
		 * The \r of a CRLF file, and spaces nobody escaped, are not
		 * part of the pattern: "Trail.txt   " is about Trail.txt, and
		 * "Trail\ .txt" is about a file with a space in its name.
		 */
		while (ln && (line[ln - 1] == '\r' ||
			      (line[ln - 1] == ' ' && !is_escaped(line, ln - 1))))
			line[--ln] = '\0';

		if (ln && line[0] == '#') {
			/* a comment, unless the # was escaped */
		} else if (ln && line[0] == '!') {
			/* "!" alone says nothing about anything */
			if (ln > 1) {
				add_rule(f, line + 1, ln - 1);
				f->r[f->nr - 1].negated = 1;
			}
		} else if (ln) {
			add_rule(f, line, ln);
		}
		free(line);
		if (!eol)
			break;
		p = eol + 1;
	}
	buf_release(&b);
}

/*
 * The rules that apply in one directory, read the first time it is asked
 * about.  A directory with no ignore file in it is one entry that answers
 * nothing: cheap, and the filesystem is asked about it only once.
 */
static struct ignore_file *find_file(struct repo *r, const char *dir)
{
	struct ignore_file *f;
	size_t i;

	if (!files_root || strcmp(files_root, r->root)) {
		for (i = 0; i < nfiles; i++) {
			size_t k;

			for (k = 0; k < files[i].nr; k++)
				free(files[i].r[k].pattern);
			free(files[i].r);
			free(files[i].dir);
		}
		free(files);
		files = NULL;
		nfiles = files_alloc = 0;
		free(files_root);
		files_root = xstrdup(r->root);
	}
	for (i = 0; i < nfiles; i++)
		if (!strcmp(files[i].dir, dir))
			return &files[i];

	if (nfiles == files_alloc) {
		files_alloc = files_alloc ? files_alloc * 2 : 8;
		files = xrealloc(files, files_alloc * sizeof(*files));
	}
	f = &files[nfiles++];
	memset(f, 0, sizeof *f);
	f->dir = xstrdup(dir);
	read_into(f, r, "/.gitignore");
	read_into(f, r, "/.gitpromptignore");
	return f;
}

/* ------------------------------------------------------------------ */
/* the question                                                        */

/*
 * What the rules say about one path: 0 when none of them speaks, 1 when the
 * last one to speak puts it back, 2 when the last one to speak takes it out.
 *
 * The directory the path is in is asked first and the root last, so a rule
 * below beats a rule above it, and inside one directory the lines are read
 * backwards, so the last line about a path is the one that decides.  That is
 * git's order, and it is why a subdirectory can put back what the root took
 * out.
 */
static int decide_one(struct repo *r, const char *rel, int is_dir)
{
	char *dir = xstrdup(rel);
	int verdict = 0;

	for (;;) {
		char *slash = strrchr(dir, '/');
		const struct ignore_file *f;
		const char *rel_in;
		size_t i;

		if (slash)
			*slash = '\0';          /* dir is the parent */
		else if (dir[0])
			dir[0] = '\0';          /* no parent left but the root */
		else
			break;                  /* the root has been asked */

		f = find_file(r, dir);
		rel_in = rel + strlen(dir) + (dir[0] ? 1 : 0);
		for (i = f->nr; i-- > 0; ) {
			if (!rule_matches(&f->r[i], rel_in, is_dir))
				continue;
			verdict = f->r[i].negated ? 1 : 2;
			goto done;
		}
	}
done:
	free(dir);
	return verdict;
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/*
 * The index's paths, sorted once.  The work tree is walked a file at a time and
 * every file asks whether it is tracked, so the answer has to be a search and
 * not a scan of every entry.
 */
static void tracked_load(struct repo *r)
{
	struct index_state ist = INDEX_INIT;
	size_t i;

	if (tracked_root && !strcmp(tracked_root, r->root))
		return;
	free(tracked_root);
	tracked_root = xstrdup(r->root);
	slist_release(&tracked);
	index_read(&ist, repo_index_path(r));
	for (i = 0; i < ist.nr; i++)
		slist_push(&tracked, ist.e[i].path);
	if (tracked.nr > 1)
		qsort(tracked.v, tracked.nr, sizeof(*tracked.v), cmp_str);
	index_release(&ist);
}

static int tracked_has(const char *rel)
{
	size_t lo = 0, hi = tracked.nr;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = strcmp(tracked.v[mid], rel);

		if (!c)
			return 1;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return 0;
}

static int ignored(struct repo *r, const char *rel, int is_dir)
{
	char *up;

	if (!r || !r->root || !rel || !rel[0])
		return 0;
	tracked_load(r);
	if (tracked_has(rel))
		return 0;

	/*
	 * Every directory above the path has the first say.  A path under a
	 * directory that is out is out too, and no ! further down brings it
	 * back -- which is the whole difference between a rule about a
	 * directory and a rule about the files in it.
	 */
	up = xstrdup(rel);
	for (;;) {
		char *slash = strrchr(up, '/');

		if (!slash)
			break;
		*slash = '\0';
		if (decide_one(r, up, 1) == 2) {
			free(up);
			return 1;
		}
	}
	free(up);

	return decide_one(r, rel, is_dir) == 2;
}

/*
 * The index was rewritten, so the copy of it kept here is behind.  Only the
 * index cache goes: the rule files are read from disk and do not move under a
 * running command.
 */
void ignore_forget(void)
{
	free(tracked_root);
	tracked_root = NULL;
	slist_release(&tracked);
}

int path_is_ignored(struct repo *r, const char *relpath)
{
	return ignored(r, relpath, 0);
}

/*
 * The same question asked about a directory.  A pattern may name a directory or
 * a file -- "build/" has to mean the directory, "build" means either -- so this
 * is the only form that can honour a trailing slash.
 */
int path_is_ignored_dir(struct repo *r, const char *relpath)
{
	return ignored(r, relpath, 1);
}
