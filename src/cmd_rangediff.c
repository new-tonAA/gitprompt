/*
 * range-diff -- two versions of the same series, told apart.
 *
 * A series that was rebased, or reworked after review, is the same work
 * written twice: the commits line up one for one, most of them carrying the
 * change they carried before and a few of them carrying something else.  git's
 * range-diff shows that lining up, and this is that command: one line per
 * pair, the mark in the middle saying whether the two are the same change
 * (=), a change that differs (!), or a commit only one side has (< or >).
 *
 * The comparison is by a patch id of this command's own making: the paths, the
 * modes and the removed and added lines, with hunk positions and blob names
 * left out -- which is what lets a commit whose base moved up still count as
 * the same change.  It is not git's patch-id and does not claim to be; it is
 * only ever compared against another one of ours, within one run.
 *
 * The pairing is the same idea in two steps.  First the commits whose patch id
 * is equal and whose message is equal, marked =.  What is left is then lined
 * up in order and marked ! -- but only where the two patches have at least
 * half their changed lines in common, so that a commit one side has and the
 * other does not is left as < or > rather than called a version of whatever
 * happens to sit opposite it.  git weighs a cost against a creation factor
 * there instead, so the two can still disagree about a pair that sits near the
 * line; that is why the surface suite compares the marks on a series where
 * both agree.  The body printed under a ! pair is this command's diff of the
 * two commits as it holds them, message then patch; git diffs its own
 * rendering of the two commits there instead.
 *
 * Three argument forms, as in git: <base>..<tip> <base>..<tip>, a single
 * <tip1>...<tip2> whose base is the merge base of the two, and <base> <tip1>
 * <tip2>.  The ... form is read here rather than through the revision parser,
 * whose reading of A...B is the symmetric difference -- both sides at once --
 * which is not what a range is in this command.
 */
#include "gp.h"
#include <stdlib.h>
#include <string.h>

/* one changed line of a patch: what is left of it once the -/+ is taken off */
struct rd_cline {
	const char *p;
	size_t len;
};

struct rd_commit {
	oid_t oid;
	char hex[8];
	char *subject;
	char *message;
	struct buf patch;
	u8 patch_id[GP_SHA1_RAWSZ];
	struct rd_cline *lines;  /* the - and + lines, into patch */
	size_t nr_lines, cap_lines;
	long pair;              /* the other side's index, or -1 */
	char mark;              /* '=', '!', '<', '>' */
};

struct rd_side {
	struct rd_commit *c;
	size_t nr, cap;
};
#define RD_SIDE_INIT { NULL, 0, 0 }

struct rd_ctx {
	struct repo *r;
	struct rd_side *side;
};

static struct rd_commit *rd_push(struct rd_side *s)
{
	if (s->nr == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 16;
		s->c = xrealloc(s->c, s->cap * sizeof *s->c);
	}
	memset(&s->c[s->nr], 0, sizeof s->c[s->nr]);
	s->c[s->nr].pair = -1;
	return &s->c[s->nr++];
}

/* ------------------------------------------------------------------ */
/* the patch id                                                        */

/*
 * A number in a hunk header, or in a mode.  The line is a slice of a buffer
 * that ends in a newline, so a scan that runs past the digits stops there
 * rather than off the end.
 */
static long rd_num(const char **pp)
{
	const char *p = *pp;
	long v = 0;

	while (*p >= '0' && *p <= '9')
		v = v * 10 + (*p++ - '0');
	*pp = p;
	return v;
}

/*
 * What makes two patches the same change: the file each names, whether it was
 * created or deleted, the modes, and the lines taken out and put in.  What it
 * leaves out is the position of a hunk and the names of the blobs on either
 * side, which move when the base moves without the change itself changing.
 */
