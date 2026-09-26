/*
 * merge3.c - a three-way merge of file contents, line by line.
 *
 * A file both sides changed is not automatically a conflict: only changes that
 * touch are.  What each side did to the base is a diff, and the merge walks the
 * two diffs together -- a region one side changed is taken from that side, a
 * region both changed is a conflict unless the two produced the same text (or
 * a strategy option says which side wins), and the lines neither side touched
 * go out unchanged.  So a file edited on both sides keeps both sets of edits
 * when they are apart, which is the difference between a merge and a
 * whole-file conflict.
 *
 * Two changes that touch -- one ending where the other begins -- are one
 * region, which is git's rule as well: ours editing the line after theirs is a
 * conflict, not two changes that happen to be adjacent.
 *
 * The diff is the longest common subsequence of the two line sequences, found
 * by Hirschberg's algorithm so that a large file costs time but not memory.
 * The alignment a diff settles on decides which edits a merge calls
 * overlapping, and where a line repeats, more than one minimal diff exists:
 * this takes the one the search returns rather than sliding an edit onto the
 * later of two identical lines the way git does, because that describes a
 * replacement as an insertion and a deletion, and a merge that splits a
 * replacement in two can keep one half and lose the other.  Lines are compared
 * as bytes, newline included, so a file that differs only at its end is not
 * silently "fixed"; conflict markers are always on lines of their own, as git's
 * are, and the lines the two sides agree on are left outside them.
 */
#include "gp.h"

/* one line: a slice of the buffer it came from, newline included */
struct line {
	const u8 *p;
	size_t len;
};

static size_t split_lines(const u8 *s, size_t len, struct line **out)
{
	struct line *v = NULL;
	size_t alloc = 0, n = 0, i = 0;

	while (i < len) {
		size_t start = i;

		if (n == alloc) {
			alloc = alloc ? alloc * 2 : 64;
			v = xrealloc(v, alloc * sizeof(*v));
		}
		while (i < len && s[i] != '\n')
			i++;
		if (i < len)
			i++;            /* the newline belongs to the line before it */
		v[n].p = s + start;
		v[n].len = i - start;
		n++;
	}
	*out = v;
	return n;
}

/* ------------------------------------------------------------------ */
/* line identity                                                       */

/*
 * Lines are compared many times over, so each distinct one is given a small
 * integer: a hash table over the line text, with the text itself kept as the
 * key.  Ids start at 1, which leaves 0 to mean "no entry" in the table.
 */
struct idmap {
	struct line *key;
	u32 *id;
	size_t cap, used, next;
};

static u32 line_hash(const struct line *l)
{
	u32 h = 2166136261u;
	size_t i;

	for (i = 0; i < l->len; i++) {
		h ^= l->p[i];
		h *= 16777619u;
	}
	return h;
}

static int same_line(const struct line *a, const struct line *b)
{
	return a->len == b->len && !memcmp(a->p, b->p, a->len);
}

static void idmap_init(struct idmap *m)
{
	m->cap = 256;
	m->key = xcalloc(m->cap, sizeof(*m->key));
	m->id = xcalloc(m->cap, sizeof(*m->id));
	m->used = 0;
	m->next = 0;
}

static void idmap_release(struct idmap *m)
{
	free(m->key);
	free(m->id);
}

static void idmap_grow(struct idmap *m)
{
	struct line *okey = m->key;
	u32 *oid = m->id;
	size_t oldcap = m->cap, i;

	m->cap *= 2;
	m->key = xcalloc(m->cap, sizeof(*m->key));
	m->id = xcalloc(m->cap, sizeof(*m->id));
	for (i = 0; i < oldcap; i++) {
		size_t j;

		if (!oid[i])
			continue;
		j = line_hash(&okey[i]) & (m->cap - 1);
		while (m->id[j])
			j = (j + 1) & (m->cap - 1);
		m->key[j] = okey[i];
		m->id[j] = oid[i];
	}
	free(okey);
	free(oid);
}

