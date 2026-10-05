/*
 * index.c - the staging area, in git's binary index format.
 *
 * Header "DIRC", a version, a count, then one fixed-size record per entry
 * (a stat block, the id, flags, the path itself, NUL, padded to a multiple
 * of eight bytes), then the whole thing SHA-1'd.  Big-endian throughout.
 * Version 2 is written unless a path needs the second flags word version 3
 * has room for -- that word is where skip-worktree lives -- and both are read.
 *
 * An entry is a path at a merge stage.  Most paths have one entry, at stage
 * 0; a path in conflict has its base, our version and their version as three
 * entries at stages 1, 2 and 3, which is how git records a conflict and why
 * `git ls-files -u` reads a conflicted gitprompt index the same way it reads
 * git's own.
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
/*
 * Two versions are written, and the choice is not a preference: an entry whose
 * skip-worktree bit is set has to carry a second flags word beside the path,
 * and only version 3 has anywhere to put it.  So a plain index stays version 2
 * and an index with any sparse path in it becomes version 3, which is the rule
 * git follows and the reason both are read here.
 */
#define IDX_VERSION 2
#define IDX_VERSION_EXTENDED 3
#define IDX_ENTRY_FIXED 62       /* everything up to and including flags */

/*
 * In the first flags word the second-highest bit is not a flag of its own but
 * the marker saying a second word follows the path -- which is where
 * skip-worktree lives, since the low bits are taken by the merge stage and the
 * name length.  The two names collide in value and mean different things, so
 * they are kept apart: one is the marker on disk, the other is the bit in
 * `struct index_entry.flags`.
 */
#define IDX_F_EXTENDED 0x4000            /* on disk, in the first word */

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

struct index_entry *index_get_stage(const struct index_state *istate,
				    const char *path, unsigned stage)
{
	size_t i;
	for (i = 0; i < istate->nr; i++)
		if (istate->e[i].stage == stage &&
		    !strcmp(istate->e[i].path, path))
			return (struct index_entry *)&istate->e[i];
	return NULL;
}

struct index_entry *index_get(const struct index_state *istate,
			      const char *path)
{
	struct index_entry *first = NULL;
	size_t i;

	for (i = 0; i < istate->nr; i++) {
		if (strcmp(istate->e[i].path, path))
			continue;
		if (istate->e[i].stage == 0)
			return (struct index_entry *)&istate->e[i];
		if (!first)
			first = (struct index_entry *)&istate->e[i];
	}
	return first;
}

void index_add(struct index_state *istate, const struct index_entry *e)
{
	struct index_entry *slot = NULL;
	size_t i;

	if (e->stage) {
		struct index_entry *old = index_get_stage(istate, e->path,
							  e->stage);
		if (old) {
			free(old->path);
			*old = *e;
			old->path = xstrdup(e->path);
			return;
		}
		index_push(istate, e);
		return;
	}

	/*
	 * A stage-0 entry is the whole story for its path: the unmerged stages
	 * go, which is how `add` records a conflict as resolved.  The new entry
	 * takes the first slot the path occupied and the others are dropped, so
	 * a caller looping over the index does not have the array move under it
	 * the way an append would.
	 */
	for (i = 0; i < istate->nr; i++) {
		if (strcmp(istate->e[i].path, e->path))
			continue;
		if (!slot) {
			slot = &istate->e[i];
			free(slot->path);
			*slot = *e;
			slot->path = xstrdup(e->path);
			continue;
		}
		free(istate->e[i].path);
		memmove(istate->e + i, istate->e + i + 1,
			(istate->nr - i - 1) * sizeof(*istate->e));
		istate->nr--;
		i--;
	}
	if (slot)
		return;
	index_push(istate, e);
}

void index_remove(struct index_state *istate, const char *path)
{
	size_t i = 0;

	while (i < istate->nr) {
		if (strcmp(istate->e[i].path, path)) {
			i++;
			continue;
		}
		free(istate->e[i].path);
		memmove(istate->e + i, istate->e + i + 1,
			(istate->nr - i - 1) * sizeof(*istate->e));
		istate->nr--;
	}
}

int index_has_unmerged(const struct index_state *istate)
{
	size_t i;
	for (i = 0; i < istate->nr; i++)
		if (istate->e[i].stage)
			return 1;
	return 0;
}

