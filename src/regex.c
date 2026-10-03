/*
 * regex.c - a small regular expression engine.
 *
 * `git grep` matches with whatever regex library the system provides.  This
 * build has to behave the same on the three platforms it is tested on, and one
 * of them -- the TDM-GCC used for development -- ships no POSIX <regex.h> at
 * all, only the C++ one.  So the engine lives here: a compile step that turns a
 * pattern into a tree, and a backtracking match over that tree that is the same
 * everywhere.
 *
 * What it covers is what git grep's patterns actually use: literals, `.`,
 * bracket expressions, the two anchors, grouping, alternation, and the three
 * repetition operators -- in both the basic syntax (git's default, where
 * `+ ? | ( ) {` are ordinary characters and `\+ \? \| \( \) \{` are the
 * operators) and the extended one (`-E`, where it is the other way round).
 *
 * What it does not cover it refuses when the pattern is compiled, rather than
 * quietly answering something else: the interval `{n,m}`, back references `\1`,
 * POSIX character classes such as `[[:alpha:]]`, the word-boundary escapes
 * `\< \> \b`, and repetition of an expression that can match nothing at all
 * (`(a*)*`), which is the one construct that can hang a backtracking matcher.
 * A refusal is a 128 and a message; the alternative is a wrong answer.
 */
#include "gp.h"

#include <ctype.h>
#include <string.h>

enum rx_kind {
	RX_CHAR,                /* one literal character */
	RX_ANY,                 /* . */
	RX_CLASS,               /* [...] */
	RX_BOL,                 /* ^ */
	RX_EOL,                 /* $ */
	RX_SEQ,                 /* the kids in order */
	RX_ALT,                 /* any one of the kids */
	RX_REP                  /* sub, repeated min..max times */
};

struct rx_node {
	enum rx_kind kind;
	unsigned char ch;               /* RX_CHAR */
	unsigned char set[32];          /* RX_CLASS: one bit per byte value */
	struct rx_node **kids;          /* RX_SEQ, RX_ALT */
	size_t nkids;
	struct rx_node *sub;            /* RX_REP */
	int min, max;                   /* RX_REP; max < 0 means unbounded */
};

struct rx {
	struct rx_node *root;
	int icase;
};

/* ------------------------------------------------------------------ */
/* compiling                                                           */

struct rx_parse {
	const char *p;
	size_t pos;
	int ere;
	int icase;
	char **err;
	int failed;
};

static void rx_free_node(struct rx_node *n)
{
	size_t i;

	if (!n)
		return;
	for (i = 0; i < n->nkids; i++)
		rx_free_node(n->kids[i]);
	free(n->kids);
	rx_free_node(n->sub);
	free(n);
}

static void rx_fail(struct rx_parse *p, const char *msg)
{
	if (p->failed)
		return;
	p->failed = 1;
	free(*p->err);
	*p->err = xstrfmt("%s", msg);
}

static int rx_peek(const struct rx_parse *p)
{
	return (unsigned char)p->p[p->pos];
}

static int rx_peek2(const struct rx_parse *p)
{
	return p->p[p->pos] ? (unsigned char)p->p[p->pos + 1] : 0;
}

/*
 * Is the operator `op` the next thing in the pattern?  `*` is bare in both
 * syntaxes; the others are bare in the extended one and written `\op` in the
 * basic one.  Either way the operator is consumed.
 */
static int rx_eat(struct rx_parse *p)
{
	return (unsigned char)p->p[p->pos++];
}

static int rx_take_bare(struct rx_parse *p, int op)
{
	if (rx_peek(p) != op)
		return 0;
	rx_eat(p);
	return 1;
}

static int rx_take_meta(struct rx_parse *p, int op)
{
	if (p->ere)
		return rx_take_bare(p, op);
	if (rx_peek(p) != '\\' || rx_peek2(p) != op)
		return 0;
	p->pos += 2;
	return 1;
}