static u32 idmap_id(struct idmap *m, const struct line *l)
{
	size_t j = line_hash(l) & (m->cap - 1);
	u32 id;

	while (m->id[j]) {
		if (same_line(&m->key[j], l))
			return m->id[j];
		j = (j + 1) & (m->cap - 1);
	}
	id = ++m->next;
	m->key[j] = *l;
	m->id[j] = id;
	m->used++;
	if (m->used * 4 >= m->cap * 3)
		idmap_grow(m);          /* the id does not change when it moves */
	return id;
}

static u32 *to_ids(struct idmap *m, const struct line *v, size_t n)
{
	u32 *ids = n ? xmalloc(n * sizeof(*ids)) : NULL;
	size_t i;

	for (i = 0; i < n; i++)
		ids[i] = idmap_id(m, &v[i]);
	return ids;
}

/* ------------------------------------------------------------------ */
/* the longest common subsequence                                      */

/*
 * row[j] = the length of the LCS of a[alo:alo+i) and b[blo:blo+j), for the i
 * the row was left at; `tmp` holds the row before it, since the recurrence
 * wants both the previous row and the current one.
 */
static void lcs_row(const u32 *a, size_t alo, size_t ahi,
		    const u32 *b, size_t blo, size_t bhi, u32 *row, u32 *tmp)
{
	size_t i, j, n = ahi - alo, m = bhi - blo;

	for (j = 0; j <= m; j++)
		row[j] = 0;
	for (i = 1; i <= n; i++) {
		for (j = 0; j <= m; j++)
			tmp[j] = row[j];
		for (j = 1; j <= m; j++) {
			if (a[alo + i - 1] == b[blo + j - 1])
				row[j] = tmp[j - 1] + 1;
			else
				row[j] = tmp[j] > row[j - 1] ? tmp[j] : row[j - 1];
		}
	}
}

/* the same, from the end: row[j] = LCS of a[alo:ahi) and b[blo+j:bhi) */
static void lcs_row_back(const u32 *a, size_t alo, size_t ahi,
			 const u32 *b, size_t blo, size_t bhi,
			 u32 *row, u32 *tmp)
{
	size_t i, j, n = ahi - alo, m = bhi - blo;

	for (j = 0; j <= m; j++)
		row[j] = 0;
	for (i = 1; i <= n; i++) {
		for (j = 0; j <= m; j++)
			tmp[j] = row[j];
		row[m] = 0;
		for (j = m; j-- > 0; ) {
			if (a[ahi - i] == b[blo + j])
				row[j] = tmp[j + 1] + 1;
			else
				row[j] = tmp[j] > row[j + 1] ? tmp[j] : row[j + 1];
		}
	}
}

/* the matched positions, in order */
struct pairs {
	size_t *a, *b;
	size_t n, alloc;
};

static void pair_push(struct pairs *p, size_t ai, size_t bi)
{
	if (p->n == p->alloc) {
		p->alloc = p->alloc ? p->alloc * 2 : 128;
		p->a = xrealloc(p->a, p->alloc * sizeof(*p->a));
		p->b = xrealloc(p->b, p->alloc * sizeof(*p->b));
	}
	p->a[p->n] = ai;
	p->b[p->n] = bi;
	p->n++;
}

/*
 * The LCS of two line sequences, as the pairs of positions that match, in
 * order.  Hirschberg: halve the first sequence, find the place in the second
 * where splitting matches best -- which needs only the lengths, one row each
 * way -- and recurse into the two halves.  That is what keeps the memory
 * proportional to the shorter sequence rather than to both.
 */
static void lcs_pairs(const u32 *a, size_t alo, size_t ahi,
		      const u32 *b, size_t blo, size_t bhi,
		      struct pairs *out, u32 *fwd, u32 *bwd, u32 *tmp)
{
	size_t m = bhi - blo, am, j, best = 0, bestk = 0;

	if (alo >= ahi || blo >= bhi)
		return;
	if (ahi - alo == 1) {
		for (j = blo; j < bhi; j++)
			if (a[alo] == b[j]) {
				pair_push(out, alo, j);
				break;
			}
		return;
	}
	am = alo + (ahi - alo) / 2;
	lcs_row(a, alo, am, b, blo, bhi, fwd, tmp);
	lcs_row_back(a, am, ahi, b, blo, bhi, bwd, tmp);
	for (j = 0; j <= m; j++) {
		size_t v = fwd[j] + bwd[j];

		if (v > best) {
			best = v;
			bestk = j;
		}
	}
	lcs_pairs(a, alo, am, b, blo, blo + bestk, out, fwd, bwd, tmp);
	lcs_pairs(a, am, ahi, b, blo + bestk, bhi, out, fwd, bwd, tmp);
}

