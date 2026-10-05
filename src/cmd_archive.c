/*
 * archive -- write the files of a tree into a tar or zip container.
 *
 * This is git's archive, so a `gitprompt archive` and a `git archive` of the
 * same tree hold the same files with the same contents; the containers are
 * plain ustar and plain zip, which is why `tar -tf` and `unzip -l` read them
 * without being told anything.  Nothing about the repository is touched: the
 * command reads the tree and writes a stream.
 *
 * The zip entries are stored, not deflated.  A deflated entry needs a raw
 * deflate stream, and the only deflate this tree has is zlib's, which wraps
 * one in a zlib header a zip reader will not accept; storing is a format
 * `unzip` reads exactly, so it is the one that can be right.  The tar entries
 * are ustar, so a path longer than the 100-byte name field is refused rather
 * than written as a pax extension git would write.
 */
#include "gp.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ------------------------------------------------------------------ */
/* tar                                                                 */

struct ar_ctx {
	struct repo *r;
	struct buf *out;
	long mtime;
	int failed;
};

static void put_octal(char *field, size_t width, unsigned long long value)
{
	size_t digits = width - 1;    /* the field ends in the NUL we write */
	size_t i;

	for (i = 0; i < digits; i++) {
		field[digits - 1 - i] = (char)('0' + (int)(value & 7));
		value >>= 3;
	}
	field[digits] = '\0';
}

static void tar_header(struct buf *out, const char *path, u32 mode,
		       unsigned long long size, long mtime, char typeflag,
		       const char *linkname)
{
	char hdr[512];
	unsigned long sum = 0;
	size_t i;

	memset(hdr, 0, sizeof hdr);
	memcpy(hdr, path, strlen(path));
	put_octal(hdr + 100, 8, mode & 0777);
	put_octal(hdr + 108, 8, 0);
	put_octal(hdr + 116, 8, 0);
	put_octal(hdr + 124, 12, size);
	put_octal(hdr + 136, 12, mtime < 0 ? 0ULL : (unsigned long long)mtime);
	memset(hdr + 148, ' ', 8);            /* the checksum covers spaces here */
	hdr[156] = typeflag;
	if (linkname)
		memcpy(hdr + 157, linkname, strlen(linkname));
	memcpy(hdr + 257, "ustar", 5);
	hdr[262] = '\0';
	hdr[263] = '0';
	hdr[264] = '0';

	for (i = 0; i < sizeof hdr; i++)
		sum += (unsigned char)hdr[i];
	put_octal(hdr + 148, 7, sum);
	hdr[155] = ' ';

	buf_add(out, hdr, sizeof hdr);
}

static void tar_pad(struct buf *out, size_t len)
{
	static const char zeros[512];
	size_t rem = len % 512;

	if (rem)
		buf_add(out, zeros, 512 - rem);
}

static void tar_one(const char *path, u32 mode, const oid_t *oid, void *ud)
{
	struct ar_ctx *c = ud;
	enum obj_type type;
	struct buf blob;

	buf_init(&blob);
	if (odb_read(&c->r->odb, oid, &type, &blob) < 0) {
		gp_error("archive: cannot read %s", path);
		c->failed = 1;
		buf_release(&blob);
		return;
	}

	if (strlen(path) > 100) {
		gp_error("archive: path is too long for a ustar header: %s", path);
		c->failed = 1;
		buf_release(&blob);
		return;
	}

	if ((mode & 0170000) == 0120000) {
		tar_header(c->out, path, mode, 0, c->mtime, '2',
			   (const char *)blob.b);
	} else {
		tar_header(c->out, path, mode, blob.len, c->mtime, '0', NULL);
		buf_add(c->out, blob.b, blob.len);
		tar_pad(c->out, blob.len);
	}

	buf_release(&blob);
}

/* ------------------------------------------------------------------ */
/* zip                                                                 */

struct zip_entry {
	char *name;
	u32 crc;
	unsigned long csize, usize, offset;
	u32 mode;
};

struct zip_ctx {
	struct repo *r;
	struct buf *out;
	unsigned int dos_time, dos_date;
	struct zip_entry *entries;
	size_t nr, cap;
	int failed;
};

