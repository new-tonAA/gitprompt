/*
 * index.c - the staging area, in git's binary index version 2 format.
 *
 * Header "DIRC", a version, a count, then one fixed-size record per path
 * (a stat block, the id, flags, the path itself, NUL, padded to a multiple
 * of eight bytes), then the whole thing SHA-1'd.  Big-endian throughout.
 *
 * gitprompt keeps its index at .gitprompt/index rather than .git/index, so
 * it does not collide with a real git repository sitting in the same tree.
 */
#include "gp.h"

#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define gp_stat _stat64
#define gp_stat_t struct _stat64
#else
#include <unistd.h>
#define gp_stat stat
#define gp_stat_t struct stat
#endif

#define IDX_SIGNATURE "DIRC"
#define IDX_VERSION 2
#define IDX_ENTRY_FIXED 62       /* everything up to and including flags */

static void put_be32(struct buf *b, u32 v)
{
	u8 t[4];
	t[0] = (u8)(v >> 24);
	t[1] = (u8)(v >> 16);
	t[2] = (u8)(v >> 8);
	t[3] = (u8)v;
	buf_add(b, t, 4);
}

static void put_be16(struct buf *b, u16 v)
{
	u8 t[2];
	t[0] = (u8)(v >> 8);
	t[1] = (u8)v;
	buf_add(b, t, 2);
}

static u32 get_be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static u16 get_be16(const u8 *p)
{
	return (u16)(((u16)p[0] << 8) | (u16)p[1]);
}

/* ------------------------------------------------------------------ */

void index_release(struct index_state *istate)
{
	size_t i;
	for (i = 0; i < istate->nr; i++)
		free(istate->e[i].path);
	free(istate->e);
	istate->e = NULL;
	istate->nr = istate->alloc = 0;
}

void index_clear(struct index_state *istate)
{
	index_release(istate);
}

static void index_push(struct index_state *istate,
		       const struct index_entry *e)
{
	if (istate->nr == istate->alloc) {
		istate->alloc = istate->alloc ? istate->alloc * 2 : 32;
		istate->e = xrealloc(istate->e, istate->alloc * sizeof(*istate->e));
	}
	istate->e[istate->nr] = *e;
	istate->e[istate->nr].path = xstrdup(e->path);
	istate->nr++;
}

struct index_entry *index_get(const struct index_state *istate,
			      const char *path)
{
	size_t i;
	for (i = 0; i < istate->nr; i++)
		if (!strcmp(istate->e[i].path, path))
			return (struct index_entry *)&istate->e[i];
	return NULL;
}

void index_add(struct index_state *istate, const struct index_entry *e)
{
	struct index_entry *old = index_get(istate, e->path);
	if (old) {
		free(old->path);
		*old = *e;
		old->path = xstrdup(e->path);
		return;
	}
	index_push(istate, e);
}

void index_remove(struct index_state *istate, const char *path)
{
	size_t i;
	for (i = 0; i < istate->nr; i++) {
		if (!strcmp(istate->e[i].path, path)) {
			free(istate->e[i].path);
			memmove(istate->e + i, istate->e + i + 1,
				(istate->nr - i - 1) * sizeof(*istate->e));
			istate->nr--;
			return;
		}
	}
}

/* ------------------------------------------------------------------ */
/* read                                                                */