/* the same question, without consuming anything */
static int rx_has_meta(const struct rx_parse *p, int op)
{
	if (p->ere)
		return rx_peek(p) == op;
	return rx_peek(p) == '\\' && rx_peek2(p) == op;
}

static struct rx_node *rx_new(enum rx_kind kind)
{
	struct rx_node *n = xcalloc(1, sizeof(*n));

	n->kind = kind;
	return n;
}

static void rx_add(struct rx_node *n, struct rx_node *kid)
{
	n->kids = xrealloc(n->kids, (n->nkids + 1) * sizeof(*n->kids));
	n->kids[n->nkids++] = kid;
}

static void rx_class_bit(struct rx_node *n, unsigned char c, int icase)
{
	n->set[c >> 3] |= (unsigned char)(1u << (c & 7));
	if (icase) {
		unsigned char lo = (unsigned char)tolower(c);
		unsigned char up = (unsigned char)toupper(c);

		n->set[lo >> 3] |= (unsigned char)(1u << (lo & 7));
		n->set[up >> 3] |= (unsigned char)(1u << (up & 7));
	}
}

/* the `[` has already been eaten */
static struct rx_node *rx_parse_class(struct rx_parse *p)
{
	struct rx_node *n = rx_new(RX_CLASS);
	int negate = 0;
	size_t i;

	if (rx_peek(p) == '^') {
		negate = 1;
		rx_eat(p);
	}
	/* a `]` in the first position stands for itself */
	if (rx_peek(p) == ']') {
		rx_class_bit(n, ']', p->icase);
		rx_eat(p);
	}

	for (;;) {
		int c = rx_peek(p);
		int lo;

		if (c == 0) {
			rx_fail(p, "bracket expression is never closed");
			rx_free_node(n);
			return NULL;
		}
		if (c == ']') {
			rx_eat(p);
			break;
		}
		if (c == '[' && rx_peek2(p) == ':') {
			rx_fail(p,
				"POSIX character classes such as [[:alpha:]] are not supported");
			rx_free_node(n);
			return NULL;
		}

		lo = rx_eat(p);
		if (rx_peek(p) == '-' && rx_peek2(p) != ']' && rx_peek2(p) != 0) {
			int hi;

			rx_eat(p);      /* the dash */
			hi = rx_eat(p);
			if (hi < lo) {
				int t = lo;
				lo = hi;
				hi = t;
			}
			for (c = lo; c <= hi; c++)
				rx_class_bit(n, (unsigned char)c, p->icase);
		} else {
			rx_class_bit(n, (unsigned char)lo, p->icase);
		}
	}

	if (negate)
		for (i = 0; i < sizeof(n->set); i++)
			n->set[i] = (unsigned char)~n->set[i];
	return n;
}

static struct rx_node *rx_parse_alt(struct rx_parse *p);

/*
 * Is `$` about to end a branch?  POSIX makes `$` an anchor only where the
 * branch ends; anywhere else -- `a$b` -- it is an ordinary character, and so it
 * is in the system regexes, which is what git grep uses.
 */
static int rx_at_branch_end(const struct rx_parse *p)
{
	return rx_peek(p) == 0 || rx_has_meta(p, '|') || rx_has_meta(p, ')');
}

static struct rx_node *rx_parse_atom(struct rx_parse *p, int at_start)
{
	struct rx_node *n;
	int c = rx_peek(p);

	if (c == 0)
		return NULL;

	if (rx_take_meta(p, '(')) {
		n = rx_parse_alt(p);
		if (p->failed) {
			rx_free_node(n);
			return NULL;
		}
		if (!rx_take_meta(p, ')')) {
			rx_fail(p, "unmatched (");
			rx_free_node(n);
			return NULL;
		}
		return n;
	}

	if (c == '.') {
		rx_eat(p);
		return rx_new(RX_ANY);
	}
	if (c == '^') {
		rx_eat(p);
		if (at_start)
			return rx_new(RX_BOL);
		n = rx_new(RX_CHAR);
		n->ch = '^';
		return n;
	}
	if (c == '$') {
		rx_eat(p);
		if (rx_at_branch_end(p))
			return rx_new(RX_EOL);
		n = rx_new(RX_CHAR);
		n->ch = '$';
		return n;
	}
	if (c == '[') {
		rx_eat(p);
		return rx_parse_class(p);
	}