static void rd_patch_id(struct rd_commit *rc)
{
	gp_sha1_ctx ctx;
	const struct buf *patch = &rc->patch;
	const char *s = (const char *)patch->b;
	const char *end = s + patch->len;
	long rem_a = 0, rem_b = 0;
	int in_hunk = 0;

	gp_sha1_init(&ctx);
	while (s < end) {
		const char *nl = memchr(s, '\n', (size_t)(end - s));
		size_t ll = nl ? (size_t)(nl - s) : (size_t)(end - s);

		if (in_hunk && rem_a <= 0 && rem_b <= 0)
			in_hunk = 0;

		if (in_hunk) {
			if (s[0] == '-' || s[0] == '+' || s[0] == '\\') {
				gp_sha1_update(&ctx, s, ll);
				gp_sha1_update(&ctx, "\n", 1);
			}
			if (s[0] == '-' || s[0] == '+') {
				if (rc->nr_lines == rc->cap_lines) {
					rc->cap_lines = rc->cap_lines ? rc->cap_lines * 2 : 8;
					rc->lines = xrealloc(rc->lines, rc->cap_lines *
							     sizeof *rc->lines);
				}
				rc->lines[rc->nr_lines].p = s + 1;
				rc->lines[rc->nr_lines].len = ll - 1;
				rc->nr_lines++;
			}
			if (s[0] == '-')
				rem_a--;
			else if (s[0] == '+')
				rem_b--;
			else if (s[0] == ' ')
				rem_a--, rem_b--;
		} else if (ll > 3 && s[0] == '@' && s[1] == '@') {
			const char *p = s + 2;

			while (*p == ' ')
				p++;
			if (*p == '-') {
				p++;
				rd_num(&p);           /* the first line, not needed */
				rem_a = (*p == ',') ? (p++, rd_num(&p)) : 1;
			}
			while (*p == ' ')
				p++;
			if (*p == '+') {
				p++;
				rd_num(&p);
				rem_b = (*p == ',') ? (p++, rd_num(&p)) : 1;
			}
			in_hunk = 1;
		} else if (ll > 11 && !memcmp(s, "diff --git ", 11)) {
			/* the path, and for a rename both of them */
			gp_sha1_update(&ctx, s, ll);
			gp_sha1_update(&ctx, "\n", 1);
		} else if (!memcmp(s, "new file mode ", 14) ||
			   !memcmp(s, "deleted file mode ", 18) ||
			   !memcmp(s, "old mode ", 9) ||
			   !memcmp(s, "new mode ", 9) ||
			   !memcmp(s, "rename from ", 12) ||
			   !memcmp(s, "rename to ", 10) ||
			   !memcmp(s, "copy from ", 10) ||
			   !memcmp(s, "copy to ", 8)) {
			gp_sha1_update(&ctx, s, ll);
			gp_sha1_update(&ctx, "\n", 1);
		}
		s = nl ? nl + 1 : end;
	}
	gp_sha1_final(&ctx, rc->patch_id);
}

/* ------------------------------------------------------------------ */
/* collecting one side                                                 */

static void rd_one(const oid_t *oid, const struct commit *commit, void *ud)
{
	struct rd_ctx *ctx = ud;
	struct rd_commit *rc = rd_push(ctx->side);
	oid_t ptree;
	int has_parent = 0;

	rc->oid = *oid;
	oid_hex(oid, rc->hex);
	rc->hex[7] = '\0';
	rc->subject = commit_message_line(commit);
	rc->message = xstrdup(commit->message ? commit->message : "");
	buf_init(&rc->patch);

	/* a root commit's patch is against nothing, as in git */
	if (commit->parents.nr > 0) {
		struct commit pc = COMMIT_INIT;

		read_commit(ctx->r, &commit->parents.oid[0], &pc);
		ptree = pc.tree;
		has_parent = 1;
		commit_release(&pc);
	}
	diff_trees(ctx->r, has_parent ? &ptree : NULL, &commit->tree,
		   &rc->patch, 0);
	rd_patch_id(rc);
}

/* the commits base..tip, oldest first, which is the order they are numbered */
static void rd_collect(struct repo *r, const oid_t *base, const oid_t *tip,
		       struct rd_side *side)
{
	struct rev_list rl;
	struct rd_ctx ctx;
	size_t i;

	memset(&rl, 0, sizeof rl);
	oid_array_append(&rl.include, tip);
	oid_array_append(&rl.exclude, base);

