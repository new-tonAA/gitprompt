/*
 * tree.c - turning a flat set of paths into trees, and trees back into
 * files.
 *
 * The index is flat (one entry per file, path with slashes) and a tree is
 * nested, so writing a tree is a recursive grouping by first path
 * component.  Reading one is the same walk in reverse.
 */
#include "gp.h"

#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define gp_chmod(p, m) ((void)(p), (void)(m))
#else
#include <unistd.h>
#include <sys/types.h>
#define gp_chmod(p, m) chmod((p), (m))
#endif

/* ------------------------------------------------------------------ */
/* building a tree from the index                                      */

/*
 * `prefix` is "" at the top or "dir/" below it.  Entries that begin with
 * it are this level's; a remainder with no slash is a file, otherwise the
 * component before the slash is a subdirectory to recurse into.
 */
static int build_tree(struct repo *r, struct index_state *ist,
		      const char *prefix, oid_t *out)
{
	struct tree t = TREE_INIT;
	size_t plen = strlen(prefix);
	size_t i;
	int rc = 0;

	/* subdirectory names seen at this level, de-duplicated by sorting
	 * the entries first and taking each new name once */
	for (i = 0; i < ist->nr; i++) {
		const char *path = ist->e[i].path;
		const char *rest;
		const char *slash;

		/* an unmerged stage is one side of a conflict, not a file; the
		 * caller has already refused to write a tree from one */
		if (ist->e[i].stage)
			continue;
		if (strncmp(path, prefix, plen))
			continue;
		rest = path + plen;
		if (!rest[0])
			continue;
		slash = strchr(rest, '/');
		if (slash) {
			size_t n = (size_t)(slash - rest);
			char *dir = xstrndup(rest, n);
			int seen = 0;
			size_t j;

			/* already handled this directory name? */
			for (j = 0; j < i; j++) {
				const char *op = ist->e[j].path;
				if (strncmp(op, prefix, plen))
					continue;
				if (!strncmp(op + plen, dir, n) && op[plen + n] == '/') {
					seen = 1;
					break;
				}
			}
			if (!seen) {
				char *subprefix = xstrfmt("%s%s/", prefix, dir);
				oid_t sub;
				if (build_tree(r, ist, subprefix, &sub) < 0) {
					free(subprefix);
					free(dir);
					rc = -1;
					break;
				}
				tree_append(&t, MODE_TREE, &sub, dir);
				free(subprefix);
			}
			free(dir);
		} else {
			tree_append(&t, ist->e[i].mode, &ist->e[i].oid, rest);
		}
	}

	if (rc == 0) {
		struct buf b;
		tree_sort_for_write(&t);
		buf_init(&b);
		tree_format(&t, &b);
		rc = odb_write(&r->odb, OBJ_TREE, b.b, b.len, out);
		buf_release(&b);
	}
	tree_release(&t);
	return rc;
}

int write_tree_from_index(struct repo *r, const struct index_state *istate,
			  oid_t *out)
{
	/*
	 * An unmerged path has no one content a tree could hold, so there is
	 * nothing honest to write: git refuses here too.
	 */
	if (index_has_unmerged(istate)) {
		gp_error("cannot write a tree from an index with unmerged paths\n"
			 "hint: resolve them with 'gitprompt add' first");
		return -1;
	}
	/* build_tree only reads the entries, but the helper takes a mutable
	 * pointer; the cast keeps the public signature const-correct */
	return build_tree(r, (struct index_state *)istate, "", out);
}

int hash_worktree_blob(struct repo *r, const char *relpath, oid_t *oid, u32 *mode)
{
	struct buf b;
	char *full = r->root ? xstrfmt("%s/%s", r->root, relpath)
			     : xstrdup(relpath);
	int rc;

	buf_init(&b);
	if (read_file(full, &b) < 0) {
		free(full);
		buf_release(&b);
		return -1;
	}
	*mode = MODE_BLOB;
#ifndef _WIN32
	{
		struct stat st;

		if (stat(full, &st) == 0 && (st.st_mode & 0111))
			*mode = MODE_EXEC;
	}
#endif
	free(full);
	rc = odb_write(&r->odb, OBJ_BLOB, b.b, b.len, oid);
	buf_release(&b);
	return rc;
}

void index_from_worktree(struct repo *r, const struct index_state *idx,
			 struct index_state *out)
{
	size_t i;

	for (i = 0; i < idx->nr; i++) {
		struct index_entry e = idx->e[i];
		u32 mode;

		if (hash_worktree_blob(r, e.path, &e.oid, &mode) < 0)
			continue;
		e.mode = mode;
		index_add(out, &e);
	}
}

/* ------------------------------------------------------------------ */
/* walking a tree                                                      */

