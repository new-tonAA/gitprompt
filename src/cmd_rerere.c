/*
 * rerere -- reuse the way a conflict was resolved the last time it came up.
 *
 * A conflict is met, resolved once, and met again: the same two sides, in the
 * same file, or in another file that has the same two sides somewhere in it.
 * What was done about it the first time is remembered, keyed by the conflict
 * itself, and put back the second time so the same work is not done twice.
 *
 * The record is not consulted for an opinion about whether the resolution was
 * right.  A remembered resolution is written into the file and left unmerged,
 * exactly as if it had just been typed: it shows up in `status` and in a `diff`
 * and has to be staged before it counts -- the one exception being
 * `rerere.autoupdate`, where a resolution taken from the cache is staged as
 * well.  What the mechanism removes is the retyping, not the review.
 *
 * An entry lives in `<gitprompt dir>/rr-cache/<id>/`: `preimage` is the
 * conflict as it was, `thisimage` the file the conflict was found in, and
 * `postimage` what the file became once it was resolved.  A conflict is
 * remembered as soon as it is met, which is why the entry for one not yet
 * resolved has no `postimage` in it.
 *
 * The id is a hash of the two sides' bytes and nothing else: not the text
 * around the conflict, and not the labels on the markers, so a conflict that
 * moved down a file, or was named the other way round, still finds the answer
 * recorded for it.  git hashes the same thing in a way this does not
 * reproduce, so the entries are not shared with it -- a resolution recorded
 * here is not one git will reuse, or the other way round.  That, and the rest
 * of what is not carried, is in docs/limitations.md.
 */
#include "gp.h"

#include <ctype.h>
#include <dirent.h>
#include <string.h>

#define RR_DIR "rr-cache"
#define RR_MERGE "MERGE_RR"

/* ------------------------------------------------------------------ */
/* the cache on disk                                                   */

static int rr_ascii_icmp(const char *a, const char *b)
{
	int ca, cb;

	while (*a || *b) {
		ca = tolower((unsigned char)*a);
		cb = tolower((unsigned char)*b);
		if (ca != cb)
			return ca - cb;
		a++;
		b++;
	}
	return 0;
}

/*
 * A config value read as a boolean, the way git reads one: with no value the
 * caller's default stands, and a value that is neither a yes nor a no leaves
 * the default in place rather than stopping the command.
 */
static int rr_config_bool(struct repo *r, const char *key, int dflt)
{
	char *v = NULL;
	int rc = dflt;

	if (repo_config_get(r, key, &v) < 0 || !v)
		return dflt;
	if (!*v || !strcmp(v, "1") || !rr_ascii_icmp(v, "true") ||
	    !rr_ascii_icmp(v, "yes") || !rr_ascii_icmp(v, "on"))
		rc = 1;
	else if (!strcmp(v, "0") || !rr_ascii_icmp(v, "false") ||
		 !rr_ascii_icmp(v, "no") || !rr_ascii_icmp(v, "off"))
		rc = 0;
	free(v);
	return rc;
}

static char *rr_cache_dir(struct repo *r)
{
	return xstrfmt("%s/%s", r->gpdir, RR_DIR);
}

/*
 * Whether rerere runs at all: `rerere.enabled` when it is set, and otherwise
 * whether a cache is already there -- which is git's rule, and the one that
 * makes `rerere.enabled = false` the way to turn off a repository that has
 * acquired one.
 */
int rerere_enabled(struct repo *r)
{
	char *d = rr_cache_dir(r);
	int on = is_directory(d);

	free(d);
	return rr_config_bool(r, "rerere.enabled", on);
}

static int rerere_autoupdate(struct repo *r)
{
	return rr_config_bool(r, "rerere.autoupdate", 0);
}

static char *rr_entry_path(struct repo *r, const char *hex, const char *name)
{
	return xstrfmt("%s/%s/%s/%s", r->gpdir, RR_DIR, hex, name);
}

