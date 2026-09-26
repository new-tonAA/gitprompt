/*
 * odb.c - the loose object store.
 *
 * An object is "<type> <length>\0<payload>"; its id is the SHA-1 of that
 * whole byte string, and it is stored zlib-deflated at objects/<id[0:2]>/<rest>.
 * Identical to git, so `git cat-file` reads a gitprompt store, and vice
 * versa for every object type git knows.
 */
#include "gp.h"

#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <process.h>
#define gp_getpid() ((long)_getpid())
#else
#include <unistd.h>
#define gp_getpid() ((long)getpid())
#endif

#include <zlib.h>

void odb_init(struct odb *o, const char *dir)
{
	o->dir = xstrdup(dir);
	buf_init(&o->tmp);
}

void odb_release(struct odb *o)
{
	free(o->dir);
	o->dir = NULL;
	buf_release(&o->tmp);
}

static char *odb_path(struct odb *o, const oid_t *oid)
{
	char hex[GP_SHA1_HEXSZ + 1];
	oid_hex(oid, hex);
	return xstrfmt("%s/%c%c/%s", o->dir, hex[0], hex[1], hex + 2);
}

int odb_exists(struct odb *o, const oid_t *oid)
{
	char *p = odb_path(o, oid);
	int rc = is_file(p);
	free(p);
	return rc;
}

static char lower_hex(int c)
{
	if (c >= 'A' && c <= 'F')
		return (char)(c - 'A' + 'a');
	return (char)c;
}

/* ------------------------------------------------------------------ */
/* inflate / deflate                                                   */

static int inflate_all(const u8 *in, size_t inlen, struct buf *out)
{
	z_stream zs;
	u8 chunk[65536];
	int rc;

	memset(&zs, 0, sizeof zs);
	if (inflateInit(&zs) != Z_OK)
		return -1;
	zs.next_in = (Bytef *)in;
	zs.avail_in = (uInt)inlen;

	for (;;) {
		zs.next_out = chunk;
		zs.avail_out = sizeof chunk;
		rc = inflate(&zs, Z_NO_FLUSH);
		if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
			inflateEnd(&zs);
			return -1;
		}
		buf_add(out, chunk, sizeof chunk - zs.avail_out);
		if (rc == Z_STREAM_END)
			break;
		if (zs.avail_in == 0 && zs.avail_out != 0) {
			inflateEnd(&zs);       /* truncated stream */
			return -1;
		}
	}
	inflateEnd(&zs);
	return 0;
}

static int deflate_all(const u8 *in, size_t inlen, struct buf *out)
{
	uLongf bound = compressBound((uLong)inlen);
	u8 *dst = xmalloc(bound ? bound : 1);
	int rc = compress2(dst, &bound, (const Bytef *)in, (uLong)inlen,
			   Z_DEFAULT_COMPRESSION);
	if (rc != Z_OK) {
		free(dst);
		return -1;
	}
	buf_reset(out);
	buf_add(out, dst, bound);
	free(dst);
	return 0;
}

/* ------------------------------------------------------------------ */
/* read                                                                */

/*
 * Split the inflated header from the payload.  The header is
 * "<type> <length>\0" and the declared length must match the payload
 * exactly; anything else is a corrupt object.
 */
static int parse_header(struct buf *raw, enum obj_type *type, size_t *hdrlen,
			size_t *paylen)
{
	u8 *nul = memchr(raw->b, '\0', raw->len);
	u8 *sp;
	char typename[32];
	size_t tlen;
	char *end;
	long declared;

	if (!nul)
		return -1;
	sp = memchr(raw->b, ' ', (size_t)(nul - raw->b));
	if (!sp)
		return -1;
	tlen = (size_t)(sp - raw->b);
	if (tlen == 0 || tlen >= sizeof typename)
		return -1;
	memcpy(typename, raw->b, tlen);
	typename[tlen] = '\0';
	*type = obj_type_from_name(typename);
	if (*type == OBJ_NONE)
		return -1;

	errno = 0;
	declared = strtol((char *)sp + 1, &end, 10);
	if (errno || declared < 0)
		return -1;
	if (end != (char *)nul)
		return -1;
	*hdrlen = (size_t)(nul - raw->b) + 1;
	*paylen = (size_t)declared;
	return 0;
}

