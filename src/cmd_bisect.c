/*
 * bisect - find the commit that introduced a change, by halving the range.
 *
 * The user knows one commit that is bad and one that is good, and somewhere
 * between them is the first bad commit.  Each commit tested splits what is
 * left, so log2 of the range is all it takes to find it -- the work is not in
 * the halving but in choosing *which* commit halves the range, and in
 * remembering the answers between runs.
 *
 * The remembering is git's, down to the file names: `refs/bisect/bad` and one
 * `refs/bisect/good-<id>` per good commit hold the range, `BISECT_START` holds
 * where to go back to, and `BISECT_LOG` holds every step in a form that can be
 * read back.  A session started by one of the two programs can therefore be
 * finished by the other, which is worth more than any private design would be.
 *
 * The choice of commit is git's `do_find_bisection`: weigh each candidate by
 * how many of the others it can reach, and take the one nearest half.  On a
 * linear history this picks the same commit git picks, step for step.  On a
 * merge history the walk that produces those weights can leave one commit at
 * half where git found another, so the probes may come in a different order --
 * but the commit the halves finally settle on is the first bad one either way,
 * which is the only thing the answer is.
 */
#include "gp.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#endif

#define TERM_BAD   "bad"
#define TERM_GOOD  "good"
#define REF_BAD    "refs/bisect/bad"
#define REF_PREFIX "refs/bisect/"

/* git's pseudo-ref holding the commit the last answer was about */
#define REF_EXPECTED "BISECT_EXPECTED_REV"

/* ------------------------------------------------------------------ */
/* the state a run remembers                                           */

struct bisect_state {
	oid_t bad;
	int have_bad;
	struct oid_array goods;
	struct oid_array skips;
};

static void state_init(struct bisect_state *s)
{
	memset(s, 0, sizeof *s);
	s->goods = (struct oid_array)OID_ARRAY_INIT;
	s->skips = (struct oid_array)OID_ARRAY_INIT;
}

static void state_release(struct bisect_state *s)
{
	oid_array_clear(&s->goods);
	oid_array_clear(&s->skips);
}

static char *state_path(struct repo *r, const char *name)
{
	return repo_git_path(r, "%s", name);
}

static void state_unlink(struct repo *r, const char *name)
{
	char *p = state_path(r, name);

	remove_file(p);
	free(p);
}

/* a session is under way when BISECT_START has something in it */
static int state_running(struct repo *r)
{
	char *p = state_path(r, "BISECT_START");
	struct buf b;
	int there;

	buf_init(&b);
	there = read_file(p, &b) == 0 && b.len > 0;
	buf_release(&b);
	free(p);
	return there;
}

static void register_ref(const char *name, const oid_t *oid, void *data)
{
	struct bisect_state *s = data;
	const char *tail = name + strlen(REF_PREFIX);

	if (!strcmp(name, REF_BAD)) {
		s->bad = *oid;
		s->have_bad = 1;
	} else if (!strncmp(tail, "good-", 5)) {
		oid_array_append(&s->goods, oid);
	} else if (!strncmp(tail, "skip-", 5)) {
		oid_array_append(&s->skips, oid);
	}
}

static void read_state(struct repo *r, struct bisect_state *s)
{
	state_init(s);
	refs_list(&r->refs, "refs/bisect", register_ref, s);
	refs_list_packed(&r->refs, "refs/bisect", register_ref, s);
}

static void ref_name(const char *state, const oid_t *oid, struct buf *out)
{
	char hex[GP_SHA1_HEXSZ + 1];

	buf_reset(out);
	if (!strcmp(state, TERM_BAD)) {
		buf_addstr(out, REF_BAD);
		return;
	}
	oid_hex(oid, hex);
	buf_addstr(out, REF_PREFIX);
	buf_addstr(out, state);
	buf_addch(out, '-');
	buf_addstr(out, hex);
}

static void clean_state(struct repo *r)
{
	struct bisect_state s;
	struct buf ref = BUF_INIT;
	size_t i;

	read_state(r, &s);
	refs_delete(&r->refs, REF_BAD);
	for (i = 0; i < s.goods.nr; i++) {
		ref_name(TERM_GOOD, &s.goods.oid[i], &ref);
		refs_delete(&r->refs, buf_cstr(&ref));
	}
	for (i = 0; i < s.skips.nr; i++) {
		ref_name("skip", &s.skips.oid[i], &ref);
		refs_delete(&r->refs, buf_cstr(&ref));
	}
	buf_release(&ref);
	state_release(&s);

	refs_delete(&r->refs, REF_EXPECTED);
	/*
	 * BISECT_START goes last, so that a run interrupted here is still
	 * recognisable as unfinished.
	 */
	state_unlink(r, "BISECT_ANCESTORS_OK");
	state_unlink(r, "BISECT_LOG");
	state_unlink(r, "BISECT_NAMES");
	state_unlink(r, "BISECT_RUN");
	state_unlink(r, "BISECT_TERMS");
	state_unlink(r, "BISECT_FIRST_PARENT");
	state_unlink(r, "BISECT_START");
}

/* ------------------------------------------------------------------ */
/* the log, which is also what the user is told                        */

static void log_append(struct repo *r, const char *text, size_t len)
{
	char *path = state_path(r, "BISECT_LOG");
	struct buf b;

	buf_init(&b);
	if (read_file(path, &b) < 0)
		buf_reset(&b);
	buf_add(&b, text, len);
	write_file(path, b.b, b.len);
	buf_release(&b);
	free(path);
}

static void log_append_str(struct repo *r, const char *text)
{
	log_append(r, text, strlen(text));
}

/*
 * Say something about the session: on the screen, and in the log behind a
 * comment character so that reading the log back explains itself.
 */
static void log_printf(struct repo *r, const char *fmt, ...)
{
	va_list ap;
	int n;
	char *msg, *line;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	msg = xmalloc((size_t)n + 1);
	va_start(ap, fmt);
	vsnprintf(msg, (size_t)n + 1, fmt, ap);
	va_end(ap);

	fputs(msg, stdout);
	line = xstrfmt("# %s", msg);
	log_append_str(r, line);
	free(line);
	free(msg);
}