/* ------------------------------------------------------------------ */
/* the diff                                                            */

/* base[b_off:b_off+b_len) is replaced by side[s_off:s_off+s_len) */
struct change {
	size_t b_off, b_len;
	size_t s_off, s_len;
};

struct change_list {
	struct change *v;
	size_t n, alloc;
};

static void change_push(struct change_list *l, size_t b_off, size_t b_len,
			size_t s_off, size_t s_len)
{
	if (!b_len && !s_len)
		return;
	if (l->n == l->alloc) {
		l->alloc = l->alloc ? l->alloc * 2 : 32;
		l->v = xrealloc(l->v, l->alloc * sizeof(*l->v));
	}
	l->v[l->n].b_off = b_off;
	l->v[l->n].b_len = b_len;
	l->v[l->n].s_off = s_off;
	l->v[l->n].s_len = s_len;
	l->n++;
}

static void change_list_release(struct change_list *l)
{
	free(l->v);
}

/*
 * Which lines of each file the alignment leaves unmatched: matched lines are 0
 * in the flags, the others are 1.  That is what the two diff commands and the
 * merge all want to start from, and it is what can be slid around.
 */
static void diff_flags(const u32 *base, size_t bn, const u32 *side, size_t sn,
		       u8 *bflags, u8 *sflags)
{
	struct pairs p = { NULL, NULL, 0, 0 };
	u32 *fwd, *bwd, *tmp;
	size_t i = 0, j = 0, k;

	fwd = xmalloc((sn + 1) * sizeof(*fwd));
	bwd = xmalloc((sn + 1) * sizeof(*bwd));
	tmp = xmalloc((sn + 1) * sizeof(*tmp));
	lcs_pairs(base, 0, bn, side, 0, sn, &p, fwd, bwd, tmp);
	free(fwd);
	free(bwd);
	free(tmp);

	memset(bflags, 0, bn);
	memset(sflags, 0, sn);
	for (k = 0; k < p.n; k++) {
		while (i < p.a[k])
			bflags[i++] = 1;
		while (j < p.b[k])
			sflags[j++] = 1;
		i = p.a[k] + 1;
		j = p.b[k] + 1;
	}
	while (i < bn)
		bflags[i++] = 1;
	while (j < sn)
		sflags[j++] = 1;
	free(p.a);
	free(p.b);
}

/*
 * The changed runs, as base[b_off:b_off+b_len) replaced by side[s_off:...].
 *
 * Sliding a run later, the way git's diff does when the line past its end
 * repeats the line it starts with, would line the two descriptions of an edit
 * that touches one of several identical lines up with git's -- but only by
 * rewriting a replacement as an insertion and a deletion, and a merge that
 * splits a replacement that way will hand half of it to the other side and
 * keep the other half, which duplicates or drops a line without a conflict to
 * show it.  So the runs stay where the longest common subsequence puts them.
 */
static void flags_changes(const u8 *bflags, size_t bn, const u8 *sflags,
			  size_t sn, struct change_list *out)
{
	size_t i = 0, j = 0;

	while (i < bn || j < sn) {
		size_t bi, si;

		if (i < bn && !bflags[i] && j < sn && !sflags[j]) {
			i++;
			j++;
			continue;
		}
		bi = i;
		si = j;
		while (i < bn && bflags[i])
			i++;
		while (j < sn && sflags[j])
			j++;
		if (i == bi && j == si) {
			/*
			 * Nothing changed at either head: one file has run out
			 * and the other has only matched lines left, which the
			 * alignment cannot have left behind.
			 */
			break;
		}
		change_push(out, bi, i - bi, si, j - si);
	}
}

/* ------------------------------------------------------------------ */
/* how much of one text the other still has                            */