static int rr_read(struct repo *r, const char *hex, const char *name,
		   struct buf *out)
{
	char *p = rr_entry_path(r, hex, name);
	int rc = read_file(p, out);

	free(p);
	return rc;
}

static int rr_has(struct repo *r, const char *hex, const char *name)
{
	struct buf b = BUF_INIT;
	int rc = rr_read(r, hex, name, &b) == 0;

	buf_release(&b);
	return rc;
}

static int rr_write(struct repo *r, const char *hex, const char *name,
		    const void *data, size_t len)
{
	char *dir = xstrfmt("%s/%s/%s", r->gpdir, RR_DIR, hex);
	char *p = rr_entry_path(r, hex, name);
	int rc;

	mkdir_p(dir);
	rc = write_file(p, data, len);
	free(dir);
	free(p);
	return rc;
}

static void rr_entry_remove(struct repo *r, const char *hex)
{
	char *d = xstrfmt("%s/%s/%s", r->gpdir, RR_DIR, hex);

	remove_dir_recursive(d);
	free(d);
}

static char *rr_merge_rr(struct repo *r)
{
	return xstrfmt("%s/%s", r->gpdir, RR_MERGE);
}

/* ------------------------------------------------------------------ */
/* MERGE_RR, which ties a path in the work tree to the conflict it came from */

/*
 * Drop a path's line, leaving the rest.  The file is written back only when
 * something is left in it; otherwise it goes, since an empty MERGE_RR and no
 * MERGE_RR mean the same thing.
 */
static void rr_pending_remove(struct repo *r, const char *path)
{
	struct buf b = BUF_INIT, keep = BUF_INIT;
	char *p = rr_merge_rr(r);
	size_t plen = strlen(path);
	const char *s, *end;

	if (read_file(p, &b) < 0)
		goto done;
	for (s = (const char *)b.b, end = s + b.len; s < end; ) {
		const char *eol = memchr(s, '\n', (size_t)(end - s));
		const char *e = eol ? eol : end;         /* the line, no newline */
		const char *tab = memchr(s, '\t', (size_t)(e - s));

		if (!(tab && (size_t)(tab - s) == GP_SHA1_HEXSZ &&
		      (size_t)(e - tab - 1) == plen &&
		      !memcmp(tab + 1, path, plen)))
			buf_add(&keep, s, (size_t)(e - s) + (eol ? 1 : 0));
		s = eol ? eol + 1 : end;
	}
	if (keep.len)
		write_file(p, keep.b, keep.len);
	else
		remove_file(p);
done:
	buf_release(&b);
	buf_release(&keep);
	free(p);
}

/* the id a path is filed under, if it is waiting for a resolution */
static int rr_pending_lookup(struct repo *r, const char *path, char *hex)
{
	struct buf b = BUF_INIT;
	char *p = rr_merge_rr(r);
	size_t plen = strlen(path);
	const char *s, *end;
	int found = 0;

	if (read_file(p, &b) < 0) {
		free(p);
		return 0;
	}
	free(p);
	for (s = (const char *)b.b, end = s + b.len; s < end; ) {
		const char *eol = memchr(s, '\n', (size_t)(end - s));
		const char *e = eol ? eol : end;
		const char *tab = memchr(s, '\t', (size_t)(e - s));

		if (tab && (size_t)(tab - s) == GP_SHA1_HEXSZ &&
		    (size_t)(e - tab - 1) == plen &&
		    !memcmp(tab + 1, path, plen)) {
			memcpy(hex, s, GP_SHA1_HEXSZ);
			hex[GP_SHA1_HEXSZ] = '\0';
			found = 1;
			break;
		}
		s = eol ? eol + 1 : end;
	}
	buf_release(&b);
	return found;
}

static void rr_pending_add(struct repo *r, const char *hex, const char *path)
{
	struct buf b = BUF_INIT;
	char *p = rr_merge_rr(r);

	rr_pending_remove(r, path);        /* one line per path */
	read_file(p, &b);
	buf_addf(&b, "%s\t%s\n", hex, path);
	write_file(p, b.b, b.len);
	buf_release(&b);
	free(p);
}