	ctx.r = r;
	ctx.side = side;
	rev_list_run(r, &rl, rd_one, &ctx);
	rev_list_release(&rl);

	/* the walk is newest first; git numbers a series from its base up */
	for (i = 0; i < side->nr / 2; i++) {
		struct rd_commit t = side->c[i];

		side->c[i] = side->c[side->nr - 1 - i];
		side->c[side->nr - 1 - i] = t;
	}
}

static void rd_side_release(struct rd_side *s)
{
	size_t i;

	for (i = 0; i < s->nr; i++) {
		free(s->c[i].subject);
		free(s->c[i].message);
		free(s->c[i].lines);
		buf_release(&s->c[i].patch);
	}
	free(s->c);
}

/* ------------------------------------------------------------------ */
/* pairing                                                             */

/*
 * How much two patches have in common: the changed lines they share.  A pair
 * is only called a changed change when that is at least half of the longer
 * patch, so that two commits that merely sit opposite each other are not
 * claimed to be versions of one another.
 */
static int rd_alike(const struct rd_commit *a, const struct rd_commit *b)
{
	size_t i, j, most = a->nr_lines > b->nr_lines ? a->nr_lines : b->nr_lines;
	size_t shared = 0;

	if (!most)
		return 0;
	for (i = 0; i < a->nr_lines; i++)
		for (j = 0; j < b->nr_lines; j++)
			if (a->lines[i].len == b->lines[j].len &&
			    !memcmp(a->lines[i].p, b->lines[j].p,
				    a->lines[i].len)) {
				shared++;
				break;
			}
	return shared * 2 >= most;
}

/*
 * The two sides, lined up.  First the commits whose patch is the same change
 * and whose message is the same, marked =.  What is left of each side is then
 * walked together in order, and a left is paired with the next right whose
 * patch has at least half its changed lines in common, marked !; whatever the
 * walk cannot pair is left as < or >.
 *
 * Both passes run forward only, so the pairs never cross: a left's partner
 * always sits further along than the left before it.  Printing walks the two
 * sides on that, and it is also what a series is -- commits in an order -- so
 * a left is never paired with a right that sits before its neighbour's.
 */
static void rd_pair(struct rd_side *l, struct rd_side *r)
{
	size_t i, j, last = 0, nl = 0, nr = 0;
	size_t *lu, *ru;

	for (i = 0; i < l->nr; i++)
		for (j = last; j < r->nr; j++) {
			if (memcmp(l->c[i].patch_id, r->c[j].patch_id,
				   GP_SHA1_RAWSZ))
				continue;
			if (strcmp(l->c[i].message, r->c[j].message))
				continue;
			l->c[i].pair = (long)j;
			l->c[i].mark = '=';
			r->c[j].pair = (long)i;
			r->c[j].mark = '=';
			last = j + 1;
			break;
		}

	lu = xmalloc((l->nr ? l->nr : 1) * sizeof *lu);
	ru = xmalloc((r->nr ? r->nr : 1) * sizeof *ru);
	for (i = 0; i < l->nr; i++)
		if (l->c[i].pair < 0)
			lu[nl++] = i;
	for (j = 0; j < r->nr; j++)
		if (r->c[j].pair < 0)
			ru[nr++] = j;

	for (i = 0, j = 0; i < nl && j < nr; ) {
		if (!rd_alike(&l->c[lu[i]], &r->c[ru[j]])) {
			j++;
			continue;
		}
		l->c[lu[i]].pair = (long)ru[j];
		l->c[lu[i]].mark = '!';
		r->c[ru[j]].pair = (long)lu[i];
		r->c[ru[j]].mark = '!';
		i++;
		j++;
	}
	for (i = 0; i < nl; i++)
		if (l->c[lu[i]].pair < 0)
			l->c[lu[i]].mark = '<';
	for (j = 0; j < nr; j++)
		if (r->c[ru[j]].pair < 0)
			r->c[ru[j]].mark = '>';

	free(lu);
	free(ru);
}

/* ------------------------------------------------------------------ */
/* printing                                                            */