	/* an interval is refused whether it is spelled `{` (extended) or `\{`
	 * (basic); a bare `{` in the basic syntax is an ordinary character */
	if ((p->ere && c == '{') || (!p->ere && c == '\\' && rx_peek2(p) == '{')) {
		rx_fail(p, "interval expressions {n,m} are not supported");
		return NULL;
	}

	if (c == '\\') {
		int e;

		rx_eat(p);
		e = rx_peek(p);
		if (e == 0) {
			rx_fail(p, "pattern ends with a bare backslash");
			return NULL;
		}
		if (e >= '1' && e <= '9') {
			rx_fail(p, "back references are not supported");
			return NULL;
		}
		if (e == '<' || e == '>' || e == 'b' || e == 'B') {
			rx_fail(p, "the word-boundary escapes \\< and \\> are not supported");
			return NULL;
		}
		rx_eat(p);
		n = rx_new(RX_CHAR);
		n->ch = (unsigned char)e;
		return n;
	}

	/* a repetition operator with nothing to its left is an ordinary
	 * character, which is what the system regexes do with it too */
	rx_eat(p);
	n = rx_new(RX_CHAR);
	n->ch = (unsigned char)c;
	return n;
}

/* can this node match the empty string? */
static int rx_nullable(const struct rx_node *n)
{
	size_t i;

	switch (n->kind) {
	case RX_CHAR:
	case RX_ANY:
	case RX_CLASS:
		return 0;
	case RX_BOL:
	case RX_EOL:
		return 1;
	case RX_SEQ:
		for (i = 0; i < n->nkids; i++)
			if (!rx_nullable(n->kids[i]))
				return 0;
		return 1;
	case RX_ALT:
		for (i = 0; i < n->nkids; i++)
			if (rx_nullable(n->kids[i]))
				return 1;
		return 0;
	case RX_REP:
		return n->min == 0 || rx_nullable(n->sub);
	}
	return 0;
}

static struct rx_node *rx_parse_seq(struct rx_parse *p)
{
	struct rx_node *seq = rx_new(RX_SEQ);
	int first = 1;

	for (;;) {
		struct rx_node *atom;

		if (rx_peek(p) == 0 || rx_has_meta(p, '|') || rx_has_meta(p, ')'))
			break;

		atom = rx_parse_atom(p, first);
		if (p->failed) {
			rx_free_node(atom);
			rx_free_node(seq);
			return NULL;
		}
		if (!atom)
			break;
		first = 0;

		for (;;) {
			int min, max;

			if (rx_take_bare(p, '*')) {
				min = 0;
				max = -1;
			} else if (rx_take_meta(p, '+')) {
				min = 1;
				max = -1;
			} else if (rx_take_meta(p, '?')) {
				min = 0;
				max = 1;
			} else {
				break;
			}

			/*
			 * `(a*)*` and its relatives are the one pattern that can
			 * make a backtracking matcher run forever; the system
			 * regexes define it away, and so does this one.
			 */
			if (max != 1 && rx_nullable(atom)) {
				rx_fail(p, "repetition of an expression that can match nothing");
				rx_free_node(atom);
				rx_free_node(seq);
				return NULL;
			}

			{
				struct rx_node *rep = rx_new(RX_REP);

				rep->sub = atom;
				rep->min = min;
				rep->max = max;
				atom = rep;
			}
		}

		rx_add(seq, atom);
	}

	return seq;
}

static struct rx_node *rx_parse_alt(struct rx_parse *p)
{
	struct rx_node *first = rx_parse_seq(p);
	struct rx_node *alt;

	if (p->failed) {
		rx_free_node(first);
		return NULL;
	}
	if (!rx_take_meta(p, '|'))
		return first;