static char *commit_subject(struct repo *r, const oid_t *oid)
{
	struct commit c = COMMIT_INIT;
	char *s;

	read_commit(r, oid, &c);
	s = commit_message_line(&c);
	commit_release(&c);
	return s;
}

/*
 * One answer, in the three places it has to be written: the ref, the line the
 * log explains, and the line that would replay the answer.  `start` writes its
 * own command line instead, which is what nolog is for.
 */
static void bisect_write(struct repo *r, const char *state, const oid_t *oid,
			 int nolog)
{
	char hex[GP_SHA1_HEXSZ + 1];
	struct buf ref = BUF_INIT;
	char *subj, *line;

	ref_name(state, oid, &ref);
	if (refs_write(&r->refs, buf_cstr(&ref), oid) < 0)
		gp_die("bisect: cannot write %s", buf_cstr(&ref));
	buf_release(&ref);

	oid_hex(oid, hex);
	subj = commit_subject(r, oid);
	line = xstrfmt("# %s: [%s] %s\n", state, hex, subj);
	log_append_str(r, line);
	free(line);
	free(subj);

	if (!nolog) {
		line = xstrfmt("git bisect %s %s\n", state, hex);
		log_append_str(r, line);
		free(line);
	}
}

/* git quotes the arguments of the start line, so the log reads back */
static void quote_arg(struct buf *out, const char *s)
{
	buf_addch(out, ' ');
	buf_addch(out, '\'');
	for (; *s; s++) {
		if (*s == '\'')
			buf_addstr(out, "'\\''");
		else
			buf_addch(out, *s);
	}
	buf_addch(out, '\'');
}

/* ------------------------------------------------------------------ */
/* the candidate graph                                                 */

/*
 * A candidate and the part of the graph around it that matters.  `first` and
 * `extra` are indices into the same array, or -1 for a parent that is not a
 * candidate -- git's "uninteresting" parent, where the walk stops.
 */
struct bnode {
	oid_t oid;
	int weight;             /* git's weight: -2 unknown, -1 one parent, 0.. */
	int counted;            /* git's COUNTED flag, cleared between counts */
	int first;
	int *extra;
	int nextra;
};

struct bgraph {
	struct bnode *n;
	int nr;
	oid_t *sorted;          /* the candidate ids, sorted, for lookup */
	int *rank;              /* rank[i] is the node that sorted[i] came from */
};

