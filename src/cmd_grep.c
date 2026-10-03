/*
 * grep.c - print the lines that match a pattern.
 *
 * Three things can be searched, and choosing between them is most of what the
 * command has to get right:
 *
 *   - the working tree (the default): every path the index knows about, read
 *     from disk;
 *   - the index (`--cached`): what has been staged, not what has been saved
 *     since;
 *   - a revision: each `<rev>` argument names a tree, and each tree is walked.
 *
 * The pattern syntax is the system regexes' -- basic by default, extended with
 * `-E`, literal with `-F` -- because that is what git grep does and what
 * people's patterns are written against.  The engine is in regex.c, and what it
 * cannot do it refuses loudly rather than answering something else.
 *
 * The exit status is the part of the contract easiest to get wrong and most
 * worth keeping: 0 when something was printed, 1 when nothing was, and 128 for
 * a pattern that does not compile or an argument that is neither a revision nor
 * a path.  That is git's.
 */
#include "gp.h"

#include <string.h>

enum grep_mode {
	GREP_LINES,             /* path[:line]:text */
	GREP_FILES,             /* -l: the path, once */
	GREP_NONFILES,          /* -L: the paths that had no match */
	GREP_COUNT              /* -c: path:count */
};

struct grep_ctx {
	struct rx **res;
	size_t nres;
	struct slist specs;
	enum grep_mode mode;
	int invert;
	int word;
	int lineno;             /* -n */
	int printed;            /* something reached stdout */
};

/* ------------------------------------------------------------------ */
/* one file                                                            */

static void grep_body(struct grep_ctx *g, const char *display,
		      const struct buf *body)
{
	struct dline *lines = NULL;
	size_t nr = 0, i;
	long count = 0;
	int hit_file = 0;

	/* a NUL means it is not text, and a text tool printing raw NULs is
	 * worse than saying nothing -- this is what git's default is too */
	if (body->len && memchr(body->b, '\0', body->len))
		return;

	diff_split_lines(body->b, body->len, &lines, &nr);

	for (i = 0; i < nr; i++) {
		size_t len = lines[i].len;
		size_t ms, me;
		int hit;

		if (len && lines[i].p[len - 1] == '\n')
			len--;          /* `$` anchors before the newline */

		hit = rx_search_any(g->res, g->nres, lines[i].p, len,
				    g->word, &ms, &me);
		if (hit < 0)
			gp_die("grep: the pattern is too complex for this file");
		if (g->invert)
			hit = !hit;
		if (!hit)
			continue;

		count++;
		hit_file = 1;
		/* one line settles whether the file is named at all */
		if (g->mode == GREP_FILES || g->mode == GREP_NONFILES)
			break;
		if (g->mode == GREP_LINES) {
			fputs(display, stdout);
			if (g->lineno)
				printf(":%lu", (unsigned long)(i + 1));
			putchar(':');
			fwrite(lines[i].p, 1, len, stdout);
			putchar('\n');
			g->printed = 1;
		}
	}

	switch (g->mode) {
	case GREP_FILES:
		if (hit_file) {
			printf("%s\n", display);
			g->printed = 1;
		}
		break;
	case GREP_NONFILES:
		if (!hit_file) {
			printf("%s\n", display);
			g->printed = 1;
		}
		break;
	case GREP_COUNT:
		if (count) {
			printf("%s:%ld\n", display, count);
			g->printed = 1;
		}
		break;
	case GREP_LINES:
		break;
	}

	free(lines);
}

/* ------------------------------------------------------------------ */
/* where the lines come from                                           */

struct rev_walk {
	struct grep_ctx *g;
	struct repo *r;
	const char *rev;
};

/* one file of a revision's tree; the revision's name goes in front of the
 * path, exactly as the user spelled it, which is what git prints */