/* ------------------------------------------------------------------ */
/* the conflict itself                                                 */

/*
 * Which conflict marker a line is, if any: 1 for `<<<<<<<`, 2 for `=======`,
 * 3 for `>>>>>>>`, 4 for a diff3 `|||||||`, 0 for anything else.  Seven
 * characters are what makes a marker a marker -- a line of eight is not one,
 * and neither is a shorter line of the same character.
 */
static int rr_marker(const char *p, size_t n)
{
	size_t i;
	char c;

	if (n < 7)
		return 0;
	c = p[0];
	if (c != '<' && c != '=' && c != '>' && c != '|')
		return 0;
	for (i = 0; i < 7; i++)
		if (p[i] != c)
			return 0;
	if (n > 7 && p[7] == c)
		return 0;
	switch (c) {
	case '<':
		return 1;
	case '=':
		return 2;
	case '>':
		return 3;
	default:
		return 4;
	}
}

/*
 * The conflicted text as it is kept: the marker lines cut down to their seven
 * characters with the labels after them dropped, and nothing else moved.  That
 * is the form git writes, and it is what makes the record independent of which
 * side was called which -- the same two sides, named the other way round, share
 * an entry rather than making a second one.
 *
 * `body`, when given, collects the bytes of the two sides in the order they
 * appear, and is what the entry's id is taken over.  Returns the number of
 * conflicts -- 0 being a file this has nothing to say about -- or -1 for a
 * `|||||||` line, a conflict that never ends, or a marker out of place.
 */
static int rr_normalize(const struct buf *text, struct buf *norm,
			struct buf *body)
{
	const char *p = (const char *)text->b;
	const char *end = p + text->len;
	enum { OUT, OURS, THEIRS } state = OUT;
	int n = 0;

	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		size_t len = eol ? (size_t)(eol - p) + 1 : (size_t)(end - p);
		int m = rr_marker(p, len);

		switch (m) {
		case 1:
			if (state != OUT)
				return -1;
			buf_addstr(norm, "<<<<<<<\n");
			state = OURS;
			break;
		case 2:
			if (state != OURS)
				return -1;
			buf_addstr(norm, "=======\n");
			state = THEIRS;
			break;
		case 3:
			if (state != THEIRS)
				return -1;
			buf_addstr(norm, ">>>>>>>\n");
			state = OUT;
			n++;
			break;
		case 4:
			return -1;
		default:
			buf_add(norm, p, len);
			if (body && state != OUT)
				buf_add(body, p, len);
			break;
		}
		p += len;
	}
	if (state != OUT)
		return -1;
	return n;
}

/*
 * The id of a set of conflicts: the sha1 of the two sides' bytes, in order.
 * 1 and the hex when the text holds at least one conflict, 0 when it holds
 * none, -1 when it holds something this cannot read.
 */
static int rr_id(const struct buf *text, char hex[GP_SHA1_HEXSZ + 1])
{
	struct buf norm = BUF_INIT, body = BUF_INIT;
	oid_t oid;
	int n = rr_normalize(text, &norm, &body);

	buf_release(&norm);
	if (n <= 0) {
		buf_release(&body);
		return n;
	}
	gp_sha1(body.b ? body.b : (const u8 *)"", body.len, oid.raw);
	oid_hex(&oid, hex);
	buf_release(&body);
	return 1;
}

/* ------------------------------------------------------------------ */
/* replaying a resolution                                              */

/* a conflict set cut into the text between its conflicts and the conflicts */
struct rr_seg {
	size_t off, len;
	int conflict;
};

static void rr_seg_push(struct rr_seg **v, size_t *nr, size_t *alloc,
			size_t off, size_t len, int conflict)
{
	if (*nr == *alloc) {
		*alloc = *alloc ? *alloc * 2 : 8;
		*v = xrealloc(*v, *alloc * sizeof(**v));
	}
	(*v)[*nr].off = off;
	(*v)[*nr].len = len;
	(*v)[*nr].conflict = conflict;
	(*nr)++;
}