static int node_of(const struct bgraph *g, const oid_t *oid)
{
	int lo = 0, hi = g->nr - 1;

	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		int c = oid_compare(oid, &g->sorted[mid]);

		if (c == 0)
			return g->rank[mid];
		if (c < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return -1;
}

static int parent_at(const struct bnode *n, int k)
{
	return k == 0 ? n->first : n->extra[k - 1];
}

static int interesting_parents(const struct bnode *n)
{
	int k, seen = 0;

	for (k = 0; k <= n->nextra; k++)
		if (parent_at(n, k) >= 0)
			seen++;
	return seen;
}

static void graph_build(struct repo *r, const struct oid_array *cands,
			struct bgraph *g)
{
	int i, j;

	g->nr = (int)cands->nr;
	g->n = xcalloc((size_t)g->nr, sizeof *g->n);
	g->sorted = xmalloc((size_t)g->nr * sizeof *g->sorted);
	g->rank = xmalloc((size_t)g->nr * sizeof *g->rank);

	for (i = 0; i < g->nr; i++) {
		g->n[i].oid = cands->oid[i];
		g->n[i].first = -1;
		g->n[i].extra = NULL;
		g->n[i].nextra = 0;
		g->n[i].weight = 0;
		g->n[i].counted = 0;
		g->sorted[i] = cands->oid[i];
		g->rank[i] = i;
	}
	/* an insertion sort of the ids, carrying their positions along */
	for (i = 1; i < g->nr; i++) {
		oid_t o = g->sorted[i];
		int rk = g->rank[i];

		for (j = i; j > 0 && oid_compare(&g->sorted[j - 1], &o) > 0; j--) {
			g->sorted[j] = g->sorted[j - 1];
			g->rank[j] = g->rank[j - 1];
		}
		g->sorted[j] = o;
		g->rank[j] = rk;
	}
	for (i = 0; i < g->nr; i++) {
		struct commit c = COMMIT_INIT;

		read_commit(r, &cands->oid[i], &c);
		if (c.parents.nr) {
			g->n[i].first = node_of(g, &c.parents.oid[0]);
			g->n[i].nextra = (int)c.parents.nr - 1;
			if (g->n[i].nextra)
				g->n[i].extra = xmalloc((size_t)g->n[i].nextra *
							sizeof(int));
			for (j = 1; j < (int)c.parents.nr; j++)
				g->n[i].extra[j - 1] = node_of(g, &c.parents.oid[j]);
		}
		commit_release(&c);
	}
}

static void graph_free(struct bgraph *g)
{
	int i;

	for (i = 0; i < g->nr; i++)
		free(g->n[i].extra);
	free(g->n);
	free(g->sorted);
	free(g->rank);
}

/*
 * How many candidates this one reaches, along the first-parent strand and then
 * out through every extra parent.  Each candidate is counted once, which is
 * what makes this a distance rather than a subtree size.
 */
static int count_distance(struct bgraph *g, int i)
{
	int nr = 0;

	while (i >= 0) {
		struct bnode *n = &g->n[i];
		int k;

		if (n->counted)
			break;
		nr++;
		n->counted = 1;
		for (k = 0; k < n->nextra; k++)
			if (n->extra[k] >= 0)
				nr += count_distance(g, n->extra[k]);
		i = n->first;
	}
	return nr;
}

static void clear_counted(struct bgraph *g)
{
	int i;

	for (i = 0; i < g->nr; i++)
		g->n[i].counted = 0;
}

/*
 * Is this the halfway point?  For small ranges the answer is strict -- 2 and 3
 * are halfway of 5, and 3 is halfway of 6 -- and for large ones it is enough
 * to be within about a tenth of a percent, because being exact stops mattering
 * long before then and finding it exactly would cost a full pass each time.
 */
static int approx_halfway(const struct bnode *n, int nr)
{
	int diff = 2 * n->weight - nr;

	if (diff >= -1 && diff <= 1)
		return 1;
	return abs(diff) < nr / 1024;
}

/*
 * The candidate nearest half, ties going to the one seen first.  `order` is
 * oldest first, so a tie goes to the oldest -- which is what git does, and
 * what makes the sequence of probes predictable.
 */
static int best_bisection(const struct bgraph *g, const int *order, int nr)
{
	int best = order[0];
	int best_distance = -1;
	int d, k;

	for (k = 0; k < nr; k++) {
		int i = order[k];

		d = g->n[i].weight;
		if (nr - d < d)
			d = nr - d;
		if (d > best_distance) {
			best = i;
			best_distance = d;
		}
	}
	return best;
}

struct dist {
	int node;
	int distance;
};

static const struct bgraph *sort_graph;

static int dist_cmp(const void *a_, const void *b_)
{
	const struct dist *a = a_, *b = b_;

	if (a->distance != b->distance)
		return b->distance - a->distance;   /* nearest half first */
	return oid_compare(&sort_graph->n[a->node].oid,
			   &sort_graph->n[b->node].oid);
}

/*
 * The same ranking, but written out in full: with skips in play the first
 * choice may not be testable, so the ones behind it have to be known too.
 */
static void best_bisection_sorted(const struct bgraph *g, const int *order,
				  int nr, int *out)
{
	struct dist *d = xcalloc((size_t)nr, sizeof(*d));
	int k;

	for (k = 0; k < nr; k++) {
		int i = order[k];
		int w = g->n[i].weight;

		d[k].node = i;
		d[k].distance = nr - w < w ? nr - w : w;
	}
	sort_graph = g;
	qsort(d, (size_t)nr, sizeof(*d), dist_cmp);
	for (k = 0; k < nr; k++)
		out[k] = d[k].node;
	free(d);
}

/*
 * Weigh every candidate and return the one nearest half.  `out` receives the
 * order to search in, which is only more than one commit when skips are
 * involved -- then every candidate has to be ranked, because the ones nearest
 * half may turn out to be untestable.
 */
static int do_find_bisection(struct bgraph *g, const int *order, int nr,
			     int all, int *out)
{
	int counted = 0;
	int k, i;

	for (k = 0; k < nr; k++) {
		struct bnode *n = &g->n[order[k]];
		int np = interesting_parents(n);

		if (np == 0) {
			n->weight = 1;
			counted++;
		} else if (np == 1) {
			n->weight = -1;
		} else {
			n->weight = -2;
		}
	}

	/*
	 * A merge cannot inherit a distance from one parent alone, because the
	 * two parents usually reach the same ancestors and counting both would
	 * count those twice.  Merges are measured directly; anything already at
	 * half ends the search.
	 */
	for (k = 0; k < nr; k++) {
		struct bnode *n = &g->n[order[k]];

		if (n->weight != -2)
			continue;
		n->weight = count_distance(g, order[k]);
		clear_counted(g);
		if (!all && approx_halfway(n, nr)) {
			for (i = 0; i < nr; i++)
				out[i] = order[i];
			return order[k];
		}
		counted++;
	}

	/*
	 * Everything else has one parent inside the range, so its distance is
	 * that parent's plus one.  Sweeping repeatedly fills a strand inwards
	 * from the ends that can already be counted.
	 */
	while (counted < nr) {
		for (k = 0; k < nr; k++) {
			struct bnode *n = &g->n[order[k]];
			int q = -1;

			if (n->weight >= 0)
				continue;
			for (i = 0; i <= n->nextra; i++) {
				int pi = parent_at(n, i);

				if (pi >= 0 && g->n[pi].weight >= 0) {
					q = pi;
					break;
				}
			}
			if (q < 0)
				continue;
			n->weight = g->n[q].weight + 1;
			counted++;
			if (!all && approx_halfway(n, nr)) {
				for (i = 0; i < nr; i++)
					out[i] = order[i];
				return order[k];
			}
		}
	}

	for (i = 0; i < nr; i++)
		out[i] = order[i];
	if (!all)
		return best_bisection(g, order, nr);
	best_bisection_sorted(g, order, nr, out);
	return out[0];
}

/* git's own pseudo-random pick, so that skipping keeps halving rather than
 * crawling one commit at a time */
#define PRN_MODULO 32768

static unsigned get_prn(unsigned count)
{
	count = count * 1103515245u + 12345u;
	return (count / 65536) % PRN_MODULO;
}

static int sqrti(int val)
{
	float d, x = (float)val;

	if (!val)
		return 0;
	do {
		float y = (x + (float)val / x) / 2;

		d = y > x ? y - x : x - y;
		x = y;
	} while (d >= 0.5);
	return (int)x;
}

static int is_skipped(const struct oid_array *skips, const oid_t *oid)
{
	return oid_array_contains(skips, oid);
}

/* ------------------------------------------------------------------ */
/* the work tree                                                       */

/*
 * The work tree has to be untouched before a probe may replace it, and git
 * says so in the words its own checkout prints.  Only the leaving-a-bisection
 * path uses this on its own: there the index is whatever the last probe left,
 * so comparing it against the branch being returned to would call every file
 * staged.
 */
static int require_clean_worktree(struct repo *r)
{
	char **dirty = NULL;
	int n = worktree_dirty_paths(r, &dirty), i;

	if (n <= 0)
		return 0;
	gp_error("Your local changes to the following files would be "
		 "overwritten by checkout:");
	for (i = 0; i < n; i++)
		fprintf(stderr, "\t%s\n", dirty[i]);
	fprintf(stderr, "Please commit your changes or stash them before "
			"you switch branches.\nAborting\n");
	path_list_free(dirty);
	return -1;
}

static int require_clean(struct repo *r)
{
	struct index_state ist;
	oid_t head, index_tree;
	struct commit hc = COMMIT_INIT;

	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));

	if (index_has_unmerged(&ist)) {
		gp_error("cannot bisect: you have unmerged paths\n"
			 "hint: resolve them first");
		index_release(&ist);
		return -1;
	}
	if (refs_head(&r->refs, &head) == 0) {
		read_commit(r, &head, &hc);
		if (write_tree_from_index(r, &ist, &index_tree) == 0 &&
		    !oid_equal(&index_tree, &hc.tree)) {
			index_release(&ist);
			commit_release(&hc);
			gp_error("cannot bisect: you have staged changes\n"
				 "hint: commit them or unstage them first");
			return -1;
		}
		commit_release(&hc);
	}
	index_release(&ist);
	return require_clean_worktree(r);
}