static void grep_tree_cb(const char *path, u32 mode, const oid_t *oid, void *data)
{
	struct rev_walk *w = data;
	struct buf b = BUF_INIT;
	char *display;

	(void)mode;
	if (!slist_matches(&w->g->specs, path))
		return;
	if (odb_read(&w->r->odb, oid, NULL, &b) == 0) {
		display = xstrfmt("%s:%s", w->rev, path);
		grep_body(w->g, display, &b);
		free(display);
	}
	buf_release(&b);
}

static void grep_worktree(struct repo *r, struct grep_ctx *g)
{
	struct index_state ist;
	size_t i;

	memset(&ist, 0, sizeof(ist));
	index_read(&ist, repo_index_path(r));

	for (i = 0; i < ist.nr; i++) {
		struct index_entry *e = &ist.e[i];
		struct buf b = BUF_INIT;
		char *full;

		/* a conflicted path has no stage-0 entry and no file to read */
		if (e->stage || !slist_matches(&g->specs, e->path))
			continue;
		full = r->root ? xstrfmt("%s/%s", r->root, e->path)
			       : xstrdup(e->path);
		if (read_file(full, &b) == 0)
			grep_body(g, e->path, &b);
		buf_release(&b);
		free(full);
	}

	index_release(&ist);
}

static void grep_cached(struct repo *r, struct grep_ctx *g)
{
	struct index_state ist;
	size_t i;

	memset(&ist, 0, sizeof(ist));
	index_read(&ist, repo_index_path(r));

	for (i = 0; i < ist.nr; i++) {
		struct index_entry *e = &ist.e[i];
		struct buf b = BUF_INIT;

		if (e->stage || !slist_matches(&g->specs, e->path))
			continue;
		if (odb_read(&r->odb, &e->oid, NULL, &b) == 0)
			grep_body(g, e->path, &b);
		buf_release(&b);
	}

	index_release(&ist);
}

static int path_exists(struct repo *r, const char *path)
{
	char *full = r->root ? xstrfmt("%s/%s", r->root, path) : xstrdup(path);
	int yes = is_file(full) || is_directory(full);

	free(full);
	return yes;
}

/* ------------------------------------------------------------------ */