/*
 * Cut a conflict set into its pieces.  There is always one more piece of text
 * than there are conflicts, so the two alternate and either end may be empty.
 */
static int rr_segments(const struct buf *text, struct rr_seg **out, size_t *out_nr)
{
	const char *base = (const char *)text->b;
	const char *p = base;
	const char *end = p + text->len;
	enum { OUT, OURS, THEIRS } state = OUT;
	struct rr_seg *v = NULL;
	size_t nr = 0, alloc = 0, start = 0, cstart = 0;

	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		size_t len = eol ? (size_t)(eol - p) + 1 : (size_t)(end - p);
		size_t at = (size_t)(p - base);

		switch (rr_marker(p, len)) {
		case 1:
			if (state != OUT)
				goto bad;
			rr_seg_push(&v, &nr, &alloc, start, at - start, 0);
			cstart = at;
			state = OURS;
			break;
		case 2:
			if (state != OURS)
				goto bad;
			state = THEIRS;
			break;
		case 3:
			if (state != THEIRS)
				goto bad;
			state = OUT;
			rr_seg_push(&v, &nr, &alloc, cstart, at + len - cstart, 1);
			start = at + len;
			break;
		case 4:
			goto bad;
		default:
			break;
		}
		p += len;
	}
	if (state != OUT)
		goto bad;
	rr_seg_push(&v, &nr, &alloc, start, text->len - start, 0);
	*out = v;
	*out_nr = nr;
	return 0;
bad:
	free(v);
	return -1;
}

/* the first occurrence of `needle` in `hay` at or after `from`, or -1 */
static long rr_find(const char *hay, size_t hlen, const char *needle,
		    size_t nlen, size_t from)
{
	size_t i;

	if (nlen == 0)
		return (long)from;
	if (from > hlen || nlen > hlen - from)
		return -1;
	for (i = from; i + nlen <= hlen; i++)
		if (hay[i] == needle[0] && !memcmp(hay + i, needle, nlen))
			return (long)i;
	return -1;
}

/*
 * Put the resolution recorded for a conflict set into the file in front of us.
 *
 * The recorded preimage and the file are the same outside their conflicts --
 * that is what the id being equal means -- so what the resolution holds there
 * is the file itself, and everything else is what was written in place of a
 * conflict.  The text on either end of the postimage is the text on either end
 * of the file, which anchors the two: what sits between the opening text and
 * the first conflict's far side is the first resolution, and so on inwards.
 *
 * A stretch of text that is not found where it should be means the resolution
 * rearranged the rest of the file as well, which is more than this will follow:
 * -1, and the conflict is left as it is.
 */