static int path_cmp(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

char **index_unmerged_paths(const struct index_state *istate, size_t *nr)
{
	char **v;
	size_t i, n = 0;

	*nr = 0;
	if (!index_has_unmerged(istate))
		return NULL;

	v = xmalloc((istate->nr + 1) * sizeof(*v));
	for (i = 0; i < istate->nr; i++) {
		size_t k;
		int seen = 0;

		if (!istate->e[i].stage)
			continue;
		for (k = 0; k < n; k++)
			if (!strcmp(v[k], istate->e[i].path)) {
				seen = 1;
				break;
			}
		if (!seen)
			v[n++] = istate->e[i].path;   /* borrowed, not copied */
	}
	qsort(v, n, sizeof(*v), path_cmp);
	v[n] = NULL;
	*nr = n;
	return v;
}

void index_paths_free(char **v)
{
	free(v);        /* the strings belong to the index they came from */
}

/* ------------------------------------------------------------------ */
/* read                                                                */

void index_read(struct index_state *istate, const char *path)
{
	struct buf b;
	const u8 *p;
	u32 count, i, version;
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
	version = get_be32(b.b + 4);
	if (version != IDX_VERSION && version != IDX_VERSION_EXTENDED) {
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
		size_t entlen, hdrsz;

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
		/*
		 * Only the assume-valid bit and skip-worktree are carried through.
		 * Bits 12-13 are the merge stage, which has a field of its own from
		 * here on, and the extended bit says a second flags word follows the
		 * path -- which is where skip-worktree lives, so it is read when the
		 * entry claims one and the entry is that much longer for it.
		 */
		e.flags = flags & IDX_FLAG_ASSUME_VALID;
		e.stage = (u16)((flags >> 12) & 3);
		/*
		 * The second flags word sits between the first one and the path --
		 * which is what makes the path start two bytes later, not what makes
		 * the record two bytes longer at the far end.
		 */
		hdrsz = IDX_ENTRY_FIXED;
		if (flags & IDX_F_EXTENDED) {
			u16 ext;

			if ((size_t)(p - b.b) + IDX_ENTRY_FIXED + 2 >
			    b.len - GP_SHA1_RAWSZ)
				break;
			ext = get_be16(p + IDX_ENTRY_FIXED);
			if (ext & IDX_FLAG_SKIP_WORKTREE)
				e.flags |= IDX_FLAG_SKIP_WORKTREE;
			hdrsz += 2;
		}
		entlen = (hdrsz + namelen + 8) & ~(size_t)7;    /* pad to 8 */
		e.path = xstrndup((const char *)p + hdrsz, namelen);
		index_push(istate, &e);
		free(e.path);
		p += entlen;
	}
	buf_release(&b);
}

/* ------------------------------------------------------------------ */
/* write                                                               */

/* path first, then stage: an unmerged path's three entries go out together,
 * lowest stage first, which is the order git insists on */
static int entry_path_cmp(const void *a, const void *b)
{
	const struct index_entry *x = a;
	const struct index_entry *y = b;
	int c = strcmp(x->path, y->path);

	if (c)
		return c;
	return x->stage - y->stage;
}

void index_write(const struct index_state *istate, const char *path)
{
	struct buf b;
	gp_sha1_ctx ctx;
	u8 digest[GP_SHA1_RAWSZ];
	struct index_entry *sorted;
	size_t i;
	char *dir;

	/*
	 * git requires the index to be sorted by path, and refuses to read one
	 * that is not ("unordered stage entries").  Entries are appended as they
	 * are staged, so the order they arrive in is not the order they go out
	 * in; write a sorted copy.  The in-memory entries are left alone --
	 * callers hold pointers into them -- and the paths are borrowed, not
	 * copied, because nothing here outlives the buffer.
	 */
	sorted = xmalloc(istate->nr ? istate->nr * sizeof(*sorted) : 1);
	if (istate->nr)
		memcpy(sorted, istate->e, istate->nr * sizeof(*sorted));
	qsort(sorted, istate->nr, sizeof(*sorted), entry_path_cmp);

	buf_init(&b);
	buf_add(&b, IDX_SIGNATURE, 4);
	for (i = 0; i < istate->nr; i++)
		if (sorted[i].flags & IDX_FLAG_SKIP_WORKTREE)
			break;
	put_be32(&b, i < istate->nr ? IDX_VERSION_EXTENDED : IDX_VERSION);
	put_be32(&b, (u32)istate->nr);

	for (i = 0; i < istate->nr; i++) {
		const struct index_entry *e = &sorted[i];
		size_t start = b.len;
		size_t namelen = strlen(e->path);
		int extended = (e->flags & IDX_FLAG_SKIP_WORKTREE) != 0;
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

		flags = (u16)(e->flags & IDX_FLAG_ASSUME_VALID);
		flags |= (u16)((e->stage & 3) << 12);
		flags |= (u16)(namelen < 0x0fff ? namelen : 0x0fff);
		if (extended)
			flags |= IDX_F_EXTENDED;
		put_be16(&b, flags);
		/* the second flags word, between the first one and the name */
		if (extended)
			put_be16(&b, IDX_FLAG_SKIP_WORKTREE);

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
	free(sorted);

	/*
	 * Which paths are tracked is half of what the ignore rules answer, and
	 * the ignore layer keeps a copy of it.  A command that writes the index
	 * and then walks the work tree in the same run would otherwise be told
	 * about the index as it was before.
	 */
	ignore_forget();
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