int cmd_grep(struct repo *r, int argc, char **argv)
{
	static const char *const allows[] = {
		"-e=",
		"-i", "--ignore-case",
		"-w", "--word-regexp",
		"-v", "--invert-match",
		"-n", "--line-number",
		"-l", "--files-with-matches", "--name-only",
		"-L", "--files-without-match",
		"-c", "--count",
		"-F", "--fixed-strings",
		"-E", "--extended-regexp",
		"-G", "--basic-regexp",
		"--cached",
		NULL
	};
	struct opts o;
	struct grep_ctx g;
	struct { const char *rev; oid_t oid; } *revs;
	const char **pats;
	size_t npats = 0, nrevs = 0;
	int cut = argc, cached, icase, fixed, ere;
	int i, argi = 0, seen_path = 0;

	/*
	 * opts_init swallows the `--` and does not say where it was, but git's
	 * disambiguation needs the split: before it an argument may be a
	 * revision, after it everything is a path.  So the parse stops there,
	 * and the rest is taken as pathspecs below.
	 */
	for (i = 0; i < argc; i++)
		if (!strcmp(argv[i], "--")) {
			cut = i;
			break;
		}
	opts_init(&o, cut, argv, allows);

	memset(&g, 0, sizeof(g));

	pats = xmalloc(((size_t)argc + 1) * sizeof(*pats));
	/* -e may be given more than once, and opts_value would keep only the
	 * last of them, so the flag list is read directly */
	for (i = 0; i < o.nf; i++) {
		if (strcmp(o.flags[i].name, "-e"))
			continue;
		if (!o.flags[i].value) {
			gp_error("grep: -e needs a pattern");
			return 1;
		}
		pats[npats++] = o.flags[i].value;
	}
	if (!npats) {
		if (!o.nargs) {
			gp_error("grep: expected a pattern");
			return 1;
		}
		pats[npats++] = o.args[0];
		argi = 1;
	}

	icase = opts_flag(&o, "-i") || opts_flag(&o, "--ignore-case");
	fixed = opts_flag(&o, "-F") || opts_flag(&o, "--fixed-strings");
	ere = !fixed && (opts_flag(&o, "-E") ||
			 opts_flag(&o, "--extended-regexp"));

	g.res = xmalloc((npats + 1) * sizeof(*g.res));
	g.nres = npats;
	for (i = 0; i < (int)npats; i++) {
		char *err = NULL;
		int rc = fixed
			? rx_compile_fixed(&g.res[i], pats[i], icase, &err)
			: rx_compile(&g.res[i], pats[i], ere, icase, &err);

		if (rc < 0)
			gp_die("grep: invalid regular expression: %s",
			       err ? err : pats[i]);
	}
	free(pats);

	g.invert = opts_flag(&o, "-v") || opts_flag(&o, "--invert-match");
	g.word = opts_flag(&o, "-w") || opts_flag(&o, "--word-regexp");
	g.lineno = opts_flag(&o, "-n") || opts_flag(&o, "--line-number");

	/* -L wins over -l, which wins over -c: what git does with all three */
	if (opts_flag(&o, "-L") || opts_flag(&o, "--files-without-match"))
		g.mode = GREP_NONFILES;
	else if (opts_flag(&o, "-l") || opts_flag(&o, "--files-with-matches") ||
		 opts_flag(&o, "--name-only"))
		g.mode = GREP_FILES;
	else if (opts_flag(&o, "-c") || opts_flag(&o, "--count"))
		g.mode = GREP_COUNT;
	else
		g.mode = GREP_LINES;

	cached = opts_flag(&o, "--cached");

	/*
	 * What is left is a revision or a path.  Before a path has been seen an
	 * argument that names a tree is a revision; one that names neither is
	 * an error, which is what git calls an ambiguous argument.  Once a path
	 * has been seen no later argument can be a revision -- `grep p HEAD
	 * file` searches HEAD, `grep p file HEAD` looks for a file called HEAD.
	 */
	revs = xmalloc(((size_t)o.nargs + 1) * sizeof(*revs));
	for (; argi < o.nargs; argi++) {
		const char *a = o.args[argi];
		oid_t oid;
		struct buf n;

		if (!seen_path && !cached && resolve_rev_tree(r, a, &oid) == 0) {
			revs[nrevs].rev = a;
			revs[nrevs].oid = oid;
			nrevs++;
			continue;
		}
		if (!seen_path && !path_exists(r, a))
			gp_die("grep: ambiguous argument '%s': unknown revision or path not in the working tree",
			       a);
		seen_path = 1;

		buf_init(&n);
		path_normalize(a, &n);
		if (n.len)
			slist_push(&g.specs, buf_cstr(&n));
		buf_release(&n);
	}

	/* everything after `--` is a path, and git does not check that it
	 * exists -- a pathspec that matches nothing is not an error */
	for (i = cut + 1; i < argc; i++) {
		struct buf n;

		buf_init(&n);
		path_normalize(argv[i], &n);
		if (n.len)
			slist_push(&g.specs, buf_cstr(&n));
		buf_release(&n);
	}

	if (cached) {
		grep_cached(r, &g);
	} else if (nrevs) {
		for (i = 0; i < (int)nrevs; i++) {
			struct rev_walk w;

			w.g = &g;
			w.r = r;
			w.rev = revs[i].rev;
			if (load_tree_flat(r, &revs[i].oid, "", grep_tree_cb,
					   &w) < 0)
				gp_die("grep: cannot read the tree of %s",
				       revs[i].rev);
		}
	} else {
		grep_worktree(r, &g);
	}

	for (i = 0; i < (int)g.nres; i++)
		rx_release(g.res[i]);
	free(g.res);
	free(revs);
	slist_release(&g.specs);

	return g.printed ? 0 : 1;
}