/*
 * Move the work tree onto a commit and leave HEAD detached there, which is
 * what a bisection looks like from the outside: a series of detached probes,
 * with the branch to return to kept in BISECT_START.
 */
static int check_out(struct repo *r, const oid_t *oid)
{
	struct commit c = COMMIT_INIT;
	char hex[GP_SHA1_HEXSZ + 1];
	char *subj;

	if (require_clean(r))
		return -1;
	read_commit(r, oid, &c);
	refs_set_head_detached(&r->refs, oid);
	checkout_tree(r, &c.tree, 1, 1);
	commit_release(&c);
	refs_write(&r->refs, REF_EXPECTED, oid);

	oid_hex(oid, hex);
	subj = commit_subject(r, oid);
	printf("[%s] %s\n", hex, subj);
	free(subj);
	return 0;
}

/* ------------------------------------------------------------------ */

enum step {
	STEP_WAIT,              /* one end of the range is missing */
	STEP_PROBE,             /* a commit was checked out to be tested */
	STEP_FIRST_BAD,         /* the answer */
	STEP_MERGE_BASE,        /* a merge base was checked out to be tested */
	STEP_ONLY_SKIPPED,      /* nothing testable is left */
	STEP_ERROR
};

/*
 * Nothing testable is left.  The console names the skips and then the bad
 * commit itself, but the log names every commit still in the running -- the
 * two lists differ, and the log is the one a later `replay` would read.
 */
static void report_only_skipped(struct repo *r, const struct oid_array *tried,
				const struct oid_array *cands,
				const oid_t *bad)
{
	size_t i;

	printf("There are only 'skip'ped commits left to test.\n"
	       "The first bad commit could be any of:\n");
	for (i = 0; i < tried->nr; i++) {
		char hex[GP_SHA1_HEXSZ + 1];

		oid_hex(&tried->oid[i], hex);
		printf("%s\n", hex);
	}
	if (bad) {
		char hex[GP_SHA1_HEXSZ + 1];

		oid_hex(bad, hex);
		printf("%s\n", hex);
	}
	printf("We cannot bisect more!\n");

	log_append_str(r, "# only skipped commits left to test\n");
	for (i = 0; i < cands->nr; i++) {
		char hex[GP_SHA1_HEXSZ + 1];
		char *subj = commit_subject(r, &cands->oid[i]);
		char *line;

		oid_hex(&cands->oid[i], hex);
		line = xstrfmt("# possible first bad commit: [%s] %s\n", hex,
			       subj);
		log_append_str(r, line);
		free(line);
		free(subj);
	}
}

/* what check_ancestors tells its caller */
enum { ANC_OK, ANC_ERROR, ANC_MERGE_BASE };

/*
 * A good commit that the bad one cannot reach means the two ends have been
 * confused, and the real boundary is at a merge base.  git checks one out and
 * asks for it to be tested; this does the same.
 */
static int check_ancestors(struct repo *r, struct bisect_state *s)
{
	char *path = state_path(r, "BISECT_ANCESTORS_OK");
	struct buf b = BUF_INIT;
	struct buf hexes = BUF_INIT;
	size_t i, k;
	int all_ok = 1;

	buf_init(&b);
	if (read_file(path, &b) == 0) {
		buf_release(&b);
		free(path);
		return ANC_OK;
	}
	buf_release(&b);

	if (s->goods.nr)
		for (i = 0; i < s->goods.nr; i++)
			if (!is_ancestor(r, &s->goods.oid[i], &s->bad)) {
				all_ok = 0;
				break;
			}
	if (all_ok) {
		write_file(path, "\n", 1);
		free(path);
		return ANC_OK;
	}

	buf_init(&hexes);
	for (k = 0; k < s->goods.nr; k++) {
		char hex[GP_SHA1_HEXSZ + 1];

		oid_hex(&s->goods.oid[k], hex);
		if (k)
			buf_addch(&hexes, ' ');
		buf_addstr(&hexes, hex);
	}

	for (i = 0; i < s->goods.nr; i++) {
		struct oid_array bases = OID_ARRAY_INIT;

		if (is_ancestor(r, &s->goods.oid[i], &s->bad))
			continue;
		merge_bases(r, &s->bad, &s->goods.oid[i], &bases);

		for (k = 0; k < bases.nr; k++) {
			char hex[GP_SHA1_HEXSZ + 1];

			oid_hex(&bases.oid[k], hex);
			if (oid_equal(&bases.oid[k], &s->bad)) {
				fprintf(stderr, "The merge base %s is bad.\n"
					"This means the bug has been fixed "
					"between %s and [%s].\n", hex, hex,
					buf_cstr(&hexes));
				oid_array_clear(&bases);
				buf_release(&hexes);
				free(path);
				return ANC_ERROR;
			}
			if (oid_array_contains(&s->goods, &bases.oid[k]))
				continue;
			if (is_skipped(&s->skips, &bases.oid[k])) {
				fprintf(stderr, "warning: the merge base between "
					"%s and [%s] must be skipped.\n", hex,
					buf_cstr(&hexes));
				continue;
			}
			printf("Bisecting: a merge base must be tested\n");
			oid_array_clear(&bases);
			buf_release(&hexes);
			free(path);
			return check_out(r, &bases.oid[k]) < 0 ? ANC_ERROR
							       : ANC_MERGE_BASE;
		}
		oid_array_clear(&bases);
	}
	buf_release(&hexes);
	free(path);
	return ANC_OK;
}