/*
 * A percentage: the lines the two have in common, over the lines there are
 * between them, so an unchanged file is 100 and a file with no line left of
 * the other is 0.  Two empty files are each other's whole text.
 *
 * This is what tells a file that was moved and edited from a file that was
 * deleted beside an unrelated one.  git scores the same judgement from the
 * bytes its delta copied rather than from the lines its diff matched, which is
 * finer -- a one-line file that was rewritten has no line in common and scores
 * 0 here however much of it survived -- but it is the same question asked of
 * the same two texts, and the same 50% line between "the same file" and "a
 * different one".
 */
int merge3_similarity(const struct buf *a, const struct buf *b)
{
	struct line *al = NULL, *bl = NULL;
	struct change_list cl = { NULL, 0, 0 };
	struct idmap m;
	u32 *aid, *bid;
	u8 *afoo, *bfoo;
	size_t an, bn, changed = 0, common, i;

	an = split_lines(a->b, a->len, &al);
	bn = split_lines(b->b, b->len, &bl);
	if (!an && !bn)
		return 100;

	idmap_init(&m);
	aid = to_ids(&m, al, an);
	bid = to_ids(&m, bl, bn);
	afoo = xcalloc(an ? an : 1, 1);
	bfoo = xcalloc(bn ? bn : 1, 1);
	diff_flags(aid, an, bid, bn, afoo, bfoo);
	flags_changes(afoo, an, bfoo, bn, &cl);

	/* every change replaces base lines with side lines, so what one
	 * description leaves behind is what the other does */
	for (i = 0; i < cl.n; i++)
		changed += cl.v[i].b_len;
	common = an - changed;

	free(afoo);
	free(bfoo);
	free(aid);
	free(bid);
	free(al);
	free(bl);
	idmap_release(&m);
	change_list_release(&cl);
	return (int)((200 * (unsigned long)common) / (an + bn));
}

/* ------------------------------------------------------------------ */
/* the merge                                                           */

static void emit_lines(struct buf *out, const struct line *v,
		       size_t from, size_t to)
{
	size_t i;

	for (i = from; i < to; i++)
		buf_add(out, v[i].p, v[i].len);
}

/*
 * What `side` makes of base[b_lo:b_hi): its changes spliced in, the base lines
 * between them kept.  A region can be wider than either side's changes -- one
 * side's change bridging another's -- and then the lines no change covers are
 * simply carried over.
 */
static void region_text(const struct line *base, const struct line *side,
			size_t b_lo, size_t b_hi, const struct change_list *cl,
			size_t *next, struct buf *out)
{
	size_t cur = b_lo;

	/*
	 * `<=` rather than `<`, to match the region the caller grew: a change
	 * that only inserts at the region's end is at b_off == b_hi, and it is
	 * part of this region, not the next one.
	 */
	while (*next < cl->n && cl->v[*next].b_off <= b_hi) {
		const struct change *c = &cl->v[*next];

		if (c->b_off > cur)
			emit_lines(out, base, cur, c->b_off);
		emit_lines(out, side, c->s_off, c->s_off + c->s_len);
		cur = c->b_off + c->b_len;
		(*next)++;
	}
	if (cur < b_hi)
		emit_lines(out, base, cur, b_hi);
}

/* a marker is a line of its own, so text that stops short of a newline gets one */
static void emit_terminated(struct buf *out, const struct line *v,
			    size_t from, size_t to)
{
	emit_lines(out, v, from, to);
	if (to > from && v[to - 1].len && v[to - 1].p[v[to - 1].len - 1] != '\n')
		buf_addch(out, '\n');
}

