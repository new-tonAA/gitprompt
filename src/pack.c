/*
 * pack.c - packfiles: reading them, and writing the one `gc` makes.
 *
 * A pack holds many objects in one file, deflated, and some of them are
 * stored as a delta against another object in the same pack.  git writes one
 * on every fetch, so a gitprompt store that the git binary has fetched into
 * holds most of its objects here rather than loose -- which is why this file
 * has to exist: without it, a delegated `fetch` leaves the store unreadable
 * to its own tool.
 *
 *   .pack  "PACK", version 2, count, then per object a header and deflated
 *          data, then the SHA-1 of everything before it.
 *   .idx   what to find where: a fanout table, the sorted ids, their crc32s
 *          and their offsets, then the pack's checksum and the index's own.
 *
 * Version 2 of both is written.  Both versions of the index are read, since
 * a repository old enough to hold a v1 index is exactly the kind this should
 * not refuse.
 */
#include "gp.h"

#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include <zlib.h>

/* the pack's type codes, which are not the object type enum's numbering */
#define PACK_COMMIT    1
#define PACK_TREE      2
#define PACK_BLOB      3
#define PACK_TAG       4
#define PACK_OFS_DELTA 6
#define PACK_REF_DELTA 7

/* a delta chain deeper than this is a corrupt pack, or a hostile one */
#define PACK_MAX_DEPTH 200

struct pack {
	char *pack_path;
	char *idx_path;
	struct buf idx;         /* the whole .idx, held for binary search */
	size_t nr;              /* objects in this pack */
	const u8 *fanout;       /* 256 big-endian cumulative counts */
	const u8 *sha1s;        /* nr ids, 20 bytes each, ascending (v2) */
	const u8 *offsets;      /* nr 4-byte offsets, v2 */
	const u8 *large;        /* the 8-byte overflow offsets behind them */
	size_t nr_large;
	int v1;
};

/* ------------------------------------------------------------------ */
/* big-endian reads                                                    */

static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u64 be64(const u8 *p)
{
	return ((u64)be32(p) << 32) | be32(p + 4);
}

static void put_be32(struct buf *b, u32 v)
{
	u8 t[4];
	t[0] = (u8)(v >> 24);
	t[1] = (u8)(v >> 16);
	t[2] = (u8)(v >> 8);
	t[3] = (u8)v;
	buf_add(b, t, 4);
}

static void hex_of(const oid_t *o, char *out)
{
	oid_hex(o, out);
}

/* ------------------------------------------------------------------ */
/* opening an index                                                    */

/*
 * Load a .idx and point its tables into the loaded bytes.  The tables are
 * views rather than copies, so the buffer has to outlive them -- it does,
 * living in the same struct.
 */
static int pack_open_idx(struct pack *p, const char *idx_path,
			 const char *pack_path)
{
	const u8 *b;
	size_t len;
	u32 nr;

	memset(p, 0, sizeof *p);
	buf_init(&p->idx);
	if (read_file(idx_path, &p->idx) < 0)
		return -1;
	b = p->idx.b;
	len = p->idx.len;

	if (len >= 8 && b[0] == 0xff && b[1] == 't' && b[2] == 'O' && b[3] == 'c') {
		u32 version = be32(b + 4);

		if (version != 2) {
			gp_error("%s: unsupported pack index version %lu",
				 idx_path, (unsigned long)version);
			goto bad;
		}
		if (len < 8 + 256 * 4)
			goto bad;
		nr = be32(b + 8 + 255 * 4);
		if (len < 8 + 256 * 4 + (size_t)nr * 20 + (size_t)nr * 4
			  + (size_t)nr * 4 + 40)
			goto bad;
		p->fanout = b + 8;
		p->sha1s = b + 8 + 256 * 4;
		p->offsets = p->sha1s + (size_t)nr * 20 + (size_t)nr * 4;
		p->v1 = 0;
		/*
		 * An offset whose top bit is set is not an offset but an
		 * index into the 8-byte overflow table that follows.
		 */
		{
			const u8 *large = p->offsets + (size_t)nr * 4;
			size_t i, n = 0;

			for (i = 0; i < nr; i++)
				if (be32(p->offsets + i * 4) & 0x80000000u)
					n++;
			if (n) {
				if (len < (size_t)(large - b) + n * 8 + 40)
					goto bad;
				p->large = large;
				p->nr_large = n;
			}
		}
	} else {
		/* v1: the fanout, then (offset, id) pairs, sorted by id */
		if (len < 256 * 4 + 24)
			goto bad;
		nr = be32(b + 255 * 4);
		if (len < 256 * 4 + (size_t)nr * 24 + 40)
			goto bad;
		p->fanout = b;
		p->sha1s = NULL;
		p->offsets = NULL;
		p->v1 = 1;
	}

	p->nr = nr;
	p->pack_path = xstrdup(pack_path);
	p->idx_path = xstrdup(idx_path);
	return 0;
bad:
	buf_release(&p->idx);
	return -1;
}