/*
 * The answer.  Printed the way git prints it -- the commit as `show` would show
 * it -- because this is the one line anyone reads twice.
 */
static void report_first_bad(struct repo *r, const oid_t *oid)
{
	char hex[GP_SHA1_HEXSZ + 1];
	char *av[3];

	oid_hex(oid, hex);
	printf("%s is the first bad commit\n", hex);
	av[0] = (char *)"--stat";
	av[1] = hex;
	av[2] = NULL;
	cmd_show(r, 2, av);
}

static int estimate_steps(int all)
{
	int n = 0, e, x;

	if (all < 3)
		return 0;
	while ((1 << (n + 1)) <= all)
		n++;
	e = 1 << n;
	x = all - e;
	return (e < 3 * x) ? n : n - 1;
}

/*
 * Halve the range once: choose the commit, check it out, and say how much is
 * left.  The printed count is git's `all - reaches - 1` -- the candidates not
 * accounted for by the one just checked out -- and it is computed from the
 * whole range even when skips have narrowed the choice of commit.
 */
static int next_step(struct repo *r, struct bisect_state *s)
{
	struct oid_array cands = OID_ARRAY_INIT;
	struct oid_array bad_side = OID_ARRAY_INIT;
	struct oid_array excl = OID_ARRAY_INIT;
	struct oid_array tried = OID_ARRAY_INIT;
	struct bgraph g;
	int *order, *picked;
	int nr, all, reaches, chosen, k, anc;
	oid_t probe;

	anc = check_ancestors(r, s);
	if (anc == ANC_ERROR)
		return STEP_ERROR;
	if (anc == ANC_MERGE_BASE)
		return STEP_MERGE_BASE;

	commit_ancestors(r, &s->bad, &bad_side);
	for (k = 0; k < (int)s->goods.nr; k++)
		commit_ancestors(r, &s->goods.oid[k], &excl);
	for (k = 0; k < (int)bad_side.nr; k++)
		if (!oid_array_contains(&excl, &bad_side.oid[k]))
			oid_array_append(&cands, &bad_side.oid[k]);
	oid_array_clear(&bad_side);
	oid_array_clear(&excl);

	if (!cands.nr) {
		char hex[GP_SHA1_HEXSZ + 1];

		oid_hex(&s->bad, hex);
		printf("%s was both good and bad\n", hex);
		oid_array_clear(&cands);
		return STEP_ERROR;
	}

	nr = (int)cands.nr;
	graph_build(r, &cands, &g);
	order = xmalloc((size_t)nr * sizeof(*order));
	picked = xmalloc((size_t)nr * sizeof(*picked));
	/* the walk is newest first; git reverses it, so index 0 is oldest */
	for (k = 0; k < nr; k++)
		order[k] = nr - 1 - k;

	chosen = do_find_bisection(&g, order, nr, s->skips.nr > 0, picked);
	all = nr;
	reaches = g.n[chosen].weight;
	probe = g.n[chosen].oid;

	if (s->skips.nr && is_skipped(&s->skips, &g.n[picked[0]].oid)) {
		struct oid_array live = OID_ARRAY_INIT;

		/* the head of the ranking cannot be tested, so the whole
		 * ranking is filtered and one of the rest picked at random,
		 * which still halves the range on average */
		for (k = 0; k < nr; k++) {
			if (is_skipped(&s->skips, &g.n[picked[k]].oid))
				oid_array_append(&tried, &g.n[picked[k]].oid);
			else
				oid_array_append(&live, &g.n[picked[k]].oid);
		}
		if (live.nr) {
			unsigned prn = get_prn((unsigned)live.nr);
			int index = (int)(((int)live.nr * (int)prn /
					   PRN_MODULO) * sqrti((int)prn) /
					  sqrti(PRN_MODULO));
			int at = 0;

			if (index < (int)live.nr) {
				if (!oid_equal(&live.oid[index], &s->bad))
					at = index;
				else if (index > 0)
					at = index - 1;
			}
			probe = live.oid[at];
		}
		oid_array_clear(&live);
	}

	if (oid_equal(&probe, &s->bad)) {
		if (tried.nr) {
			report_only_skipped(r, &tried, &cands, &s->bad);
			oid_array_clear(&tried);
			graph_free(&g);
			free(order);
			free(picked);
			oid_array_clear(&cands);
			return STEP_ONLY_SKIPPED;
		}
		oid_array_clear(&tried);
		graph_free(&g);
		free(order);
		free(picked);
		oid_array_clear(&cands);
		report_first_bad(r, &s->bad);
		return STEP_FIRST_BAD;
	}

	{
		int left = all - reaches - 1;
		int steps = estimate_steps(all);

		printf("Bisecting: %d revision%s left to test after this "
		       "(roughly %d step%s)\n", left, left == 1 ? "" : "s",
		       steps, steps == 1 ? "" : "s");
	}

	oid_array_clear(&tried);
	graph_free(&g);
	free(order);
	free(picked);
	oid_array_clear(&cands);
	return check_out(r, &probe) < 0 ? STEP_ERROR : STEP_PROBE;
}

static void print_status(struct repo *r, struct bisect_state *s)
{
	if (s->goods.nr && s->have_bad)
		return;
	if (!s->goods.nr && !s->have_bad)
		log_printf(r, "status: waiting for both good and bad commits\n");
	else if (s->goods.nr)
		log_printf(r, "status: waiting for bad commit, %d good commit%s "
			   "known\n", (int)s->goods.nr,
			   s->goods.nr == 1 ? "" : "s");
	else
		log_printf(r, "status: waiting for good commit(s), bad commit "
			   "known\n");
}

static int auto_next(struct repo *r, struct bisect_state *s)
{
	if (!s->have_bad || !s->goods.nr) {
		print_status(r, s);
		return STEP_WAIT;
	}
	return next_step(r, s);
}