static void rd_indent(struct buf *out, const struct buf *in)
{
	const char *s = (const char *)in->b;
	const char *end = s + in->len;

	while (s < end) {
		const char *nl = memchr(s, '\n', (size_t)(end - s));
		size_t ll = nl ? (size_t)(nl - s) : (size_t)(end - s);

		buf_addstr(out, "    ");
		buf_add(out, s, ll);
		buf_addch(out, '\n');
		s = nl ? nl + 1 : end;
	}
}

/*
 * One line of the report.  A side that is not there prints as `-:  -------`,
 * which is how git writes the absence, and the subject is always the left
 * one's, as git prints it.
 */
static void rd_line(struct buf *out, size_t ln, const struct rd_commit *lc,
		    char mark, size_t rn, const struct rd_commit *rc)
{
	if (lc)
		buf_addf(out, "%d:  %s", (int)ln, lc->hex);
	else
		buf_addstr(out, "-:  -------");
	buf_addf(out, " %c ", mark);
	if (rc)
		buf_addf(out, "%d:  %s", (int)rn, rc->hex);
	else
		buf_addstr(out, "-:  -------");
	buf_addf(out, " %s\n", lc ? lc->subject : rc->subject);
}

static void rd_print(struct buf *out, struct rd_side *l, struct rd_side *r,
		     int left_only, int right_only)
{
	size_t i = 0, j = 0;

	/*
	 * Both sides in their own order, taking whichever comes next: a left the
	 * pairing left alone, then a right it left alone, then the next pair.
	 * The pairs do not cross, so the two positions only move forward.
	 */
	while (i < l->nr || j < r->nr) {
		if (i < l->nr && l->c[i].pair < 0) {
			if (!right_only)
				rd_line(out, i + 1, &l->c[i], '<', 0, NULL);
			i++;
			continue;
		}
		if (j < r->nr && r->c[j].pair < 0) {
			if (!left_only)
				rd_line(out, 0, NULL, '>', j + 1, &r->c[j]);
			j++;
			continue;
		}
		if (i >= l->nr || j >= r->nr)
			break;
		rd_line(out, i + 1, &l->c[i], l->c[i].mark, j + 1, &r->c[j]);
		if (l->c[i].mark == '!') {
			struct buf ta = BUF_INIT, tb = BUF_INIT, inner = BUF_INIT;

			/*
			 * What a ! pair is: this command's diff of the two
			 * commits as it holds them -- the message, then the
			 * patch.  The message is in there so that a pair whose
			 * change is the same one told twice still says what
			 * changed, which is then the wording and nothing else.
			 * git diffs its own rendering of the two commits; the
			 * text here is ours.
			 */
			buf_addstr(&ta, l->c[i].message);
			buf_addch(&ta, '\n');
			buf_add(&ta, l->c[i].patch.b, l->c[i].patch.len);
			buf_addstr(&tb, r->c[j].message);
			buf_addch(&tb, '\n');
			buf_add(&tb, r->c[j].patch.b, r->c[j].patch.len);
			diff_buffers(l->c[i].hex, ta.b, ta.len, r->c[j].hex,
				     tb.b, tb.len, &inner, 0);
			rd_indent(out, &inner);
			buf_release(&ta);
			buf_release(&tb);
			buf_release(&inner);
		}
		i++;
		j++;
	}
}

/* ------------------------------------------------------------------ */
/* the arguments                                                       */

/*
 * A revision that has to name a commit, or the command stops.  resolve_rev
 * answers -1 without a word when the name is not a ref or an object, and the
 * peel answers -1 without a word when what it names is not a commit, so the
 * naming is done here, once, for every revision this command reads.
 */
static void rd_rev(struct repo *r, const char *rev, oid_t *out)
{
	if (resolve_rev(r, rev, out) < 0 ||
	    commit_peel(r, out, OBJ_COMMIT, out) < 0)
		gp_die("range-diff: bad revision '%s'", rev);
}

/*
 * One range as one argument: <base>..<tip>, or <tip1>...<tip2> whose base is
 * the merge base of the two tips.  A bare revision is not a range.
 */