	alt = rx_new(RX_ALT);
	rx_add(alt, first);
	for (;;) {
		struct rx_node *next = rx_parse_seq(p);

		if (p->failed) {
			rx_free_node(next);
			rx_free_node(alt);
			return NULL;
		}
		rx_add(alt, next);
		if (!rx_take_meta(p, '|'))
			break;
	}
	return alt;
}

int rx_compile(struct rx **out, const char *pattern, int ere, int icase, char **err)
{
	struct rx_parse p;
	struct rx_node *root;
	struct rx *re;

	*out = NULL;
	*err = NULL;

	memset(&p, 0, sizeof(p));
	p.p = pattern;
	p.ere = ere;
	p.icase = icase;
	p.err = err;

	root = rx_parse_alt(&p);
	if (p.failed) {
		rx_free_node(root);
		return -1;
	}
	if (rx_peek(&p) != 0) {
		char msg[64];

		snprintf(msg, sizeof(msg), "unmatched %c", rx_peek(&p));
		rx_fail(&p, msg);
		rx_free_node(root);
		return -1;
	}

	re = xcalloc(1, sizeof(*re));
	re->root = root;
	re->icase = icase;
	*out = re;
	return 0;
}

/*
 * -F: every byte of the pattern stands for itself.  Rather than a second
 * matcher, the pattern is rewritten with the basic syntax's metacharacters
 * escaped -- those are only `. [ ^ $ * \`; `+ ? | ( )` and a bare `{` are not
 * special there -- and compiled as an ordinary basic pattern.
 */
int rx_compile_fixed(struct rx **out, const char *pattern, int icase, char **err)
{
	static const char special[] = ".[^$*\\";
	struct buf b = BUF_INIT;
	const char *s;
	int rc;

	for (s = pattern; *s; s++) {
		if (strchr(special, *s))
			buf_addf(&b, "\\%c", *s);
		else
			buf_add(&b, s, 1);
	}
	rc = rx_compile(out, buf_cstr(&b), 0, icase, err);
	buf_release(&b);
	return rc;
}

void rx_release(struct rx *re)
{
	if (!re)
		return;
	rx_free_node(re->root);
	free(re);
}

/* ------------------------------------------------------------------ */
/* matching                                                            */

/*
 * Matching is a backtracking walk with a continuation: `k` is what to do with
 * a position once the node has matched there, and it returns non-zero to say
 * "this is the match, stop looking".  That shape is what makes `-w` possible:
 * the caller can reject a match whose edges are not at word boundaries, and
 * the walk then goes on to try a different one.
 */
typedef int (*rx_cont)(void *ctx, size_t pos);

/*
 * Two guards against a pattern that would take longer than anyone wants to
 * wait.  The first is the stack: a repetition whose body is not a single byte
 * recurses once per repetition, so the depth grows with the line.  The second
 * is the work, which for a pattern like `.*foo` on a line with no `foo` grows
 * with the square of the line -- every start position rescans it.  Either one
 * running out is reported as an error rather than as "no match", because a
 * wrong answer is the worse outcome.
 */
#define RX_MAX_DEPTH 2000
#define RX_STEP_BASE 2000
#define RX_STEP_QUAD 4
#define RX_STEP_MAX 100000000L

static int rx_depth;
static long rx_steps;
static long rx_step_limit;
static int rx_gave_up;

static int rx_m(const struct rx_node *n, const char *s, size_t len, size_t i,
		rx_cont k, void *kctx, int icase);

struct rx_seq_ctx {
	const struct rx_node *node;
	size_t idx;
	const char *s;
	size_t len;
	rx_cont k;
	void *kctx;
	int icase;
};

static int rx_seq_step(void *ctx, size_t pos)
{
	struct rx_seq_ctx *c = (struct rx_seq_ctx *)ctx;
	struct rx_seq_ctx next;

	if (c->idx == c->node->nkids)
		return c->k(c->kctx, pos);

	next = *c;
	next.idx = c->idx + 1;
	return rx_m(c->node->kids[c->idx], c->s, c->len, pos,
		    rx_seq_step, &next, c->icase);
}