static int step_failed(int step)
{
	return step == STEP_ERROR || step == STEP_ONLY_SKIPPED;
}

/* ------------------------------------------------------------------ */
/* subcommands                                                         */

static int need_start(struct repo *r)
{
	if (state_running(r))
		return 0;
	fprintf(stderr, "You need to start by \"git bisect start\"\n");
	return 1;
}

static int resolve_commit(struct repo *r, const char *rev, oid_t *out)
{
	char *r2 = xstrfmt("%s^{commit}", rev);
	int rc = resolve_rev(r, r2, out);

	free(r2);
	return rc;
}

static int bisect_start(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct bisect_state s;
	struct buf line = BUF_INIT, meta = BUF_INIT;
	char *start_name = NULL;
	int i, rc;

	/* a pathspec would limit the bisection to commits that touched it,
	 * which is a filter this does not carry */
	for (i = 0; i < argc; i++)
		if (!strcmp(argv[i], "--")) {
			if (i + 1 < argc) {
				gp_error("bisect start: a pathspec is not "
					 "supported");
				return 1;
			}
			argc = i;
			break;
		}
	opts_init(&o, argc, argv, (const char *const[]){ NULL });

	if (state_running(r)) {
		char *p = state_path(r, "BISECT_START");
		struct buf b = BUF_INIT;

		/* the old start point is kept, and gone back to first */
		buf_init(&b);
		if (read_file(p, &b) == 0) {
			while (b.len && (b.b[b.len - 1] == '\n' ||
					 b.b[b.len - 1] == '\r'))
				b.b[--b.len] = '\0';
			start_name = xstrdup(buf_cstr(&b));
		}
		buf_release(&b);
		free(p);
	}
	if (start_name) {
		oid_t target;

		if (resolve_commit(r, start_name, &target) < 0 ||
		    check_out(r, &target) < 0) {
			gp_error("checking out '%s' failed. Try 'git bisect start "
				 "<valid-branch>'.", start_name);
			free(start_name);
			return 1;
		}
	} else {
		oid_t head;
		char *target;

		if (refs_head(&r->refs, &head) < 0) {
			gp_error("bad HEAD - I need a HEAD");
			return 1;
		}
		target = refs_head_target(&r->refs);
		if (target && !strncmp(target, "refs/heads/", 11))
			start_name = xstrdup(target + 11);
		else {
			char hex[GP_SHA1_HEXSZ + 1];

			oid_hex(&head, hex);
			start_name = xstrdup(hex);
		}
		free(target);
	}

	clean_state(r);

	{
		char *p = state_path(r, "BISECT_START");
		char *text = xstrfmt("%s\n", start_name);

		write_file(p, text, strlen(text));
		free(text);
		free(p);
	}
	{
		char *p = state_path(r, "BISECT_TERMS");

		write_file(p, "bad\ngood\n", 9);
		free(p);
	}
	{
		/* git always writes this; with the pathspec refused it is empty */
		char *p = state_path(r, "BISECT_NAMES");

		write_file(p, "\n", 1);
		free(p);
	}

	buf_init(&line);
	for (i = 0; i < o.nargs; i++) {
		const char *state = i == 0 ? TERM_BAD : TERM_GOOD;
		oid_t oid;

		if (resolve_commit(r, o.args[i], &oid) < 0) {
			gp_error("'%s' does not appear to be a valid revision",
				 o.args[i]);
			buf_release(&line);
			free(start_name);
			return 1;
		}
		bisect_write(r, state, &oid, 1);
		quote_arg(&line, o.args[i]);
	}

	buf_init(&meta);
	buf_addstr(&meta, "git bisect start");
	buf_add(&meta, line.b, line.len);
	buf_addch(&meta, '\n');
	log_append(r, (const char *)meta.b, meta.len);
	buf_release(&meta);
	buf_release(&line);
	free(start_name);

	read_state(r, &s);
	rc = auto_next(r, &s);
	state_release(&s);
	/*
	 * A start that cannot get as far as its first probe leaves nothing
	 * behind.  The range it has just written is undone, so a refusal over a
	 * dirty work tree does not also hand the user a bisection to reset.
	 */
	if (step_failed(rc)) {
		clean_state(r);
		return 1;
	}
	return 0;
}

static int bisect_mark(struct repo *r, int argc, char **argv, const char *state)
{
	struct opts o;
	struct bisect_state s;
	oid_t *oids;
	int i, n = 0, rc;

	opts_init(&o, argc, argv, (const char *const[]){ NULL });
	if (need_start(r))
		return 1;

	if (o.nargs > 1 && !strcmp(state, TERM_BAD)) {
		gp_error("'git bisect bad' can take only one argument.");
		return 1;
	}

	oids = xmalloc(((size_t)o.nargs + 1) * sizeof(*oids));
	if (!o.nargs) {
		if (refs_head(&r->refs, &oids[0]) < 0) {
			gp_error("Bad rev input: HEAD");
			free(oids);
			return 1;
		}
		n = 1;
	} else {
		for (i = 0; i < o.nargs; i++, n++)
			if (resolve_commit(r, o.args[i], &oids[n]) < 0) {
				gp_error("Bad rev input: %s", o.args[i]);
				free(oids);
				return 1;
			}
	}

	for (i = 0; i < n; i++)
		bisect_write(r, state, &oids[i], 0);
	free(oids);

	read_state(r, &s);
	rc = auto_next(r, &s);
	state_release(&s);
	return step_failed(rc) ? 1 : 0;
}

