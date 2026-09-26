/*
 * oid.c - SHA-1, object ids, and the object type table.
 *
 * The hash is SHA-1 over "<type> <length>\0<payload>", exactly git's, so
 * an object written by gitprompt has the same id git would give it.
 */
#include "gp.h"

const oid_t null_oid = { { 0 } };

int oid_is_null(const oid_t *o)
{
	return oid_equal(o, &null_oid);
}

int oid_equal(const oid_t *a, const oid_t *b)
{
	return !memcmp(a->raw, b->raw, GP_SHA1_RAWSZ);
}

int oid_compare(const oid_t *a, const oid_t *b)
{
	return memcmp(a->raw, b->raw, GP_SHA1_RAWSZ);
}

static const char hexchars[] = "0123456789abcdef";

void oid_hex(const oid_t *o, char out[GP_SHA1_HEXSZ + 1])
{
	int i;
	for (i = 0; i < GP_SHA1_RAWSZ; i++) {
		out[i * 2] = hexchars[o->raw[i] >> 4];
		out[i * 2 + 1] = hexchars[o->raw[i] & 0xf];
	}
	out[GP_SHA1_HEXSZ] = '\0';
}

/* the short form git prints in log and status output */
char *abbrev_oid(const oid_t *oid)
{
	char hex[GP_SHA1_HEXSZ + 1];
	oid_hex(oid, hex);
	hex[7] = '\0';
	return xstrdup(hex);
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int oid_parse(oid_t *o, const char *hex)
{
	int i;
	if (strlen(hex) != GP_SHA1_HEXSZ)
		return -1;
	for (i = 0; i < GP_SHA1_RAWSZ; i++) {
		int hi = hexval(hex[i * 2]), lo = hexval(hex[i * 2 + 1]);
		if (hi < 0 || lo < 0)
			return -1;
		o->raw[i] = (u8)((hi << 4) | lo);
	}
	return 0;
}

/*
 * Parse an abbreviated id.  Returns the number of hex digits consumed and
 * zero-fills the rest, so the caller can hand the result to a prefix
 * search.  Returns -1 on a malformed or too-short string.
 */
int oid_parse_prefix(oid_t *o, const char *hex)
{
	size_t n = strlen(hex), i;
	if (n < 4 || n > GP_SHA1_HEXSZ)
		return -1;
	memset(o->raw, 0, sizeof o->raw);
	for (i = 0; i < (n + 1) / 2; i++) {
		int hi = hexval(hex[i * 2]);
		int lo = (i * 2 + 1 < n) ? hexval(hex[i * 2 + 1]) : 0;
		if (hi < 0 || lo < 0)
			return -1;
		o->raw[i] = (u8)((hi << 4) | lo);
	}
	return (int)n;
}

void oid_array_append(struct oid_array *a, const oid_t *o)
{
	if (a->nr == a->alloc) {
		a->alloc = a->alloc ? a->alloc * 2 : 8;
		a->oid = xrealloc(a->oid, a->alloc * sizeof(*a->oid));
	}
	a->oid[a->nr++] = *o;
}

int oid_array_contains(const struct oid_array *a, const oid_t *o)
{
	size_t i;
	for (i = 0; i < a->nr; i++)
		if (oid_equal(&a->oid[i], o))
			return 1;
	return 0;
}

void oid_array_clear(struct oid_array *a)
{
	free(a->oid);
	a->oid = NULL;
	a->nr = a->alloc = 0;
}

static int oid_qcmp(const void *a, const void *b)
{
	return oid_compare((const oid_t *)a, (const oid_t *)b);
}

void oid_array_sort(struct oid_array *a)
{
	if (a->nr > 1)
		qsort(a->oid, a->nr, sizeof(*a->oid), oid_qcmp);
}

/* ------------------------------------------------------------------ */
/* object types                                                        */

static const struct {
	enum obj_type t;
	const char *name;
} type_table[] = {
	{ OBJ_COMMIT, "commit" },
	{ OBJ_TREE,   "tree"   },
	{ OBJ_BLOB,   "blob"   },
	{ OBJ_TAG,    "tag"    },
};

enum obj_type obj_type_from_name(const char *name)
{
	size_t i;
	for (i = 0; i < sizeof type_table / sizeof type_table[0]; i++)
		if (!strcmp(type_table[i].name, name))
			return type_table[i].t;
	return OBJ_NONE;
}

const char *obj_type_name(enum obj_type t)
{
	size_t i;
	for (i = 0; i < sizeof type_table / sizeof type_table[0]; i++)
		if (type_table[i].t == t)
			return type_table[i].name;
	return "unknown";
}

int obj_type_valid(enum obj_type t)
{
	return t == OBJ_COMMIT || t == OBJ_TREE || t == OBJ_BLOB || t == OBJ_TAG;
}

/* ------------------------------------------------------------------ */
/* SHA-1                                                               */

#define ROL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void sha1_transform(u32 state[5], const u8 block[64])
{
	u32 w[80];
	u32 a, b, c, d, e;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = ((u32)block[i * 4] << 24) | ((u32)block[i * 4 + 1] << 16) |
		       ((u32)block[i * 4 + 2] << 8) | (u32)block[i * 4 + 3];
	for (i = 16; i < 80; i++)
		w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

	a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

	for (i = 0; i < 80; i++) {
		u32 f, k, tmp;
		if (i < 20) {
			f = (b & c) | ((~b) & d);
			k = 0x5A827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDC;
		} else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6;
		}
		tmp = ROL(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = ROL(b, 30);
		b = a;
		a = tmp;
	}

	state[0] += a; state[1] += b; state[2] += c;
	state[3] += d; state[4] += e;
}

void gp_sha1_init(gp_sha1_ctx *ctx)
{
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xEFCDAB89;
	ctx->state[2] = 0x98BADCFE;
	ctx->state[3] = 0x10325476;
	ctx->state[4] = 0xC3D2E1F0;
	ctx->count = 0;
}

void gp_sha1_update(gp_sha1_ctx *ctx, const void *data, size_t len)
{
	const u8 *p = data;
	size_t have = (size_t)(ctx->count & 63);

	ctx->count += len;

	if (have) {
		size_t want = 64 - have;
		if (len < want) {
			memcpy(ctx->buffer + have, p, len);
			return;
		}
		memcpy(ctx->buffer + have, p, want);
		sha1_transform(ctx->state, ctx->buffer);
		p += want;
		len -= want;
	}
	while (len >= 64) {
		sha1_transform(ctx->state, p);
		p += 64;
		len -= 64;
	}
	if (len)
		memcpy(ctx->buffer, p, len);
}

void gp_sha1_final(gp_sha1_ctx *ctx, u8 out[GP_SHA1_RAWSZ])
{
	u64 bits = ctx->count * 8;
	size_t have = (size_t)(ctx->count & 63);
	u8 pad[72];
	size_t padlen;
	int i;

	/*
	 * pad is sized for the worst case: 0x80, then enough zeros to reach
	 * the next 56-byte boundary, then the 8-byte length -- 64 + 8 when
	 * `have` is 56 or more.  The zeros must be written explicitly; this
	 * is stack memory.
	 */
	memset(pad, 0, sizeof pad);
	pad[0] = 0x80;
	padlen = (have < 56) ? (56 - have) : (120 - have);

	for (i = 0; i < 8; i++)
		pad[padlen + i] = (u8)(bits >> (56 - i * 8));
	gp_sha1_update(ctx, pad, padlen + 8);

	for (i = 0; i < 5; i++) {
		out[i * 4] = (u8)(ctx->state[i] >> 24);
		out[i * 4 + 1] = (u8)(ctx->state[i] >> 16);
		out[i * 4 + 2] = (u8)(ctx->state[i] >> 8);
		out[i * 4 + 3] = (u8)(ctx->state[i]);
	}
	memset(ctx, 0, sizeof *ctx);
}

void gp_sha1(const void *data, size_t len, u8 out[GP_SHA1_RAWSZ])
{
	gp_sha1_ctx ctx;
	gp_sha1_init(&ctx);
	gp_sha1_update(&ctx, data, len);
	gp_sha1_final(&ctx, out);
}