int odb_read(struct odb *o, const oid_t *oid, enum obj_type *type,
	     struct buf *out)
{
	char *path = odb_path(o, oid);
	struct buf raw, disk;
	size_t hdrlen, paylen;
	enum obj_type t;
	int rc = -1;

	buf_init(&raw);
	buf_init(&disk);
	if (read_file(path, &disk) < 0)
		goto done;
	if (inflate_all(disk.b, disk.len, &raw) < 0)
		goto done;
	if (parse_header(&raw, &t, &hdrlen, &paylen) < 0)
		goto done;
	if (raw.len - hdrlen != paylen)
		goto done;
	if (type)
		*type = t;
	if (out) {
		buf_reset(out);
		buf_add(out, raw.b + hdrlen, paylen);
	}
	rc = 0;
done:
	free(path);
	buf_release(&raw);
	buf_release(&disk);
	return rc;
}

int odb_read_raw(struct odb *o, const oid_t *oid, enum obj_type *type,
		 struct buf *out)
{
	return odb_read(o, oid, type, out);
}

int odb_type_of(struct odb *o, const oid_t *oid, enum obj_type *type)
{
	return odb_read(o, oid, type, NULL);
}

/* ------------------------------------------------------------------ */
/* write                                                               */

/*
 * The id of an object does not depend on where the object would be stored, so
 * `o` may be NULL when write_it is 0: `gitprompt hash-object file` outside a
 * repository is exactly that call.  Everything that reads the store comes
 * after the write_it test.
 */
int odb_hash(struct odb *o, enum obj_type type, const void *data, size_t len,
	     oid_t *out, int write_it)
{
	struct buf whole, z;
	gp_sha1_ctx ctx;
	char *path, *tmp, hex[GP_SHA1_HEXSZ + 1];
	int rc = 0;

	buf_init(&whole);
	buf_init(&z);

	buf_addf(&whole, "%s %lu", obj_type_name(type), (unsigned long)len);
	buf_addch(&whole, '\0');
	buf_add(&whole, data, len);

	gp_sha1_init(&ctx);
	gp_sha1_update(&ctx, whole.b, whole.len);
	gp_sha1_final(&ctx, out->raw);

	if (!write_it)
		goto done;

	path = odb_path(o, out);
	if (is_file(path))      /* objects are immutable */
		goto done;

	oid_hex(out, hex);
	{
		char *dir = xstrfmt("%s/%c%c", o->dir, hex[0], hex[1]);
		if (mkdir_p(dir) < 0) {
			gp_error("cannot create object directory %s", dir);
			free(dir);
			free(path);
			rc = -1;
			goto done;
		}
		free(dir);
	}

	if (deflate_all(whole.b, whole.len, &z) < 0) {
		gp_error("cannot compress object");
		free(path);
		rc = -1;
		goto done;
	}
	/*
	 * Write to a temporary name and rename, so a reader never sees a
	 * half-written object under its real id.
	 */
	tmp = xstrfmt("%s.t%ld", path, gp_getpid());
	if (write_file(tmp, z.b, z.len) < 0 || rename(tmp, path) != 0) {
		gp_error("cannot write object %s", path);
		remove_file(tmp);
		free(tmp);
		free(path);
		rc = -1;
		goto done;
	}
	free(tmp);
	free(path);
done:
	buf_release(&whole);
	buf_release(&z);
	return rc;
}

int odb_write(struct odb *o, enum obj_type type, const void *data, size_t len,
	      oid_t *out)
{
	return odb_hash(o, type, data, len, out, 1);
}

/* ------------------------------------------------------------------ */
/* enumeration                                                         */