static int bisect_reset(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct buf b = BUF_INIT;
	char *start_name = NULL, *ref;
	oid_t target, old;
	int to_branch = 0;

	opts_init(&o, argc, argv, (const char *const[]){ NULL });

	if (o.nargs > 1) {
		gp_error("'gitprompt bisect reset' requires either no argument "
			 "or a commit");
		return 1;
	}

	if (o.nargs == 1) {
		start_name = xstrdup(o.args[0]);
	} else {
		char *p = state_path(r, "BISECT_START");

		buf_init(&b);
		if (read_file(p, &b) < 0 || !b.len) {
			printf("We are not bisecting.\n");
			buf_release(&b);
			free(p);
			return 0;
		}
		while (b.len && (b.b[b.len - 1] == '\n' || b.b[b.len - 1] == '\r'))
			b.b[--b.len] = '\0';
		start_name = xstrdup(buf_cstr(&b));
		buf_release(&b);
		free(p);
	}

	ref = xstrfmt("refs/heads/%s", start_name);
	to_branch = refs_read(&r->refs, ref, &target) == 0;
	if (!to_branch && resolve_commit(r, start_name, &target) < 0) {
		gp_error("'%s' is not a valid commit", start_name);
		free(ref);
		free(start_name);
		return 1;
	}

	/*
	 * The refusal comes before HEAD moves, so a reset that cannot go
	 * through leaves the bisection intact and can be tried again.
	 */
	if (require_clean_worktree(r)) {
		gp_error("could not check out original HEAD '%s'. Try "
			 "'gitprompt bisect reset <commit>'.", start_name);
		free(ref);
		free(start_name);
		return 1;
	}

	/* leaving a detached HEAD is worth saying out loud, as checkout does */
	if (refs_head(&r->refs, &old) == 0 && !refs_head_target(&r->refs) &&
	    !oid_equal(&old, &target)) {
		char *short_oid = abbrev_oid(&old);
		char *subj = commit_subject(r, &old);

		printf("Previous HEAD position was %s %s\n", short_oid, subj);
		free(short_oid);
		free(subj);
	}

	if (to_branch) {
		char *cur = refs_head_target(&r->refs);
		int already = cur && !strcmp(cur, ref);

		free(cur);
		refs_set_head(&r->refs, ref);
		if (already)
			printf("Already on '%s'\n", start_name);
		else
			printf("Switched to branch '%s'\n", start_name);
	} else {
		char *short_oid = abbrev_oid(&target);
		char *subj = commit_subject(r, &target);

		refs_set_head_detached(&r->refs, &target);
		printf("HEAD is now at %s %s\n", short_oid, subj);
		free(short_oid);
		free(subj);
	}
	free(ref);

	{
		struct commit c = COMMIT_INIT;

		read_commit(r, &target, &c);
		checkout_tree(r, &c.tree, 1, 1);
		commit_release(&c);
	}

	clean_state(r);
	free(start_name);
	return 0;
}

static int bisect_log(struct repo *r)
{
	char *p = state_path(r, "BISECT_LOG");
	struct buf b = BUF_INIT;

	buf_init(&b);
	if (read_file(p, &b) < 0 || !b.len) {
		gp_error("We are not bisecting.");
		buf_release(&b);
		free(p);
		return 1;
	}
	fwrite(b.b, 1, b.len, stdout);
	buf_release(&b);
	free(p);
	return 0;
}

/*
 * git hands the range to gitk.  Printed instead, the range is just the log the
 * commits in it would produce.
 */
static int bisect_visualize(struct repo *r)
{
	struct bisect_state s;
	struct oid_array cands = OID_ARRAY_INIT;
	struct oid_array excl = OID_ARRAY_INIT;
	size_t i, k;

	if (need_start(r))
		return 1;
	read_state(r, &s);
	if (!s.have_bad || !s.goods.nr) {
		print_status(r, &s);
		state_release(&s);
		return 1;
	}

	commit_ancestors(r, &s.bad, &cands);
	for (k = 0; k < s.goods.nr; k++)
		commit_ancestors(r, &s.goods.oid[k], &excl);
	for (i = 0; i < cands.nr; ) {
		if (oid_array_contains(&excl, &cands.oid[i])) {
			memmove(&cands.oid[i], &cands.oid[i + 1],
				(cands.nr - i - 1) * sizeof(oid_t));
			cands.nr--;
		} else {
			i++;
		}
	}
	oid_array_clear(&excl);

	for (i = 0; i < cands.nr; i++) {
		struct commit c = COMMIT_INIT;
		struct buf who = BUF_INIT, when = BUF_INIT;
		char hex[GP_SHA1_HEXSZ + 1];

		read_commit(r, &cands.oid[i], &c);
		oid_hex(&cands.oid[i], hex);
		buf_init(&who);
		buf_init(&when);
		format_author_line(c.author, &who);
		epoch_to_iso8601(commit_time(&c), &when);
		printf("commit %s\n", hex);
		printf("Author: %s\n", buf_cstr(&who));
		printf("Date:   %s\n\n", buf_cstr(&when));
		body_print_indented(c.message ? c.message : "", "    ");
		printf("\n");
		buf_release(&who);
		buf_release(&when);
		commit_release(&c);
	}

	oid_array_clear(&cands);
	state_release(&s);
	return 0;
}

/*
 * git runs the command through a shell, and that is what makes `bisect run`
 * work on a script whose first line names its interpreter -- and what makes
 * the single quotes shell_join wraps each word in mean anything.  On Unix that
 * shell is /bin/sh, which is what system() already is.  Windows has none:
 * system() hands the line to cmd.exe, which would try to run the script as a
 * program of its own and would pass the quotes through as part of the words.
 * So the line goes to the `sh` git for Windows ships instead, with cmd.exe
 * left as the fallback for a machine that has the program but no shell.
 */
static int run_status(const char *cmdline)
{
#ifdef _WIN32
	intptr_t rc = _spawnlp(_P_WAIT, "sh", "sh", "-c", cmdline, (char *)NULL);

	if (rc != -1)
		return (int)rc;
	return system(cmdline);
#else
	int rc = system(cmdline);

	if (rc < 0)
		return -1;
	if (WIFEXITED(rc))
		return WEXITSTATUS(rc);
	return -1;
#endif
}