static int rr_replay(const struct buf *pre, const struct buf *post,
		     const struct buf *cur, struct buf *out)
{
	struct rr_seg *ps = NULL, *cs = NULL;
	size_t np = 0, nc = 0, k, ppos;
	int rc = -1;

	if (rr_segments(pre, &ps, &np) < 0 || rr_segments(cur, &cs, &nc) < 0)
		goto done;
	if (np != nc || np < 3)            /* no conflict, or a different shape */
		goto done;

	/*
	 * The text before the first conflict opens the resolution and the text
	 * after the last one closes it, so the postimage has to carry both, at
	 * its two ends.
	 */
	if (post->len < ps[0].len + ps[np - 1].len ||
	    memcmp(post->b, pre->b + ps[0].off, ps[0].len) ||
	    memcmp(post->b + post->len - ps[np - 1].len,
		   pre->b + ps[np - 1].off, ps[np - 1].len))
		goto done;
	if (memcmp(pre->b + ps[0].off, cur->b + cs[0].off, ps[0].len))
		goto done;
	buf_add(out, cur->b + cs[0].off, cs[0].len);
	ppos = ps[0].len;

	/*
	 * Each conflict in turn: where the text on its far side begins in the
	 * resolution, and so the bytes skipped over are what that conflict
	 * became.  The far side of the last conflict is the text that closes
	 * the file, which is at the end by definition rather than by search.
	 */
	for (k = 1; k + 1 < np; k += 2) {
		const struct rr_seg *plain = &ps[k + 1];
		const struct rr_seg *cplain = &cs[k + 1];
		size_t next;

		if (memcmp(pre->b + plain->off, cur->b + cplain->off, plain->len))
			goto done;
		if (k + 2 == np) {
			next = post->len - plain->len;
		} else {
			long at = rr_find((const char *)post->b, post->len,
					  (const char *)pre->b + plain->off,
					  plain->len, ppos);

			if (at < 0)
				goto done;
			next = (size_t)at;
		}
		if (next < ppos)
			goto done;
		buf_add(out, post->b + ppos, next - ppos);
		buf_add(out, cur->b + cplain->off, cplain->len);
		ppos = next + plain->len;
	}
	rc = 0;
done:
	free(ps);
	free(cs);
	return rc;
}

/* write a conflict down: it is what the next meeting with it will be read from */
static void rr_record(struct repo *r, const char *hex, const char *path,
		      const struct buf *conflicted)
{
	struct buf norm = BUF_INIT;

	rr_normalize(conflicted, &norm, NULL);
	rr_write(r, hex, "preimage", norm.b, norm.len);
	buf_release(&norm);
	rr_write(r, hex, "thisimage", conflicted->b, conflicted->len);
	rr_pending_add(r, hex, path);
}

/*
 * The hook the merge machinery calls for every conflict it writes.  `out` is
 * filled with the replayed resolution when one was found; otherwise it is left
 * alone and the caller writes the conflict as it made it.
 */
enum rr_result rerere_auto(struct repo *r, const char *path,
			   const struct buf *conflicted, struct buf *out)
{
	struct buf pre = BUF_INIT, post = BUF_INIT;
	char hex[GP_SHA1_HEXSZ + 1];
	enum rr_result res;
	int id = rr_id(conflicted, hex);

	if (id != 1)
		return RR_NONE;            /* nothing here to file under */

	if (rr_read(r, hex, "preimage", &pre) == 0 &&
	    rr_read(r, hex, "postimage", &post) == 0 &&
	    rr_replay(&pre, &post, conflicted, out) == 0) {
		res = rerere_autoupdate(r) ? RR_STAGED : RR_RESOLVED;
	} else {
		buf_reset(out);
		rr_record(r, hex, path, conflicted);
		res = RR_RECORDED;
	}
	buf_release(&pre);
	buf_release(&post);
	/*
	 * The path is filed either way: a resolution put back is still a
	 * resolution the user may go on to change, and that change is the
	 * answer for the time after this one.
	 */
	rr_pending_add(r, hex, path);
	return res;
}