int load_tree_flat(struct repo *r, const oid_t *tree_oid, const char *prefix,
		   void (*fn)(const char *path, u32 mode, const oid_t *oid,
			      void *data), void *data)
{
	struct tree t = TREE_INIT;
	size_t i;
	int rc = 0;

	read_tree_obj(r, tree_oid, &t);
	for (i = 0; i < t.nr; i++) {
		char *path = prefix && prefix[0]
			? xstrfmt("%s%s", prefix, t.e[i].name)
			: xstrdup(t.e[i].name);
		if ((t.e[i].mode & 0170000) == 0040000) {
			char *sub = xstrfmt("%s/", path);
			if (load_tree_flat(r, &t.e[i].oid, sub, fn, data) < 0)
				rc = -1;
			free(sub);
		} else {
			fn(path, t.e[i].mode, &t.e[i].oid, data);
		}
		free(path);
		if (rc)
			break;
	}
	tree_release(&t);
	return rc;
}

/* ------------------------------------------------------------------ */
/* index from tree                                                     */

struct rti_ctx {
	struct repo *r;
	struct index_state *ist;
};

static void rti_cb(const char *path, u32 mode, const oid_t *oid, void *ud)
{
	struct rti_ctx *c = ud;
	struct index_entry e;
	char *full = c->r->root ? xstrfmt("%s/%s", c->r->root, path)
				: xstrdup(path);

	memset(&e, 0, sizeof e);
	e.mode = (mode & 0170000) == 0120000 ? MODE_LINK
	       : (mode & 0100) ? MODE_EXEC : MODE_BLOB;
	e.oid = *oid;
	e.path = (char *)path;
	index_fill_stat(&e, full);
	index_add(c->ist, &e);
	free(full);
}

int read_tree_into_index(struct repo *r, struct index_state *istate,
			 const oid_t *tree, const char *prefix)
{
	struct rti_ctx c;
	(void)prefix;
	index_clear(istate);
	c.r = r;
	c.ist = istate;
	return load_tree_flat(r, tree, "", rti_cb, &c);
}

/* ------------------------------------------------------------------ */
/* checking a tree out                                                 */

struct co_ctx {
	struct repo *r;
	int force;
	int wrote;
};

static void co_write_file(struct repo *r, const char *relpath, u32 mode,
			  const oid_t *oid)
{
	char *full = xstrfmt("%s/%s", r->root, relpath);
	char *dir = xstrdup(full);
	char *slash = strrchr(dir, '/');
	struct buf b;

	if (slash) {
		*slash = '\0';
		mkdir_p(dir);
	}
	free(dir);

	if ((mode & 0170000) == 0120000) {
		/* a symlink entry: write the target as the file's content,
		 * which is what the content is anyway */
		enum obj_type t;
		buf_init(&b);
		if (odb_read(&r->odb, oid, &t, &b) < 0) {
			buf_release(&b);
			free(full);
			return;
		}
		write_file(full, b.b, b.len);
		buf_release(&b);
		free(full);
		return;
	}

	buf_init(&b);
	{
		enum obj_type t;
		if (odb_read(&r->odb, oid, &t, &b) < 0) {
			gp_error("cannot read object for %s", relpath);
			buf_release(&b);
			free(full);
			return;
		}
	}
	write_file(full, b.b, b.len);
	buf_release(&b);
	gp_chmod(full, (mode & 0100) ? 0755 : 0644);
	free(full);
}

static void co_cb(const char *path, u32 mode, const oid_t *oid, void *ud)
{
	struct co_ctx *c = ud;
	co_write_file(c->r, path, mode, oid);
	c->wrote++;
}

/*
 * Check out a tree.  Files the old index knew that the new tree does not
 * contain are removed, so switching branches does not leave the previous
 * branch's files behind.  Directories are left in place even when they
 * empty out -- deleting them risks taking untracked files with them.
 */
int checkout_tree(struct repo *r, const oid_t *tree, int force, int update_index)
{
	struct co_ctx c;
	struct index_state old, fresh;
	size_t i;
	int rc;

	if (!r->root) {
		gp_error("cannot check out into a bare repository");
		return -1;
	}
	(void)force;   /* callers decide whether to clobber; see cmd_checkout */

	/* index_read frees what the struct held, so it has to start empty */
	memset(&old, 0, sizeof old);
	index_read(&old, repo_index_path(r));

	c.r = r;
	c.force = force;
	c.wrote = 0;
	rc = load_tree_flat(r, tree, "", co_cb, &c);

	if (update_index) {
		memset(&fresh, 0, sizeof fresh);
		if (read_tree_into_index(r, &fresh, tree, "") == 0) {
			for (i = 0; i < old.nr; i++) {
				if (!index_get(&fresh, old.e[i].path)) {
					char *full = xstrfmt("%s/%s", r->root,
							     old.e[i].path);
					remove_file(full);
					free(full);
				}
			}
			index_release(&old);
			/* the new index takes over the freshly read one; clearing
			 * `fresh` afterwards keeps it from being freed twice, once
			 * as itself and once through `old` */
			old = fresh;
			memset(&fresh, 0, sizeof fresh);
			index_write(&old, repo_index_path(r));
		}
		index_release(&fresh);
	}
	index_release(&old);
	return rc;
}