size_t odb_count(struct odb *o)
{
	DIR *d = opendir(o->dir);
	struct dirent *de;
	size_t n = 0;

	if (!d)
		return 0;
	while ((de = readdir(d))) {
		DIR *sub;
		struct dirent *sde;
		char *p;

		if (strlen(de->d_name) != 2)
			continue;
		p = xstrfmt("%s/%s", o->dir, de->d_name);
		sub = opendir(p);
		free(p);
		if (!sub)
			continue;
		while ((sde = readdir(sub)))
			if (strlen(sde->d_name) == 38)
				n++;
		closedir(sub);
	}
	closedir(d);
	return n;
}

void odb_foreach_loose(struct odb *o, int (*fn)(const oid_t *, void *),
		       void *data)
{
	DIR *d = opendir(o->dir);
	struct dirent *de;

	if (!d)
		return;
	while ((de = readdir(d))) {
		DIR *sub;
		struct dirent *sde;
		char *p;

		if (strlen(de->d_name) != 2)
			continue;
		p = xstrfmt("%s/%s", o->dir, de->d_name);
		sub = opendir(p);
		free(p);
		if (!sub)
			continue;
		while ((sde = readdir(sub))) {
			char hex[GP_SHA1_HEXSZ + 1];
			oid_t oid;
			if (strlen(sde->d_name) != 38)
				continue;
			snprintf(hex, sizeof hex, "%s%s", de->d_name, sde->d_name);
			if (oid_parse(&oid, hex) < 0)
				continue;
			if (fn(&oid, data)) {
				closedir(sub);
				closedir(d);
				return;
			}
		}
		closedir(sub);
	}
	closedir(d);
}

/* ------------------------------------------------------------------ */
/* prefix lookup                                                       */

struct prefix_ctx {
	const char *hex;
	size_t n;
	oid_t found;
	int nr_found;
};

static void prefix_scan_dir(struct odb *o, const char *dirname,
			    struct prefix_ctx *ctx)
{
	char *path = xstrfmt("%s/%s", o->dir, dirname);
	DIR *d = opendir(path);
	struct dirent *de;

	free(path);
	if (!d)
		return;
	while ((de = readdir(d))) {
		char full[GP_SHA1_HEXSZ + 1];
		size_t i;
		int match = 1;

		if (strlen(de->d_name) != 38)
			continue;
		snprintf(full, sizeof full, "%s%s", dirname, de->d_name);
		for (i = 0; i < ctx->n; i++) {
			if (lower_hex(full[i]) != lower_hex(ctx->hex[i])) {
				match = 0;
				break;
			}
		}
		if (!match)
			continue;
		if (oid_parse(&ctx->found, full) < 0)
			continue;
		if (++ctx->nr_found > 1)
			break;
	}
	closedir(d);
}

int odb_resolve_prefix(struct odb *o, const char *hex, oid_t *out)
{
	struct prefix_ctx ctx;
	size_t i, n = strlen(hex);

	if (n < 4 || n > GP_SHA1_HEXSZ)
		return -1;
	for (i = 0; i < n; i++)
		if (!strchr("0123456789abcdefABCDEF", hex[i]))
			return -1;

	ctx.hex = hex;
	ctx.n = n;
	ctx.nr_found = 0;

	if (n >= 2) {
		char dirname[3];
		dirname[0] = lower_hex(hex[0]);
		dirname[1] = lower_hex(hex[1]);
		dirname[2] = '\0';
		prefix_scan_dir(o, dirname, &ctx);
	} else {
		DIR *d = opendir(o->dir);
		struct dirent *de;
		if (!d)
			return -1;
		while ((de = readdir(d))) {
			if (strlen(de->d_name) != 2)
				continue;
			prefix_scan_dir(o, de->d_name, &ctx);
			if (ctx.nr_found > 1)
				break;
		}
		closedir(d);
	}

	if (ctx.nr_found == 0)
		return -1;
	if (ctx.nr_found > 1)
		return -2;              /* ambiguous */
	*out = ctx.found;
	return 0;
}