/* the command is run through a shell, so every word is quoted */
static char *shell_join(int argc, char **argv)
{
	struct buf b = BUF_INIT;
	char *out;
	int i;

	buf_init(&b);
	for (i = 0; i < argc; i++) {
		const char *s;

		if (i)
			buf_addch(&b, ' ');
		buf_addch(&b, '\'');
		for (s = argv[i]; *s; s++) {
			if (*s == '\'')
				buf_addstr(&b, "'\\''");
			else
				buf_addch(&b, *s);
		}
		buf_addch(&b, '\'');
	}
	out = xstrdup(buf_cstr(&b));
	buf_release(&b);
	return out;
}

/*
 * Exit codes 126 and 127 come from the shell as readily as from the command: a
 * script that cannot be found, or that cannot be run, gives one of them.  Read
 * as an ordinary verdict they would narrow the range on the strength of a
 * misspelt path, so neither is believed until the command has been run once
 * more at a commit that is already known good.  Failing the same way there
 * means the command is at fault and not the commit, and the run stops instead
 * of answering.
 */
static int verify_good(struct repo *r, const char *cmd, const oid_t *good, int rc)
{
	oid_t head;
	int again;

	if (refs_head(&r->refs, &head) < 0)
		return -1;
	if (check_out(r, good) < 0)
		return -1;
	printf("running %s\n", cmd);
	fflush(stdout);
	again = run_status(cmd);
	if (check_out(r, &head) < 0)
		return -1;

	if (again < 0 || again >= 128) {
		gp_error("unable to verify %s on %s revision", cmd, TERM_GOOD);
		return -1;
	}
	if (again == rc) {
		gp_error("bogus exit code %d for %s revision", again, TERM_GOOD);
		return -1;
	}
	return 0;
}

static int bisect_run(struct repo *r, int argc, char **argv)
{
	struct opts o;
	char *cmd;
	int is_first_run = 1;

	opts_init(&o, argc, argv, (const char *const[]){ NULL });
	if (need_start(r))
		return 1;
	{
		struct bisect_state s;

		read_state(r, &s);
		if (!s.have_bad || !s.goods.nr) {
			print_status(r, &s);
			state_release(&s);
			gp_error("bisect run failed: no good and bad commits");
			return 1;
		}
		state_release(&s);
	}
	if (!o.nargs) {
		gp_error("bisect run failed: no command provided.");
		return 1;
	}
	cmd = shell_join(o.nargs, o.args);
	{
		char *p = state_path(r, "BISECT_RUN");
		struct buf b;

		buf_init(&b);
		buf_addstr(&b, cmd);
		buf_addch(&b, '\n');
		write_file(p, b.b, b.len);
		buf_release(&b);
		free(p);
	}

	for (;;) {
		int rc, step;
		const char *state;
		struct bisect_state s;
		oid_t head;

		printf("running %s\n", cmd);
		fflush(stdout);
		rc = run_status(cmd);
		if (is_first_run && (rc == 126 || rc == 127)) {
			is_first_run = 0;
			read_state(r, &s);
			if (verify_good(r, cmd, &s.goods.oid[0], rc) < 0) {
				state_release(&s);
				free(cmd);
				return 1;
			}
			state_release(&s);
		}
		if (rc < 0 || rc >= 128) {
			gp_error("bisect run failed: exit code %d from %s is "
				 "< 0 or >= 128", rc, cmd);
			free(cmd);
			return 1;
		}
		if (rc == 125)
			state = "skip";
		else if (rc == 0)
			state = TERM_GOOD;
		else
			state = TERM_BAD;

		if (refs_head(&r->refs, &head) < 0) {
			gp_error("bisect run: HEAD is gone");
			free(cmd);
			return 1;
		}
		bisect_write(r, state, &head, 0);

		read_state(r, &s);
		step = auto_next(r, &s);
		state_release(&s);

		if (step == STEP_FIRST_BAD) {
			printf("bisect found first bad commit\n");
			free(cmd);
			return 0;
		}
		if (step == STEP_MERGE_BASE) {
			printf("bisect run success\n");
			free(cmd);
			return 0;
		}
		if (step == STEP_ONLY_SKIPPED) {
			gp_error("bisect run cannot continue any more");
			free(cmd);
			return 1;
		}
		if (step != STEP_PROBE) {
			gp_error("bisect run failed: 'git bisect %s' could not "
				 "continue", state);
			free(cmd);
			return 1;
		}
	}
}

/* ------------------------------------------------------------------ */

static int usage(void)
{
	fprintf(stderr,
		"usage: gitprompt bisect <subcommand> [<options>]\n"
		"\n"
		"    start [<bad> [<good>...]]   begin a bisection\n"
		"    good [<rev>...]             mark commits as good\n"
		"    bad [<rev>]                 mark a commit as bad\n"
		"    skip [<rev>...]             commit cannot be tested\n"
		"    run <cmd> [<args>...]       bisect by running a command\n"
		"    visualize                   show the range still to test\n"
		"    log                         show the steps so far\n"
		"    reset [<commit>]            end it and go back\n");
	return 2;
}

int cmd_bisect(struct repo *r, int argc, char **argv)
{
	const char *sub;

	if (argc < 1)
		return usage();
	sub = argv[0];
	argc--;
	argv++;

	if (!strcmp(sub, "start"))
		return bisect_start(r, argc, argv);
	if (!strcmp(sub, "good"))
		return bisect_mark(r, argc, argv, TERM_GOOD);
	if (!strcmp(sub, "bad"))
		return bisect_mark(r, argc, argv, TERM_BAD);
	if (!strcmp(sub, "skip"))
		return bisect_mark(r, argc, argv, "skip");
	if (!strcmp(sub, "reset"))
		return bisect_reset(r, argc, argv);
	if (!strcmp(sub, "log"))
		return bisect_log(r);
	if (!strcmp(sub, "run"))
		return bisect_run(r, argc, argv);
	if (!strcmp(sub, "visualize") || !strcmp(sub, "view"))
		return bisect_visualize(r);
	if (!strcmp(sub, "replay") || !strcmp(sub, "terms")) {
		gp_error("bisect: '%s' is not supported", sub);
		return 1;
	}
	gp_error("bisect: unknown subcommand: %s", sub);
	return 1;
}