static int rx_fold(int c, int icase)
{
	return icase ? tolower((unsigned char)c) : c;
}

/* does `n` match one byte at s[i]?  Called with i < len; n is a one-byte node */
static int rx_one(const struct rx_node *n, const char *s, size_t i, int icase)
{
	unsigned char c = (unsigned char)s[i];

	switch (n->kind) {
	case RX_CHAR:
		return rx_fold(c, icase) == rx_fold(n->ch, icase);
	case RX_ANY:
		return c != '\n';
	case RX_CLASS:
		return (n->set[c >> 3] & (1u << (c & 7))) != 0;
	default:
		break;
	}
	return 0;
}

struct rx_rep_ctx {
	const struct rx_node *sub;
	int min, max, count;
	size_t start;
	const char *s;
	size_t len;
	rx_cont k;
	void *kctx;
	int icase;
};

static int rx_rep_step(void *ctx, size_t pos);

/*
 * A repetition whose body consumes exactly one byte -- `.*`, `a*`, `[0-9]+` --
 * is walked with a loop instead of a recursion, because otherwise the stack
 * grows with the length of the line, and a line has no bound.  Positions are
 * recorded and the continuation is offered them longest first.
 */
static int rx_rep_one(const struct rx_node *sub, int min, int max,
		      const char *s, size_t len, size_t i,
		      rx_cont k, void *kctx, int icase)
{
	size_t cap = len - i + 1;
	size_t *pos = xmalloc(cap * sizeof(*pos));
	size_t n = 0, j;
	int r = 0;

	pos[0] = i;
	while (i < len && (max < 0 || n < (size_t)max) &&
	       rx_one(sub, s, i, icase)) {
		i++;
		pos[++n] = i;
	}

	for (j = n + 1; j-- > 0; ) {
		if (j < (size_t)min)
			break;
		if (++rx_steps > rx_step_limit) {
			rx_gave_up = 1;
			break;
		}
		if (k(kctx, pos[j])) {
			r = 1;
			break;
		}
	}

	free(pos);
	return r;
}

static int rx_rep_go(const struct rx_node *sub, int min, int max, int count,
		     const char *s, size_t len, size_t i,
		     rx_cont k, void *kctx, int icase)
{
	struct rx_rep_ctx c;

	if (count == 0 && (sub->kind == RX_CHAR || sub->kind == RX_ANY ||
			   sub->kind == RX_CLASS))
		return rx_rep_one(sub, min, max, s, len, i, k, kctx, icase);

	c.sub = sub;
	c.min = min;
	c.max = max;
	c.count = count;
	c.start = i;
	c.s = s;
	c.len = len;
	c.k = k;
	c.kctx = kctx;
	c.icase = icase;

	if (count < min)
		return rx_m(sub, s, len, i, rx_rep_step, &c, icase);

	/* enough repetitions: stop here, and if that leads nowhere, take
	 * another one */
	if (k(kctx, i))
		return 1;
	if (max < 0 || count < max)
		return rx_m(sub, s, len, i, rx_rep_step, &c, icase);
	return 0;
}

static int rx_rep_step(void *ctx, size_t pos)
{
	struct rx_rep_ctx *c = (struct rx_rep_ctx *)ctx;

	/* the body matched nothing, so another turn would stand still */
	if (pos == c->start)
		return 0;
	return rx_rep_go(c->sub, c->min, c->max, c->count + 1,
			 c->s, c->len, pos, c->k, c->kctx, c->icase);
}