static int rd_range(struct repo *r, const char *spec, oid_t *base, oid_t *tip)
{
	const char *dots = strstr(spec, "...");
	char *a, *b;

	if (dots) {
		struct oid_array mb = OID_ARRAY_INIT;
		oid_t oa, ob;

		a = xstrndup(spec, (size_t)(dots - spec));
		b = xstrdup(dots + 3);
		if (!*a || !*b) {
			gp_error("range-diff: bad range '%s'", spec);
			free(a);
			free(b);
			return -1;
		}
		rd_rev(r, a, &oa);
		rd_rev(r, b, &ob);
		free(a);
		free(b);
		if (merge_bases(r, &oa, &ob, &mb) == 0) {
			oid_array_clear(&mb);
			gp_error("range-diff: no merge base for '%s'", spec);
			return -1;
		}
		*base = mb.oid[0];
		*tip = ob;
		oid_array_clear(&mb);
		return 0;
	}

	dots = strstr(spec, "..");
	if (!dots) {
		gp_error("range-diff: '%s' is not a range", spec);
		return -1;
	}
	a = xstrndup(spec, (size_t)(dots - spec));
	b = xstrdup(dots + 2);
	/* `..B` names no base, and a range needs one, as in git */
	if (!*a || !*b) {
		gp_error("range-diff: bad range '%s'", spec);
		free(a);
		free(b);
		return -1;
	}
	rd_rev(r, a, base);
	rd_rev(r, b, tip);
	free(a);
	free(b);
	return 0;
}

static int rd_need_ranges(void)
{
	gp_error("range-diff: need two commit ranges");
	fprintf(stderr, "hint: range-diff <base>..<tip> <base>..<tip>, "
			"<tip1>...<tip2>, or <base> <tip1> <tip2>\n");
	return 1;
}

int cmd_range_diff(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct rd_side l = RD_SIDE_INIT, rside = RD_SIDE_INIT;
	struct buf out = BUF_INIT;
	oid_t lbase, ltip, rbase, rtip;
	int left_only, right_only;

	opts_init(&o, argc, argv, (const char *const[]){
		"--left-only", "--right-only", "--no-dual-color", NULL });
	left_only = opts_flag(&o, "--left-only");
	right_only = opts_flag(&o, "--right-only");

	if (o.nargs == 1) {
		const char *dots = strstr(o.args[0], "...");
		char *a, *b;
		struct oid_array mb = OID_ARRAY_INIT;

		if (!dots)
			return rd_need_ranges();
		a = xstrndup(o.args[0], (size_t)(dots - o.args[0]));
		b = xstrdup(dots + 3);
		if (!*a || !*b) {
			gp_error("range-diff: bad range '%s'", o.args[0]);
			free(a);
			free(b);
			return 1;
		}
		rd_rev(r, a, &ltip);
		rd_rev(r, b, &rtip);
		free(a);
		free(b);
		if (merge_bases(r, &ltip, &rtip, &mb) == 0) {
			oid_array_clear(&mb);
			gp_error("range-diff: no merge base for '%s'", o.args[0]);
			return 1;
		}
		lbase = mb.oid[0];
		rbase = lbase;
		oid_array_clear(&mb);
	} else if (o.nargs == 2) {
		if (rd_range(r, o.args[0], &lbase, &ltip) < 0)
			return 1;
		if (rd_range(r, o.args[1], &rbase, &rtip) < 0)
			return 1;
	} else if (o.nargs == 3) {
		rd_rev(r, o.args[0], &lbase);
		rd_rev(r, o.args[1], &ltip);
		rd_rev(r, o.args[2], &rtip);
		rbase = lbase;
	} else {
		return rd_need_ranges();
	}

	rd_collect(r, &lbase, &ltip, &l);
	rd_collect(r, &rbase, &rtip, &rside);

	/* an empty range is no series, and git refuses one too */
	if (!l.nr || !rside.nr) {
		rd_side_release(&l);
		rd_side_release(&rside);
		return rd_need_ranges();
	}

	rd_pair(&l, &rside);
	rd_print(&out, &l, &rside, left_only, right_only);
	fwrite(out.b, 1, out.len, stdout);

	buf_release(&out);
	rd_side_release(&l);
	rd_side_release(&rside);
	return 0;
}