void index_read(struct index_state *istate, const char *path)
{
	struct buf b;
	const u8 *p;
	u32 count, i;
	gp_sha1_ctx ctx;
	u8 want[GP_SHA1_RAWSZ];

	index_release(istate);
	buf_init(&b);
	if (read_file(path, &b) < 0) {
		buf_release(&b);
		return;         /* no index yet: an empty staging area */
	}
	if (b.len < 12 + GP_SHA1_RAWSZ ||
	    memcmp(b.b, IDX_SIGNATURE, 4) != 0) {
		gp_error("index file %s is not a gitprompt index", path);
		buf_release(&b);
		return;
	}
	/* the trailing SHA-1 covers everything before it */
	gp_sha1_init(&ctx);
	gp_sha1_update(&ctx, b.b, b.len - GP_SHA1_RAWSZ);
	gp_sha1_final(&ctx, want);
	if (memcmp(want, b.b + b.len - GP_SHA1_RAWSZ, GP_SHA1_RAWSZ) != 0) {
		gp_error("index file %s is corrupt (checksum mismatch)", path);
		buf_release(&b);
		return;
	}
	if (get_be32(b.b + 4) != IDX_VERSION) {
		gp_error("unsupported index version %u in %s",
			 (unsigned)get_be32(b.b + 4), path);
		buf_release(&b);
		return;
	}
	count = get_be32(b.b + 8);
	p = b.b + 12;

	for (i = 0; i < count; i++) {
		struct index_entry e;
		u16 flags, namelen;
		size_t entlen;

		if ((size_t)(p - b.b) + IDX_ENTRY_FIXED > b.len - GP_SHA1_RAWSZ)
			break;
		memset(&e, 0, sizeof e);
		e.ctime_sec = get_be32(p + 0);
		e.ctime_nsec = get_be32(p + 4);
		e.mtime_sec = get_be32(p + 8);
		e.mtime_nsec = get_be32(p + 12);
		e.dev = get_be32(p + 16);
		e.ino = get_be32(p + 20);
		e.mode = get_be32(p + 24);
		e.uid = get_be32(p + 28);
		e.gid = get_be32(p + 32);
		e.size = get_be32(p + 36);
		memcpy(e.oid.raw, p + 40, GP_SHA1_RAWSZ);
		flags = get_be16(p + 60);
		namelen = (u16)(flags & 0x0fff);
		e.flags = flags & (u16)~0x0fff;
		entlen = IDX_ENTRY_FIXED + namelen;
		entlen = (entlen + 8) & ~(size_t)7;    /* pad to 8 */
		e.path = xstrndup((const char *)p + IDX_ENTRY_FIXED, namelen);
		/* the file may hold a stage > 0 from git; the low bits carry
		 * the stage, and anything but stage 0 is not ours to use */
		e.flags |= (u16)(flags & 0x3000);
		index_push(istate, &e);
		free(e.path);
		p += entlen;
	}
	buf_release(&b);
}

/* ------------------------------------------------------------------ */
/* write                                                               */

void index_write(const struct index_state *istate, const char *path)
{
	struct buf b;
	gp_sha1_ctx ctx;
	u8 digest[GP_SHA1_RAWSZ];
	size_t i;
	char *dir;

	buf_init(&b);
	buf_add(&b, IDX_SIGNATURE, 4);
	put_be32(&b, IDX_VERSION);
	put_be32(&b, (u32)istate->nr);

	for (i = 0; i < istate->nr; i++) {
		const struct index_entry *e = &istate->e[i];
		size_t start = b.len;
		size_t namelen = strlen(e->path);
		u16 flags;

		put_be32(&b, e->ctime_sec);
		put_be32(&b, e->ctime_nsec);
		put_be32(&b, e->mtime_sec);
		put_be32(&b, e->mtime_nsec);
		put_be32(&b, e->dev);
		put_be32(&b, e->ino);
		put_be32(&b, e->mode);
		put_be32(&b, e->uid);
		put_be32(&b, e->gid);
		put_be32(&b, e->size);
		buf_add(&b, e->oid.raw, GP_SHA1_RAWSZ);

		flags = (u16)(e->flags & (u16)~0x0fff);
		flags |= (u16)(namelen < 0x0fff ? namelen : 0x0fff);
		put_be16(&b, flags);

		buf_add(&b, e->path, namelen);
		buf_addch(&b, '\0');
		while ((b.len - start) % 8)
			buf_addch(&b, '\0');
	}

	gp_sha1_init(&ctx);
	gp_sha1_update(&ctx, b.b, b.len);
	gp_sha1_final(&ctx, digest);
	buf_add(&b, digest, GP_SHA1_RAWSZ);

	dir = xstrdup(path);
	{
		char *slash = strrchr(dir, '/');
		if (slash) {
			*slash = '\0';
			mkdir_p(dir);
		}
	}
	free(dir);
	if (write_file(path, b.b, b.len) < 0)
		gp_error("cannot write index %s", path);
	buf_release(&b);
}

/* ------------------------------------------------------------------ */

/*
 * Fill in the stat fields for a path, so a later run can tell whether the
 * file changed without hashing it.  A missing file leaves the fields at
 * zero, which never matches a real stat and so always forces a re-hash.
 */
void index_fill_stat(struct index_entry *e, const char *fullpath)
{
	gp_stat_t st;
	memset(&st, 0, sizeof st);
	if (gp_stat(fullpath, &st) != 0)
		return;
	e->ctime_sec = (u32)st.st_ctime;
	e->mtime_sec = (u32)st.st_mtime;
	e->mtime_nsec = 0;
	e->dev = (u32)st.st_dev;
	e->ino = (u32)st.st_ino;
	e->uid = (u32)st.st_uid;
	e->gid = (u32)st.st_gid;
	e->size = (u32)st.st_size;
}