void rerere_report(const char *path, enum rr_result res)
{
	switch (res) {
	case RR_RECORDED:
		printf("Recorded preimage for '%s'\n", path);
		break;
	case RR_RESOLVED:
		printf("Resolved '%s' using previous resolution.\n", path);
		break;
	case RR_STAGED:
		printf("Staged '%s' using previous resolution.\n", path);
		break;
	case RR_NONE:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* the command line                                                    */

/*
 * The conflict a path belongs to: the one the file in front of us still
 * carries, or, once it has been resolved, the one it was filed under at the
 * time.  1 with the hex when there is one, 0 when there is not.
 */
static int rr_path_id(struct repo *r, const char *path,
		      char hex[GP_SHA1_HEXSZ + 1])
{
	struct buf f = BUF_INIT;
	char *full = xstrfmt("%s/%s", r->root, path);
	int rc = 0;

	if (read_file(full, &f) == 0 && rr_id(&f, hex) == 1)
		rc = 1;
	buf_release(&f);
	free(full);
	if (!rc && rr_pending_lookup(r, path, hex))
		rc = 1;
	return rc;
}

/* put the resolved text in the work tree and, when asked, in the index too */
static void rr_write_and_stage(struct repo *r, struct index_state *istate,
			       const char *path, const struct buf *content,
			       int stage)
{
	char *full = xstrfmt("%s/%s", r->root, path);
	struct index_entry e, *base;

	write_file(full, content->b, content->len);
	free(full);
	if (!stage)
		return;
	base = index_get(istate, path);
	if (!base)
		return;
	e = *base;
	odb_write(&r->odb, OBJ_BLOB, content->b, content->len, &e.oid);
	e.path = (char *)path;
	e.stage = 0;
	index_add(istate, &e);
	index_write(istate, repo_index_path(r));
}

/*
 * A bare `rerere`: walk the unmerged paths.  A file still holding conflicts
 * has them recorded, or resolved from the cache if they have been seen before.
 * A file that no longer holds any is one that has just been resolved by hand,
 * and what it became is written down as the answer for the next time.
 */
static int rr_run(struct repo *r)
{
	struct index_state istate = INDEX_INIT;
	char **paths;
	size_t nr = 0, i;

	index_read(&istate, repo_index_path(r));
	paths = index_unmerged_paths(&istate, &nr);
	for (i = 0; i < nr; i++) {
		struct buf file = BUF_INIT, out = BUF_INIT;
		char *full = xstrfmt("%s/%s", r->root, paths[i]);
		char hex[GP_SHA1_HEXSZ + 1];
		int id;

		if (read_file(full, &file) < 0) {
			free(full);
			continue;
		}
		free(full);
		id = rr_id(&file, hex);
		if (id == 1) {
			enum rr_result res = rerere_auto(r, paths[i], &file, &out);

			rerere_report(paths[i], res);
			if (res != RR_NONE && res != RR_RECORDED)
				rr_write_and_stage(r, &istate, paths[i], &out,
						   res == RR_STAGED);
		} else if (id == 0 && rr_pending_lookup(r, paths[i], hex)) {
			/*
			 * The conflict is gone from the file, so what it became
			 * is the answer.  The path stays in MERGE_RR until the
			 * merge itself is over, which is what lets the file be
			 * looked at again -- and resolved again, differently,
			 * if the first answer was not right.
			 */
			rr_write(r, hex, "postimage", file.b, file.len);
			printf("Recorded resolution for '%s'.\n", paths[i]);
		}
		buf_release(&out);
		buf_release(&file);
	}
	index_paths_free(paths);
	index_release(&istate);
	return 0;
}

/*
 * The conflicts waiting to be resolved, each with the id it is filed under and
 * whether a resolution has been recorded for it.  The format is this command's
 * own -- git's is not one a script can be written against either.
 */
static int rr_status(struct repo *r)
{
	struct index_state istate = INDEX_INIT;
	char **paths;
	size_t nr = 0, i;

	index_read(&istate, repo_index_path(r));
	paths = index_unmerged_paths(&istate, &nr);
	for (i = 0; i < nr; i++) {
		char hex[GP_SHA1_HEXSZ + 1];

		if (!rr_path_id(r, paths[i], hex))
			continue;
		printf("%s\n", paths[i]);
		printf("  %s %s\n", hex,
		       rr_has(r, hex, "postimage") ? "resolved" : "preimage");
	}
	index_paths_free(paths);
	index_release(&istate);
	return 0;
}

/*
 * What the recorded resolution would change, for the conflicts in front of us:
 * the conflict as it was met, against the file as it was resolved.  There is
 * nothing to show for a conflict with no resolution recorded yet.
 */
static int rr_diff(struct repo *r)
{
	struct index_state istate = INDEX_INIT;
	char **paths;
	size_t nr = 0, i;

	index_read(&istate, repo_index_path(r));
	paths = index_unmerged_paths(&istate, &nr);
	for (i = 0; i < nr; i++) {
		struct buf pre = BUF_INIT, post = BUF_INIT, out = BUF_INIT;
		char hex[GP_SHA1_HEXSZ + 1];

		if (rr_path_id(r, paths[i], hex) &&
		    rr_read(r, hex, "preimage", &pre) == 0 &&
		    rr_read(r, hex, "postimage", &post) == 0)
			diff_buffers("preimage", pre.b, pre.len, "postimage",
				     post.b, post.len, &out, 0);
		if (out.len)
			fwrite(out.b, 1, out.len, stdout);
		buf_release(&out);
		buf_release(&post);
		buf_release(&pre);
	}
	index_paths_free(paths);
	index_release(&istate);
	return 0;
}

static int rr_forget_one(struct repo *r, const char *path)
{
	char hex[GP_SHA1_HEXSZ + 1];

	if (!rr_path_id(r, path, hex)) {
		gp_error("rerere: '%s' is not a conflict", path);
		return 1;
	}
	rr_entry_remove(r, hex);
	rr_pending_remove(r, path);
	printf("Forgot resolution for '%s'.\n", path);
	return 0;
}

static int rr_name_is_hex(const char *s)
{
	size_t i;

	if (strlen(s) != GP_SHA1_HEXSZ)
		return 0;
	for (i = 0; i < GP_SHA1_HEXSZ; i++)
		if (!isxdigit((unsigned char)s[i]))
			return 0;
	return 1;
}

/* drop the entries that have no resolution in them, and so nothing to reuse */
static int rr_gc(struct repo *r)
{
	char *d = rr_cache_dir(r);
	DIR *dp = opendir(d);
	struct dirent *de;

	if (!dp) {
		free(d);
		return 0;
	}
	while ((de = readdir(dp))) {
		char *e;

		if (!rr_name_is_hex(de->d_name) ||
		    rr_has(r, de->d_name, "postimage"))
			continue;
		e = xstrfmt("%s/%s", d, de->d_name);
		remove_dir_recursive(e);
		free(e);
	}
	closedir(dp);
	free(d);
	return 0;
}

static int rr_clear(struct repo *r)
{
	char *d = rr_cache_dir(r);
	char *m = rr_merge_rr(r);

	remove_dir_recursive(d);
	remove_file(m);
	free(d);
	free(m);
	return 0;
}

int cmd_rerere(struct repo *r, int argc, char **argv)
{
	struct opts o;
	int status_, diff, forget, gc, clear_;

	opts_init(&o, argc, argv, (const char *const[]){
		"--status", "--diff", "--forget", "--gc", "--clear", NULL });

	status_ = opts_flag(&o, "--status");
	diff = opts_flag(&o, "--diff");
	forget = opts_flag(&o, "--forget");
	gc = opts_flag(&o, "--gc");
	clear_ = opts_flag(&o, "--clear");

	if (status_ + diff + forget + gc + clear_ > 1) {
		gp_error("rerere: give at most one of --status, --diff, "
			 "--forget, --gc, --clear");
		return 1;
	}
	if (status_ || diff) {
		if (opts_count(&o) > 0) {
			gp_error("rerere: --%s takes no paths",
				 status_ ? "status" : "diff");
			return 1;
		}
		return status_ ? rr_status(r) : rr_diff(r);
	}
	if (forget) {
		int i, rc = 0;

		if (opts_count(&o) < 1) {
			gp_error("rerere: --forget needs at least one path\n"
				 "usage: gitprompt rerere --forget <path>...");
			return 1;
		}
		for (i = 0; i < opts_count(&o); i++)
			if (rr_forget_one(r, opts_arg(&o, i)))
				rc = 1;
		return rc;
	}
	if (gc)
		return rr_gc(r);
	if (clear_)
		return rr_clear(r);
	if (opts_count(&o) > 0) {
		gp_error("rerere: unexpected argument '%s'", opts_arg(&o, 0));
		return 1;
	}
	return rr_run(r);
}