static void put16(struct buf *out, unsigned v)
{
	unsigned char b[2];

	b[0] = (unsigned char)(v & 0xff);
	b[1] = (unsigned char)((v >> 8) & 0xff);
	buf_add(out, b, 2);
}

static void put32(struct buf *out, unsigned long v)
{
	unsigned char b[4];

	b[0] = (unsigned char)(v & 0xff);
	b[1] = (unsigned char)((v >> 8) & 0xff);
	b[2] = (unsigned char)((v >> 16) & 0xff);
	b[3] = (unsigned char)((v >> 24) & 0xff);
	buf_add(out, b, 4);
}

/*
 * MS-DOS packs a timestamp into two 16-bit halves and zip keeps that shape
 * whatever the platform: seconds in twos, the year counted from 1980.
 */
static void dos_time(long epoch, unsigned *out_time, unsigned *out_date)
{
	static const int md[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
	i64 t = epoch > 0 ? epoch : 0;
	long days = (long)(t / 86400);
	unsigned long rem = (unsigned long)(t % 86400);
	int y = 1970, m, d, hh, mm, ss, leap;

	hh = (int)(rem / 3600);
	mm = (int)((rem % 3600) / 60);
	ss = (int)(rem % 60);

	for (;;) {
		leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
		if (days < 365 + leap)
			break;
		days -= 365 + leap;
		y++;
	}
	for (m = 0; ; m++) {
		int len;

		leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
		len = md[m] + (m == 1 && leap ? 1 : 0);
		if (days < len)
			break;
		days -= len;
	}
	d = (int)days + 1;
	m = m + 1;

	if (y < 1980) {
		*out_time = 0;
		*out_date = (unsigned)((1 << 5) | 1);       /* 1980-01-01 */
		return;
	}
	*out_time = (unsigned)((hh << 11) | (mm << 5) | (ss / 2));
	*out_date = (unsigned)(((y - 1980) << 9) | (m << 5) | d);
}

static void zip_one(const char *path, u32 mode, const oid_t *oid, void *ud)
{
	struct zip_ctx *c = ud;
	enum obj_type type;
	struct buf blob;
	struct zip_entry *e;
	size_t len;

	buf_init(&blob);
	if (odb_read(&c->r->odb, oid, &type, &blob) < 0) {
		gp_error("archive: cannot read %s", path);
		c->failed = 1;
		buf_release(&blob);
		return;
	}
	len = blob.len;

	if (c->nr == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 8;
		c->entries = xrealloc(c->entries, c->cap * sizeof *c->entries);
	}
	e = &c->entries[c->nr++];
	memset(e, 0, sizeof *e);
	e->name = xstrdup(path);
	e->mode = mode;
	e->usize = e->csize = (unsigned long)len;
	e->offset = (unsigned long)c->out->len;
	e->crc = (u32)crc32(0L, Z_NULL, 0);
	e->crc = (u32)crc32(e->crc, (const Bytef *)blob.b, (uInt)len);

	put32(c->out, 0x04034b50);
	put16(c->out, 20);
	put16(c->out, 0);
	put16(c->out, 0);                  /* stored */
	put16(c->out, c->dos_time);
	put16(c->out, c->dos_date);
	put32(c->out, e->crc);
	put32(c->out, e->csize);
	put32(c->out, e->usize);
	put16(c->out, (unsigned)strlen(path));
	put16(c->out, 0);
	buf_add(c->out, path, strlen(path));
	buf_add(c->out, blob.b, len);

	buf_release(&blob);
}

static void zip_finish(struct zip_ctx *c)
{
	unsigned long cd_start = (unsigned long)c->out->len;
	unsigned long cd_size;
	size_t i;

	for (i = 0; i < c->nr; i++) {
		struct zip_entry *e = &c->entries[i];

		put32(c->out, 0x02014b50);
		put16(c->out, 20);
		put16(c->out, 20);
		put16(c->out, 0);
		put16(c->out, 0);
		put16(c->out, c->dos_time);
		put16(c->out, c->dos_date);
		put32(c->out, e->crc);
		put32(c->out, e->csize);
		put32(c->out, e->usize);
		put16(c->out, (unsigned)strlen(e->name));
		put16(c->out, 0);
		put16(c->out, 0);
		put16(c->out, 0);
		put16(c->out, 0);
		put32(c->out, ((unsigned long)e->mode & 0xffff) << 16);
		put32(c->out, e->offset);
		buf_add(c->out, e->name, strlen(e->name));
	}

	cd_size = (unsigned long)c->out->len - cd_start;

	put32(c->out, 0x06054b50);
	put16(c->out, 0);
	put16(c->out, 0);
	put16(c->out, (unsigned)c->nr);
	put16(c->out, (unsigned)c->nr);
	put32(c->out, cd_size);
	put32(c->out, cd_start);
	put16(c->out, 0);
}

/* ------------------------------------------------------------------ */

int cmd_archive(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *fmt = "tar", *output, *prefix = "";
	const char *rev, *p;
	oid_t oid, tree_oid;
	enum obj_type type;
	struct buf out;
	long mtime = 0;
	FILE *fp;

	opts_init(&o, argc, argv, (const char *const[]){
		"--format=", "-o=", "--output=", "--prefix=", NULL });

	output = opts_value(&o, "-o") ? opts_value(&o, "-o")
				      : opts_value(&o, "--output");
	p = opts_value(&o, "--format");
	if (p)
		fmt = p;
	p = opts_value(&o, "--prefix");
	if (p)
		prefix = p;

	if (strcmp(fmt, "tar") && strcmp(fmt, "zip"))
		gp_die("archive: unknown format '%s'", fmt);

	rev = opts_arg(&o, 0) ? opts_arg(&o, 0) : "HEAD";
	if (resolve_rev(r, rev, &oid) < 0)
		gp_die("archive: unknown revision: %s", rev);

	/* a commit gives both the tree to walk and the timestamp the entries
	 * carry; a tree on its own has no date, so it gets the epoch */
	{
		oid_t peeled;

		if (commit_peel(r, &oid, OBJ_COMMIT, &peeled) == 0) {
			struct commit c = COMMIT_INIT;

			read_commit(r, &peeled, &c);
			tree_oid = c.tree;
			mtime = (long)commit_time(&c);
			commit_release(&c);
		} else if (odb_type_of(&r->odb, &oid, &type) == 0 &&
			   type == OBJ_TREE) {
			tree_oid = oid;
		} else {
			gp_die("archive: %s is not a tree or a commit", rev);
		}
	}

	buf_init(&out);

	if (!strcmp(fmt, "tar")) {
		struct ar_ctx c;
		static const char zeros[1024];

		c.r = r;
		c.out = &out;
		c.mtime = mtime;
		c.failed = 0;
		if (load_tree_flat(r, &tree_oid, prefix, tar_one, &c) < 0 ||
		    c.failed) {
			buf_release(&out);
			return 1;
		}
		buf_add(&out, zeros, sizeof zeros);
	} else {
		struct zip_ctx c;
		size_t i;

		memset(&c, 0, sizeof c);
		c.r = r;
		c.out = &out;
		dos_time(mtime, &c.dos_time, &c.dos_date);
		if (load_tree_flat(r, &tree_oid, prefix, zip_one, &c) < 0 ||
		    c.failed) {
			for (i = 0; i < c.nr; i++)
				free(c.entries[i].name);
			free(c.entries);
			buf_release(&out);
			return 1;
		}
		zip_finish(&c);
		for (i = 0; i < c.nr; i++)
			free(c.entries[i].name);
		free(c.entries);
	}

	if (output) {
		fp = fopen(output, "wb");
		if (!fp) {
			gp_error("archive: cannot write %s", output);
			buf_release(&out);
			return 1;
		}
		if (fwrite(out.b, 1, out.len, fp) != out.len) {
			gp_error("archive: short write to %s", output);
			fclose(fp);
			buf_release(&out);
			return 1;
		}
		fclose(fp);
	} else {
		fwrite(out.b, 1, out.len, stdout);
	}

	buf_release(&out);
	return 0;
}