static void pack_release(struct pack *p)
{
	buf_release(&p->idx);
	free(p->pack_path);
	free(p->idx_path);
	memset(p, 0, sizeof *p);
}

static int pack_oid_at(const struct pack *p, size_t i, oid_t *out)
{
	if (i >= p->nr)
		return -1;
	if (p->v1)
		memcpy(out->raw, p->idx.b + 256 * 4 + i * 24 + 4, GP_SHA1_RAWSZ);
	else
		memcpy(out->raw, p->sha1s + i * 20, GP_SHA1_RAWSZ);
	return 0;
}

/*
 * Binary search the id table.  The fanout narrows the range first, which is
 * what it is for: every id whose first byte is below this one's sorts before
 * it, so the search never has to look at them.
 */
static int pack_find_in(const struct pack *p, const oid_t *oid, size_t *out_i)
{
	u32 lo, hi;
	u8 first = oid->raw[0];

	if (!p->nr)
		return -1;
	lo = first ? be32(p->fanout + (size_t)(first - 1) * 4) : 0;
	hi = be32(p->fanout + (size_t)first * 4);
	if (hi > p->nr)
		return -1;

	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2;
		oid_t mid_oid;
		int cmp;

		if (pack_oid_at(p, mid, &mid_oid) < 0)
			return -1;
		cmp = memcmp(oid->raw, mid_oid.raw, GP_SHA1_RAWSZ);
		if (cmp == 0) {
			*out_i = mid;
			return 0;
		}
		if (cmp < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return -1;
}

static int pack_offset_at(const struct pack *p, size_t i, u64 *out)
{
	u32 v;

	if (p->v1) {
		*out = be32(p->idx.b + 256 * 4 + i * 24);
		return 0;
	}
	v = be32(p->offsets + i * 4);
	if (v & 0x80000000u) {
		size_t li = v & 0x7fffffffu;

		if (li >= p->nr_large)
			return -1;
		*out = be64(p->large + li * 8);
	} else {
		*out = v;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* inflating out of the pack                                           */

/*
 * A streaming reader over the pack file, so a pack larger than memory is
 * still readable: zlib is fed in chunks rather than handed the whole file.
 */
struct pin {
	FILE *f;
	u8 buf[65536];
	size_t pos, have;
};

static int pin_open(struct pin *p, const char *path, u64 offset)
{
	memset(p, 0, sizeof *p);
	p->f = fopen(path, "rb");
	if (!p->f)
		return -1;
	if (fseek(p->f, (long)offset, SEEK_SET) != 0) {
		fclose(p->f);
		p->f = NULL;
		return -1;
	}
	return 0;
}

static void pin_close(struct pin *p)
{
	if (p->f)
		fclose(p->f);
	p->f = NULL;
}

static int pin_seek(struct pin *p, u64 offset)
{
	if (fseek(p->f, (long)offset, SEEK_SET) != 0)
		return -1;
	p->pos = p->have = 0;
	return 0;
}

/* refill from the file; 1 if bytes are available, 0 at end of file */
static int pin_refill(struct pin *p)
{
	size_t n;

	if (p->pos < p->have)
		return 1;
	n = fread(p->buf, 1, sizeof p->buf, p->f);
	p->pos = 0;
	p->have = n;
	return n > 0;
}

/* take up to n bytes; returns how many were taken */
static size_t pin_take(struct pin *p, u8 *dst, size_t n)
{
	size_t got = 0;

	while (got < n) {
		size_t avail, take;

		if (!pin_refill(p))
			break;
		avail = p->have - p->pos;
		take = n - got < avail ? n - got : avail;
		if (dst)
			memcpy(dst + got, p->buf + p->pos, take);
		p->pos += take;
		got += take;
	}
	return got;
}

/*
 * Inflate one deflate stream, all of it.  The stream has to end on its own
 * and produce exactly `want` bytes -- the length the object header promised
 * -- so a truncated or padded pack is rejected here rather than turned into
 * an object with plausible contents and the wrong id.
 */
static int pin_inflate(struct pin *p, u64 want, struct buf *out)
{
	z_stream zs;
	int done = 0;

	memset(&zs, 0, sizeof zs);
	if (inflateInit(&zs) != Z_OK)
		return -1;
	buf_reset(out);

	while (!done) {
		size_t avail;
		int rc;

		if (zs.avail_in == 0 && !pin_refill(p))
			break;                  /* input ran out mid-stream */
		avail = p->have - p->pos;
		zs.next_in = p->buf + p->pos;
		zs.avail_in = (uInt)avail;

		buf_grow(out, 65536);           /* buf_grow leaves a spare byte */
		zs.next_out = out->b + out->len;
		zs.avail_out = (uInt)(out->cap - out->len);

		rc = inflate(&zs, Z_NO_FLUSH);
		p->pos += avail - zs.avail_in;
		out->len = (size_t)(zs.next_out - out->b);

		if (rc == Z_STREAM_END)
			done = 1;
		else if (rc != Z_OK && rc != Z_BUF_ERROR) {
			inflateEnd(&zs);
			return -1;
		}
	}
	inflateEnd(&zs);
	return (done && out->len == (size_t)want) ? 0 : -1;
}

/*
 * The header at the top of a packed object: a varint carrying the type in
 * its high bits and the inflated size in its low ones, least significant
 * group first.
 */
static int parse_obj_header(const u8 *b, size_t len, int *code, u64 *size,
			    size_t *used)
{
	u64 sz;
	int shift = 4;
	size_t i = 1;
	u8 c;

	if (len < 1)
		return -1;
	c = b[0];
	*code = (c >> 4) & 7;
	sz = c & 0x0f;
	while (c & 0x80) {
		if (i >= len || shift >= 64)
			return -1;
		c = b[i++];
		sz |= (u64)(c & 0x7f) << shift;
		shift += 7;
	}
	*size = sz;
	*used = i;
	return 0;
}

static int code_to_type(int code, enum obj_type *t)
{
	switch (code) {
	case PACK_COMMIT: *t = OBJ_COMMIT; return 0;
	case PACK_TREE:   *t = OBJ_TREE;   return 0;
	case PACK_BLOB:   *t = OBJ_BLOB;   return 0;
	case PACK_TAG:    *t = OBJ_TAG;    return 0;
	default:
		return -1;
	}
}

/* ------------------------------------------------------------------ */
/* deltas                                                              */

static u64 delta_size(const u8 *p, size_t len, size_t *used)
{
	u64 v = 0;
	int shift = 0;
	size_t i = 0;
	u8 c = 0;

	do {
		if (i >= len || shift >= 64)
			break;
		c = p[i++];
		v |= (u64)(c & 0x7f) << shift;
		shift += 7;
	} while (c & 0x80);
	*used = i;
	return v;
}

/*
 * Apply a delta: a source size, a target size, then copy and insert
 * commands.  Both sizes are checked against what is really there, so a delta
 * that does not add up is refused instead of producing a plausible object.
 */
static int apply_delta(const u8 *d, size_t dlen, const struct buf *base,
		       struct buf *out)
{
	size_t p = 0, used;
	u64 src, dst;

	src = delta_size(d + p, dlen - p, &used);
	p += used;
	if (src != base->len)
		return -1;
	dst = delta_size(d + p, dlen - p, &used);
	p += used;

	buf_reset(out);
	buf_grow(out, (size_t)dst);
	while (p < dlen) {
		u8 cmd = d[p++];

		if (cmd & 0x80) {
			u64 off = 0, n = 0;
			size_t i;

			for (i = 0; i < 4; i++)
				if (cmd & (1u << i)) {
					if (p >= dlen)
						return -1;
					off |= (u64)d[p++] << (8 * i);
				}
			for (i = 0; i < 3; i++)
				if (cmd & (0x10u << i)) {
					if (p >= dlen)
						return -1;
					n |= (u64)d[p++] << (8 * i);
				}
			if (n == 0)
				n = 0x10000;    /* the encoding's way of saying 64K */
			if (off + n > base->len)
				return -1;
			buf_add(out, base->b + off, (size_t)n);
		} else if (cmd) {
			if (p + cmd > dlen)
				return -1;
			buf_add(out, d + p, cmd);
			p += cmd;
		} else {
			return -1;              /* 0 is reserved */
		}
	}
	return out->len == (size_t)dst ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* reading one object                                                  */

static int pack_read_at(struct odb *o, const struct pack *pk, u64 offset,
			enum obj_type *type, struct buf *out, int depth);

/* a ref-delta names its base by id, which may sit in any pack, or loose */
static int read_base_by_oid(struct odb *o, const oid_t *base,
			    enum obj_type *type, struct buf *out, int depth)
{
	size_t i;

	for (i = 0; i < o->nr_packs; i++) {
		size_t idx;
		u64 off;

		if (pack_find_in(&o->packs[i], base, &idx) < 0)
			continue;
		if (pack_offset_at(&o->packs[i], idx, &off) < 0)
			return -1;
		return pack_read_at(o, &o->packs[i], off, type, out, depth + 1);
	}
	return odb_read(o, base, type, out);
}

static int pack_read_at(struct odb *o, const struct pack *pk, u64 offset,
			enum obj_type *type, struct buf *out, int depth)
{
	u8 hdr[64];
	size_t got, used;
	int code;
	u64 size;
	struct pin in;
	struct buf raw, base;
	int rc = -1;

	if (depth > PACK_MAX_DEPTH)
		return -1;
	if (pin_open(&in, pk->pack_path, offset) < 0) {
		return -1;
	}
	got = pin_take(&in, hdr, sizeof hdr);
	if (parse_obj_header(hdr, got, &code, &size, &used) < 0) {
		pin_close(&in);
		return -1;
	}

	buf_init(&raw);
	buf_init(&base);

	if (code == PACK_OFS_DELTA) {
		/* how far back the base is, as a big-endian varint */
		u64 back, at;
		size_t q = used;
		u8 c;

		if (q >= got)
			goto out;
		c = hdr[q++];
		back = c & 0x7f;
		while (c & 0x80) {
			if (q >= got)
				goto out;
			c = hdr[q++];
			back = ((back + 1) << 7) | (c & 0x7f);
		}
		if (back > offset)
			goto out;
		at = offset - back;
		if (pin_seek(&in, offset + q) < 0)
			goto out;
		if (pin_inflate(&in, size, &raw) < 0)
			goto out;
		if (pack_read_at(o, pk, at, type, &base, depth + 1) < 0)
			goto out;
		if (apply_delta(raw.b, raw.len, &base, out) < 0)
			goto out;
		rc = 0;
	} else if (code == PACK_REF_DELTA) {
		oid_t base_oid;
		enum obj_type bt;

		if (used + GP_SHA1_RAWSZ > got)
			goto out;
		memcpy(base_oid.raw, hdr + used, GP_SHA1_RAWSZ);
		if (pin_seek(&in, offset + used + GP_SHA1_RAWSZ) < 0)
			goto out;
		if (pin_inflate(&in, size, &raw) < 0)
			goto out;
		if (read_base_by_oid(o, &base_oid, &bt, &base, depth + 1) < 0)
			goto out;
		if (type)
			*type = bt;
		if (apply_delta(raw.b, raw.len, &base, out) < 0)
			goto out;
		rc = 0;
	} else {
		if (code_to_type(code, type) < 0)
			goto out;
		if (pin_seek(&in, offset + used) < 0)
			goto out;
		rc = pin_inflate(&in, size, out);
	}

out:
	buf_release(&raw);
	buf_release(&base);
	pin_close(&in);
	return rc;
}

/* ------------------------------------------------------------------ */
/* the store's packs                                                   */

void pack_load_all(struct odb *o)
{
	DIR *d;
	struct dirent *de;
	char *packdir;

	if (o->packs_loaded)
		return;
	o->packs_loaded = 1;

	packdir = xstrfmt("%s/pack", o->dir);
	d = opendir(packdir);
	if (!d) {
		free(packdir);
		return;                 /* no packs, which is not an error */
	}
	while ((de = readdir(d))) {
		const char *n = de->d_name;
		size_t nlen = strlen(n);
		char *idx_path, *pack_path;
		struct pack p;

		if (nlen < 6 || strcmp(n + nlen - 4, ".idx"))
			continue;
		idx_path = xstrfmt("%s/%s", packdir, n);
		pack_path = xstrfmt("%s/%.*s.pack", packdir, (int)(nlen - 4), n);

		if (pack_open_idx(&p, idx_path, pack_path) < 0) {
			gp_error("ignoring unreadable pack index %s", idx_path);
			free(idx_path);
			free(pack_path);
			continue;
		}
		if (o->nr_packs == o->alloc_packs) {
			o->alloc_packs = o->alloc_packs ? o->alloc_packs * 2 : 4;
			o->packs = xrealloc(o->packs,
					    o->alloc_packs * sizeof(*o->packs));
		}
		o->packs[o->nr_packs++] = p;
		free(idx_path);
		free(pack_path);
	}
	closedir(d);
	free(packdir);
}

void pack_release_all(struct odb *o)
{
	size_t i;

	for (i = 0; i < o->nr_packs; i++)
		pack_release(&o->packs[i]);
	free(o->packs);
	o->packs = NULL;
	o->nr_packs = o->alloc_packs = 0;
	o->packs_loaded = 0;
}

int pack_has(struct odb *o, const oid_t *oid)
{
	size_t i, idx;

	pack_load_all(o);
	for (i = 0; i < o->nr_packs; i++)
		if (pack_find_in(&o->packs[i], oid, &idx) == 0)
			return 1;
	return 0;
}

int pack_read_object(struct odb *o, const oid_t *oid, enum obj_type *type,
		     struct buf *out)
{
	size_t i;

	pack_load_all(o);
	for (i = 0; i < o->nr_packs; i++) {
		size_t idx;
		u64 off;

		if (pack_find_in(&o->packs[i], oid, &idx) < 0)
			continue;
		if (pack_offset_at(&o->packs[i], idx, &off) < 0)
			return -1;
		return pack_read_at(o, &o->packs[i], off, type, out, 0);
	}
	return -1;
}

size_t pack_count_all(struct odb *o)
{
	size_t i, n = 0;

	pack_load_all(o);
	for (i = 0; i < o->nr_packs; i++)
		n += o->packs[i].nr;
	return n;
}

size_t pack_nr(struct odb *o)
{
	pack_load_all(o);
	return o->nr_packs;
}

/*
 * On-disk size of every pack and its index, which is what count-objects
 * wants to say next to the count it already gets from the index.
 */
size_t pack_disk_size(struct odb *o)
{
	size_t i, n = 0;

	pack_load_all(o);
	for (i = 0; i < o->nr_packs; i++) {
		struct stat st;

		if (stat(o->packs[i].pack_path, &st) == 0)
			n += (size_t)st.st_size;
		if (stat(o->packs[i].idx_path, &st) == 0)
			n += (size_t)st.st_size;
	}
	return n;
}

static int pack_id_cmp(const void *a, const void *b)
{
	return memcmp(a, b, GP_SHA1_RAWSZ);
}

/* 1 if every object in the pack is in the sorted array `seen` */
static int pack_wholly_in(const struct pack *p, const oid_t *seen, size_t nr)
{
	size_t j;

	if (!nr || !p->nr)
		return 0;
	for (j = 0; j < p->nr; j++) {
		oid_t id;

		if (pack_oid_at(p, j, &id) < 0)
			return 0;
		if (!bsearch(&id, seen, nr, sizeof(*seen), pack_id_cmp))
			return 0;
	}
	return 1;
}

/*
 * Delete every pack whose objects are all in `seen` -- a pack written just
 * now holds them, so the old file is a duplicate of it.
 *
 * A pack holding anything *not* reachable is left alone: gc's grace period is
 * about exactly those objects, and deleting their only copy is the one
 * outcome this must never have.  So is the pack named by keep_path, which is
 * the one just written: a store that has not changed since the last gc
 * rewrites the same bytes under the same name, and dropping that would take
 * the objects with it.
 */
size_t pack_drop_redundant(struct odb *o, const oid_t *seen, size_t nr_seen,
			   const char *keep_path, int dry_run)
{
	size_t i, dropped = 0;

	pack_load_all(o);
	for (i = 0; i < o->nr_packs; i++) {
		char *rev;

		if (keep_path && !strcmp(o->packs[i].pack_path, keep_path))
			continue;
		if (!pack_wholly_in(&o->packs[i], seen, nr_seen))
			continue;
		rev = xstrfmt("%.*s.rev",
			      (int)(strlen(o->packs[i].pack_path) - 5),
			      o->packs[i].pack_path);
		/*
		 * Both halves have to go, or the pack was not dropped: an index
		 * without a pack is not a store, but a removal that failed still
		 * leaves a readable pack, and counting that as gone would report
		 * work that did not happen.
		 */
		if (!dry_run) {
			/* the pack first: an index with no pack is inert, but a
			 * pack with no index would be unreadable, and the objects
			 * in it would be gone */
			if (remove_file(o->packs[i].pack_path) < 0) {
				gp_warn("cannot remove %s; it was not replaced",
					o->packs[i].pack_path);
				free(rev);
				continue;
			}
			remove_file(o->packs[i].idx_path);
			remove_file(rev);
		}
		free(rev);
		dropped++;
	}
	return dropped;
}

void pack_foreach(struct odb *o, int (*fn)(const oid_t *, void *), void *data)
{
	size_t i, j;

	pack_load_all(o);
	for (i = 0; i < o->nr_packs; i++)
		for (j = 0; j < o->packs[i].nr; j++) {
			oid_t oid;

			if (pack_oid_at(&o->packs[i], j, &oid) < 0)
				continue;
			if (fn(&oid, data))
				return;
		}
}

/*
 * Prefix lookup over the packed ids.  The table is sorted, so the ids that
 * match sit together and the scan stops at the first that does not; two
 * matches is all the caller needs to hear about.
 */
int pack_resolve_prefix(struct odb *o, const char *hex, oid_t *out)
{
	char want[GP_SHA1_HEXSZ + 1];
	size_t i, j, n = strlen(hex);
	oid_t hit;
	int found = 0;

	if (n < 4 || n > GP_SHA1_HEXSZ)
		return -1;
	for (i = 0; i < n; i++) {
		char c = hex[i];

		if (c >= 'A' && c <= 'F')
			c = (char)(c - 'A' + 'a');
		if (!strchr("0123456789abcdef", c))
			return -1;
		want[i] = c;
	}
	want[n] = '\0';

	pack_load_all(o);
	for (i = 0; i < o->nr_packs; i++)
		for (j = 0; j < o->packs[i].nr; j++) {
			char have[GP_SHA1_HEXSZ + 1];
			oid_t cand;

			if (pack_oid_at(&o->packs[i], j, &cand) < 0)
				continue;
			hex_of(&cand, have);
			if (strncmp(have, want, n))
				continue;
			/* the same object in two packs is still one object:
			 * only a second id can make a prefix ambiguous */
			if (found && memcmp(hit.raw, cand.raw, GP_SHA1_RAWSZ))
				return -2;      /* ambiguous */
			hit = cand;
			found = 1;
		}
	if (!found)
		return -1;
	*out = hit;
	return 0;
}

/* ------------------------------------------------------------------ */
/* writing a pack                                                      */

struct wr_entry {
	oid_t oid;
	u32 crc;
	u64 offset;
};

static int wr_cmp(const void *a, const void *b)
{
	return memcmp(((const struct wr_entry *)a)->oid.raw,
		      ((const struct wr_entry *)b)->oid.raw, GP_SHA1_RAWSZ);
}

/* the same varint shape an object header uses, for a type and a size */
static void put_obj_header(struct buf *b, int code, u64 size)
{
	u8 first = (u8)((code << 4) | (size & 0x0f));
	u64 rest = size >> 4;

	if (rest)
		first |= 0x80;
	buf_addch(b, first);
	while (rest) {
		u8 c = (u8)(rest & 0x7f);

		rest >>= 7;
		if (rest)
			c |= 0x80;
		buf_addch(b, c);
	}
}

/*
 * Write `oids` as one pack and its v2 index, named by the pack's own
 * checksum so git's own tools agree about which pack is which.  Objects go
 * in id order, which is what lets the offset table be written in one pass.
 * They are written whole -- deflated, but not delta'd against each other --
 * because the point of packing here is to make the store readable by git and
 * by us, not to squeeze out the last byte.
 */
int pack_write(struct odb *o, const oid_t *oids, size_t nr, const char *dir,
	       u8 out_sha[GP_SHA1_RAWSZ])
{
	struct wr_entry *e;
	struct buf pack, idx;
	u8 digest[GP_SHA1_RAWSZ];
	char name[GP_SHA1_HEXSZ + 1];
	char *packpath = NULL, *idxpath = NULL;
	size_t i, n = nr;
	int rc = -1;
	gp_sha1_ctx sha;

	if (!n)
		return -1;

	e = xmalloc(n * sizeof(*e));
	for (i = 0; i < n; i++)
		e[i].oid = oids[i];
	qsort(e, n, sizeof(*e), wr_cmp);

	/* two of the same id would give the offset table an entry too many */
	{
		size_t w = 0;

		for (i = 0; i < n; i++) {
			if (w > 0 && memcmp(e[w - 1].oid.raw, e[i].oid.raw,
					    GP_SHA1_RAWSZ) == 0)
				continue;
			e[w++] = e[i];
		}
		n = w;
	}

	buf_init(&pack);
	buf_init(&idx);

	buf_addstr(&pack, "PACK");
	put_be32(&pack, 2);
	put_be32(&pack, (u32)n);

	for (i = 0; i < n; i++) {
		struct buf data;
		enum obj_type t;
		uLongf bound;
		u8 *dst;
		int code;

		buf_init(&data);
		if (odb_read(o, &e[i].oid, &t, &data) < 0) {
			char hex[GP_SHA1_HEXSZ + 1];

			oid_hex(&e[i].oid, hex);
			gp_error("gc: cannot read %s to pack it", hex);
			buf_release(&data);
			goto out;
		}
		switch (t) {
		case OBJ_COMMIT: code = PACK_COMMIT; break;
		case OBJ_TREE:   code = PACK_TREE;   break;
		case OBJ_BLOB:   code = PACK_BLOB;   break;
		case OBJ_TAG:    code = PACK_TAG;    break;
		default:
			buf_release(&data);
			goto out;
		}

		e[i].offset = pack.len;
		put_obj_header(&pack, code, data.len);

		bound = compressBound((uLong)data.len);
		dst = xmalloc(bound ? bound : 1);
		if (compress2(dst, &bound, (const Bytef *)data.b, (uLong)data.len,
			      Z_DEFAULT_COMPRESSION) != Z_OK) {
			free(dst);
			buf_release(&data);
			goto out;
		}
		buf_add(&pack, dst, bound);
		free(dst);
		/* the index's crc covers the object's bytes in the pack, this
		 * header included */
		e[i].crc = (u32)crc32(crc32(0L, Z_NULL, 0),
				      pack.b + e[i].offset,
				      (uInt)(pack.len - e[i].offset));
		buf_release(&data);
	}

	/* the pack's checksum is the id the index points back at */
	gp_sha1_init(&sha);
	gp_sha1_update(&sha, pack.b, pack.len);
	gp_sha1_final(&sha, digest);
	buf_add(&pack, digest, sizeof digest);

	/* index: magic, version, fanout */
	{
		u8 magic[4] = { 0xff, 't', 'O', 'c' };

		buf_add(&idx, magic, 4);
	}
	put_be32(&idx, 2);
	{
		u32 counts[256];
		size_t cum = 0;
		int b;

		memset(counts, 0, sizeof counts);
		for (i = 0; i < n; i++)
			counts[e[i].oid.raw[0]]++;
		for (b = 0; b < 256; b++) {
			cum += counts[b];
			put_be32(&idx, (u32)cum);
		}
	}
	for (i = 0; i < n; i++)
		buf_add(&idx, e[i].oid.raw, GP_SHA1_RAWSZ);
	for (i = 0; i < n; i++)
		put_be32(&idx, e[i].crc);
	for (i = 0; i < n; i++) {
		if (e[i].offset > 0x7fffffffu) {
			char hex[GP_SHA1_HEXSZ + 1];

			oid_hex(&e[i].oid, hex);
			gp_error("gc: %s lies past the 2GB offset the index "
				 "can hold", hex);
			goto out;
		}
		put_be32(&idx, (u32)e[i].offset);
	}
	buf_add(&idx, digest, sizeof digest);
	gp_sha1_init(&sha);
	gp_sha1_update(&sha, idx.b, idx.len);
	gp_sha1_final(&sha, digest);
	buf_add(&idx, digest, sizeof digest);

	/* name the pair after the pack's own checksum, as git does */
	{
		const u8 *trailer = pack.b + pack.len - GP_SHA1_RAWSZ;
		oid_t pack_oid;

		memcpy(pack_oid.raw, trailer, GP_SHA1_RAWSZ);
		oid_hex(&pack_oid, name);
		if (out_sha)
			memcpy(out_sha, trailer, GP_SHA1_RAWSZ);
	}

	packpath = xstrfmt("%s/pack-%s.pack", dir, name);
	idxpath = xstrfmt("%s/pack-%s.idx", dir, name);
	if (mkdir_p(dir) < 0) {
		gp_error("gc: cannot create %s", dir);
		goto out;
	}
	if (write_file(packpath, pack.b, pack.len) < 0 ||
	    write_file(idxpath, idx.b, idx.len) < 0) {
		gp_error("gc: cannot write the pack");
		goto out;
	}
	rc = 0;
out:
	free(packpath);
	free(idxpath);
	buf_release(&pack);
	buf_release(&idx);
	free(e);
	return rc;
}