static int rx_match(const struct rx_node *n, const char *s, size_t len, size_t i,
		    rx_cont k, void *kctx, int icase)
{
	switch (n->kind) {
	case RX_CHAR:
		if (i >= len || rx_fold((unsigned char)s[i], icase) !=
		    rx_fold(n->ch, icase))
			return 0;
		return k(kctx, i + 1);

	case RX_ANY:
		if (i >= len || s[i] == '\n')
			return 0;
		return k(kctx, i + 1);

	case RX_CLASS: {
		unsigned char c;

		if (i >= len)
			return 0;
		c = (unsigned char)s[i];
		if (!(n->set[c >> 3] & (1u << (c & 7))))
			return 0;
		return k(kctx, i + 1);
	}

	case RX_BOL:
		return i == 0 ? k(kctx, i) : 0;

	case RX_EOL:
		return i == len ? k(kctx, i) : 0;

	case RX_SEQ: {
		struct rx_seq_ctx c;

		c.node = n;
		c.idx = 0;
		c.s = s;
		c.len = len;
		c.k = k;
		c.kctx = kctx;
		c.icase = icase;
		return rx_seq_step(&c, i);
	}

	case RX_ALT: {
		size_t j;

		for (j = 0; j < n->nkids; j++)
			if (rx_match(n->kids[j], s, len, i, k, kctx, icase))
				return 1;
		return 0;
	}

	case RX_REP:
		return rx_rep_go(n->sub, n->min, n->max, 0, s, len, i,
				 k, kctx, icase);
	}
	return 0;
}

static int rx_m(const struct rx_node *n, const char *s, size_t len, size_t i,
		rx_cont k, void *kctx, int icase)
{
	int r;

	if (rx_gave_up)
		return 0;
	if (rx_depth >= RX_MAX_DEPTH || ++rx_steps > rx_step_limit) {
		rx_gave_up = 1;
		return 0;
	}

	rx_depth++;
	r = rx_match(n, s, len, i, k, kctx, icase);
	rx_depth--;
	return r;
}

struct rx_found {
	const char *s;
	size_t len;
	size_t start;
	size_t end;
};

static int rx_found_at(void *ctx, size_t pos)
{
	struct rx_found *f = (struct rx_found *)ctx;

	f->end = pos;
	return 1;
}

int rx_search(const struct rx *re, const char *s, size_t len, size_t from,
	      size_t *ms, size_t *me)
{
	struct rx_found f;
	size_t i;

	rx_depth = 0;
	rx_steps = 0;
	rx_gave_up = 0;
	rx_step_limit = RX_STEP_BASE +
			RX_STEP_QUAD * (long)(len + 1) * (long)(len + 1);
	if (rx_step_limit > RX_STEP_MAX)
		rx_step_limit = RX_STEP_MAX;

	f.s = s;
	f.len = len;

	for (i = from; i <= len; i++) {
		f.start = i;
		f.end = i;
		if (rx_m(re->root, s, len, i, rx_found_at, &f, re->icase)) {
			*ms = i;
			*me = f.end;
			return 1;
		}
		if (rx_gave_up)
			return -1;
	}
	return 0;
}

static int rx_is_word(int c)
{
	return isalnum((unsigned char)c) || c == '_';
}

static int rx_word_bounded(const char *s, size_t len, size_t a, size_t b)
{
	return (a == 0 || !rx_is_word((unsigned char)s[a - 1])) &&
	       (b == len || !rx_is_word((unsigned char)s[b]));
}

/*
 * Is there a match of any of the patterns, with `-w` if asked for?  The retry
 * is the one git does: a match whose edges are not at word boundaries is not
 * good enough, so the search picks up from where that match ended and tries
 * again.  (Advancing by a whole match rather than by one byte is what keeps
 * `-w alpha` from finding the `alpha` inside `alphabet`.)
 */
int rx_search_any(struct rx *const *res, size_t n, const char *s, size_t len,
		  int word, size_t *ms, size_t *me)
{
	size_t j;

	for (j = 0; j < n; j++) {
		size_t from = 0;

		for (;;) {
			size_t a, b;
			int r = rx_search(res[j], s, len, from, &a, &b);

			if (r < 0)
				return -1;
			if (r == 0)
				break;
			if (!word || rx_word_bounded(s, len, a, b)) {
				*ms = a;
				*me = b;
				return 1;
			}
			from = (b > a) ? b : a + 1;
			if (from > len)
				break;
		}
	}
	return 0;
}