int merge3(const struct buf *base, const struct buf *ours,
	   const struct buf *theirs, enum merge_favor favor,
	   const char *label_ours, const char *label_theirs, struct buf *out)
{
	struct line *bl = NULL, *ol = NULL, *tl = NULL;
	struct change_list oc = { NULL, 0, 0 }, tc = { NULL, 0, 0 };
	struct idmap m;
	u8 *bfoo, *ofoo, *bft, *tft;
	u32 *bid, *oid, *tid;
	size_t bn, on, tn, i = 0, j = 0, cursor = 0;
	int conflicts = 0;

	bn = split_lines(base->b, base->len, &bl);
	on = split_lines(ours->b, ours->len, &ol);
	tn = split_lines(theirs->b, theirs->len, &tl);

	idmap_init(&m);
	bid = to_ids(&m, bl, bn);
	oid = to_ids(&m, ol, on);
	tid = to_ids(&m, tl, tn);

	bfoo = xcalloc(bn ? bn : 1, 1);
	ofoo = xcalloc(on ? on : 1, 1);
	bft = xcalloc(bn ? bn : 1, 1);
	tft = xcalloc(tn ? tn : 1, 1);
	diff_flags(bid, bn, oid, on, bfoo, ofoo);
	diff_flags(bid, bn, tid, tn, bft, tft);
	flags_changes(bfoo, bn, ofoo, on, &oc);
	flags_changes(bft, bn, tft, tn, &tc);
	free(bfoo);
	free(ofoo);
	free(bft);
	free(tft);

	while (i < oc.n || j < tc.n) {
		struct buf ro, rt;
		size_t lo, hi, k = i, l = j;
		int ours_touched = 0, theirs_touched = 0;

		lo = i < oc.n ? oc.v[i].b_off : SIZE_MAX;
		if (j < tc.n && tc.v[j].b_off < lo)
			lo = tc.v[j].b_off;

		/*
		 * Grow the region to cover every change that touches it, from
		 * either side, so that two changes meeting at a line are one
		 * conflict rather than two edits that miss each other.
		 */
		hi = lo;
		for (;;) {
			int grew = 0;

			while (i < oc.n && oc.v[i].b_off <= hi) {
				if (oc.v[i].b_off + oc.v[i].b_len > hi)
					hi = oc.v[i].b_off + oc.v[i].b_len;
				i++;
				ours_touched = grew = 1;
			}
			while (j < tc.n && tc.v[j].b_off <= hi) {
				if (tc.v[j].b_off + tc.v[j].b_len > hi)
					hi = tc.v[j].b_off + tc.v[j].b_len;
				j++;
				theirs_touched = grew = 1;
			}
			if (!grew)
				break;
		}

		if (cursor < lo)
			emit_lines(out, bl, cursor, lo);

		buf_init(&ro);
		buf_init(&rt);
		region_text(bl, ol, lo, hi, &oc, &k, &ro);
		region_text(bl, tl, lo, hi, &tc, &l, &rt);

		if (!theirs_touched) {
			buf_add(out, ro.b, ro.len);
		} else if (!ours_touched) {
			buf_add(out, rt.b, rt.len);
		} else if (ro.len == rt.len && (!ro.len || !memcmp(ro.b, rt.b, ro.len))) {
			buf_add(out, ro.b, ro.len);     /* both sides made the same edit */
		} else if (favor == MERGE_FAVOR_OURS) {
			buf_add(out, ro.b, ro.len);
		} else if (favor == MERGE_FAVOR_THEIRS) {
			buf_add(out, rt.b, rt.len);
		} else {
			struct line *rl = NULL, *sl = NULL;
			size_t rn, sn, p = 0, s = 0;

			/*
			 * Lines the two sides agree on are not in dispute, so
			 * they go outside the markers -- which is what git
			 * does, and it keeps a conflict down to the part of the
			 * region that actually differs.
			 */
			rn = split_lines(ro.b, ro.len, &rl);
			sn = split_lines(rt.b, rt.len, &sl);
			while (p < rn && p < sn && same_line(&rl[p], &sl[p]))
				p++;
			while (s < rn - p && s < sn - p &&
			       same_line(&rl[rn - 1 - s], &sl[sn - 1 - s]))
				s++;

			emit_lines(out, rl, 0, p);
			buf_addf(out, "<<<<<<< %s\n", label_ours);
			emit_terminated(out, rl, p, rn - s);
			buf_addstr(out, "=======\n");
			emit_terminated(out, sl, p, sn - s);
			buf_addf(out, ">>>>>>> %s\n", label_theirs);
			emit_lines(out, rl, rn - s, rn);
			free(rl);
			free(sl);
			conflicts = 1;
		}
		buf_release(&ro);
		buf_release(&rt);
		cursor = hi;
	}
	if (cursor < bn)
		emit_lines(out, bl, cursor, bn);

	free(bid);
	free(oid);
	free(tid);
	change_list_release(&oc);
	change_list_release(&tc);
	idmap_release(&m);
	free(bl);
	free(ol);
	free(tl);
	return conflicts;
}
