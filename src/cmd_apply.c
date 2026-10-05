/*
 * apply -- read a patch and make the files it names match it.
 *
 * The patch is git's unified diff, which is what `gitprompt diff` prints and
 * what `git diff` prints, so a patch made by either tool is applied by the
 * other.  A hunk is matched by its exact preimage: it applies where its
 * context and removed lines are, byte for byte, and a hunk that is found
 * nowhere is a failure rather than something to guess around.  There is no
 * fuzz -- a context line that has drifted is a reason to stop, not a reason
 * to try harder.
 *
 * Nothing is written until every hunk of every file has matched, so a patch
 * that fails half way leaves the work tree and the index exactly as they
 * were; the error names the file and the line the way git's does, and the
 * exit status is 1.
 *
 * The layers are git's: the work tree alone by default, the index alone with
 * --cached, both with --index, and --check validates without writing.  A path
 * that would leave the work tree is refused -- and one named under
 * --unsafe-paths is normalised against the work tree root rather than
 * actually written outside it, which is a deliberate difference from git.
 */
#include "gp.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define ap_chmod(p, m) ((void)(p), (void)(m))
#else
#include <unistd.h>
#define ap_chmod(p, m) chmod((p), (m))
#endif

/* ------------------------------------------------------------------ */
/* the patch, as lines                                                 */

/*
 * One line, with the newline kept out of the count and named by `nl`, since a
 * last line without one is a different line from the same text with one.  A
 * line read out of the patch keeps its prefix in `op`; a line read out of a
 * file carries a space there and is otherwise the same.
 */
struct ap_line {
	char op;              /* ' ', '-' or '+' */
	const char *p;
	size_t len;
	int nl;
};

struct ap_hunk {
	long a_start, a_len, b_start, b_len;
	struct ap_line *l;
	size_t nr, alloc;
};

struct ap_file {
	char *old_path;       /* NULL when the old side is /dev/null */
	char *new_path;       /* NULL when the new side is /dev/null */
	u32 old_mode, new_mode;
	int old_absent, new_absent;
	struct ap_hunk *h;
	size_t nr, alloc;
};

struct ap_patch {
	struct ap_file *f;
	size_t nr, alloc;
};

/* a raw line of the patch, before any prefix is taken off it */
struct praw {
	const char *p;
	size_t len;
	int nl;
};

static struct praw *ap_split(const char *buf, size_t len, size_t *nr)
{
	struct praw *v = NULL;
	size_t n = 0, alloc = 0;
	const char *p = buf, *end = buf + len;

	while (p < end) {
		const char *nlp = memchr(p, '\n', (size_t)(end - p));
		size_t l = nlp ? (size_t)(nlp - p) : (size_t)(end - p);

		if (n == alloc) {
			alloc = alloc ? alloc * 2 : 64;
			v = xrealloc(v, alloc * sizeof *v);
		}
		v[n].p = p;
		v[n].len = l;
		v[n].nl = nlp ? 1 : 0;
		n++;
		p = nlp ? nlp + 1 : end;
	}
	*nr = n;
	return v;
}

static int ap_has(const char *line, size_t len, const char *pfx)
{
	size_t n = strlen(pfx);

	return len >= n && !memcmp(line, pfx, n);
}

/* the bytes after `pfx`, as a fresh string; NULL when the line is just that */
static char *ap_after(const char *line, size_t len, const char *pfx)
{
	size_t n = strlen(pfx);

	return xstrndup(line + n, len - n);
}

static void ap_hunk_push(struct ap_hunk *h, struct ap_line l)
{
	if (h->nr == h->alloc) {
		h->alloc = h->alloc ? h->alloc * 2 : 16;
		h->l = xrealloc(h->l, h->alloc * sizeof *h->l);
	}
	h->l[h->nr++] = l;
}

static struct ap_file *ap_file_new(struct ap_patch *p)
{
	if (p->nr == p->alloc) {
		p->alloc = p->alloc ? p->alloc * 2 : 8;
		p->f = xrealloc(p->f, p->alloc * sizeof *p->f);
	}
	memset(&p->f[p->nr], 0, sizeof p->f[p->nr]);
	return &p->f[p->nr++];
}

static struct ap_hunk *ap_hunk_new(struct ap_file *f)
{
	if (f->nr == f->alloc) {
		f->alloc = f->alloc ? f->alloc * 2 : 4;
		f->h = xrealloc(f->h, f->alloc * sizeof *f->h);
	}
	memset(&f->h[f->nr], 0, sizeof f->h[f->nr]);
	return &f->h[f->nr++];
}

/* one number of a range; 1 when there was one, 0 when there was not */
static int ap_num(const char **p, const char *end, long *v)
{
	const char *q = *p;
	long n = 0;

	if (q >= end || !isdigit((unsigned char)*q))
		return 0;
	while (q < end && isdigit((unsigned char)*q)) {
		n = n * 10 + (*q - '0');
		q++;
	}
	*p = q;
	*v = n;
	return 1;
}

/* `@@ -a[,b] +c[,d] @@` -- the two ranges the hunks below are between */
static int ap_hunk_header(const char *line, size_t len, struct ap_hunk *h)
{
	const char *p = line, *end = line + len;

	if (len < 4 || memcmp(p, "@@ -", 4))
		return -1;
	p += 4;
	if (!ap_num(&p, end, &h->a_start))
		return -1;
	h->a_len = 1;
	if (p < end && *p == ',') {
		p++;
		if (!ap_num(&p, end, &h->a_len))
			return -1;
	}
	if (p + 2 > end || p[0] != ' ' || p[1] != '+')
		return -1;
	p += 2;
	if (!ap_num(&p, end, &h->b_start))
		return -1;
	h->b_len = 1;
	if (p < end && *p == ',') {
		p++;
		if (!ap_num(&p, end, &h->b_len))
			return -1;
	}
	if (p + 3 > end || memcmp(p, " @@", 3))
		return -1;
	return 0;
}

/* `diff --git a/old b/new`, split on the " b/" that separates them */
static int ap_diff_paths(const char *line, size_t len, struct ap_file *f)
{
	const char *rest = line + 11, *end = line + len;
	const char *sep = NULL, *q;

	if (len < 13 || rest[0] != 'a' || rest[1] != '/') {
		gp_error("apply: cannot read a diff header: %.*s", (int)len, line);
		return -1;
	}
	for (q = rest + 2; q + 3 <= end; q++)
		if (q[0] == ' ' && q[1] == 'b' && q[2] == '/') {
			sep = q;
			break;
		}
	if (!sep) {
		gp_error("apply: cannot read a diff header: %.*s", (int)len, line);
		return -1;
	}
	free(f->old_path);
	free(f->new_path);
	f->old_path = xstrndup(rest + 2, (size_t)(sep - (rest + 2)));
	f->new_path = xstrndup(sep + 3, (size_t)(end - (sep + 3)));
	if (!*f->old_path || !*f->new_path) {
		gp_error("apply: cannot read a diff header: %.*s", (int)len, line);
		return -1;
	}
	return 0;
}

/* the path on a `--- `/`+++ ` line, or NULL for /dev/null */
static char *ap_side_path(const char *line, size_t len, const char *pfx)
{
	char *s = ap_after(line, len, pfx);
	char *out;

	if (!strcmp(s, "/dev/null")) {
		free(s);
		return NULL;
	}
	/* a quoted path is one with something in it this cannot read */
	if (s[0] == '"') {
		gp_error("apply: cannot read the quoted path %s", s);
		free(s);
		return (char *)-1;
	}
	if (s[0] && s[1] == '/')
		out = xstrdup(s + 2);
	else
		out = xstrdup(s);
	free(s);
	return out;
}

/* ------------------------------------------------------------------ */
/* reading the patch                                                   */

static int ap_parse(const char *buf, size_t len, struct ap_patch *out)
{
	struct praw *ls;
	size_t n, i;
	struct ap_file *cur = NULL;
	int rc = 0;

	memset(out, 0, sizeof *out);
	ls = ap_split(buf, len, &n);

	for (i = 0; i < n && rc == 0; i++) {
		const char *line = ls[i].p;
		size_t ll = ls[i].len;

		if (ap_has(line, ll, "diff --git ")) {
			cur = ap_file_new(out);
			if (ap_diff_paths(line, ll, cur) < 0)
				rc = -1;
			continue;
		}
		if (ap_has(line, ll, "diff --cc ") ||
		    ap_has(line, ll, "diff --combined ")) {
			gp_error("apply: combined diffs are not supported");
			rc = -1;
			continue;
		}
		if (!cur) {
			if (!ll)
				continue;
			gp_error("apply: unrecognized input: %.*s", (int)ll,
				 line);
			rc = -1;
			continue;
		}

		if (ap_has(line, ll, "@@ ")) {
			struct ap_hunk *h = ap_hunk_new(cur);

			if (ap_hunk_header(line, ll, h) < 0) {
				gp_error("apply: malformed hunk header: %.*s",
					 (int)ll, line);
				rc = -1;
				continue;
			}
			while (i + 1 < n) {
				const char *b = ls[i + 1].p;
				size_t bl = ls[i + 1].len;
				struct ap_line l;

				if (!bl) {
					/* a context line emptied of its
					 * trailing space by something on the
					 * way here */
					l.op = ' ';
					l.p = b;
					l.len = 0;
					l.nl = 1;
				} else if (b[0] == ' ' || b[0] == '-' ||
					   b[0] == '+') {
					l.op = b[0];
					l.p = b + 1;
					l.len = bl - 1;
					l.nl = 1;
				} else if (b[0] == '\\') {
					/* the marker for a last line with no
					 * newline; it belongs to the line
					 * above it */
					if (!ap_has(b, bl,
						    "\\ No newline")) {
						gp_error("apply: cannot read "
							 "the line: %.*s",
							 (int)bl, b);
						rc = -1;
						break;
					}
					if (h->nr)
						h->l[h->nr - 1].nl = 0;
					i++;
					continue;
				} else {
					break;   /* the hunk is over */
				}
				ap_hunk_push(h, l);
				i++;
			}
			continue;
		}
		if (ap_has(line, ll, "@@@")) {
			gp_error("apply: combined diffs are not supported");
			rc = -1;
			continue;
		}
		if (ap_has(line, ll, "|||||||")) {
			gp_error("apply: diff3 conflicts are not supported");
			rc = -1;
			continue;
		}
		if (ap_has(line, ll, "new file mode ")) {
			char *v = ap_after(line, ll, "new file mode ");

			cur->new_mode = (u32)strtoul(v, NULL, 8);
			cur->old_absent = 1;
			free(v);
			continue;
		}
		if (ap_has(line, ll, "deleted file mode ")) {
			char *v = ap_after(line, ll, "deleted file mode ");

			cur->old_mode = (u32)strtoul(v, NULL, 8);
			cur->new_absent = 1;
			free(v);
			continue;
		}
		if (ap_has(line, ll, "old mode ")) {
			char *v = ap_after(line, ll, "old mode ");

			cur->old_mode = (u32)strtoul(v, NULL, 8);
			free(v);
			continue;
		}
		if (ap_has(line, ll, "new mode ")) {
			char *v = ap_after(line, ll, "new mode ");

			cur->new_mode = (u32)strtoul(v, NULL, 8);
			free(v);
			continue;
		}
		if (ap_has(line, ll, "rename from ")) {
			free(cur->old_path);
			cur->old_path = ap_after(line, ll, "rename from ");
			continue;
		}
		if (ap_has(line, ll, "rename to ")) {
			free(cur->new_path);
			cur->new_path = ap_after(line, ll, "rename to ");
			continue;
		}
		if (ap_has(line, ll, "--- ")) {
			char *p = ap_side_path(line, ll, "--- ");

			if (p == (char *)-1) {
				rc = -1;
				continue;
			}
			free(cur->old_path);
			cur->old_path = p;
			cur->old_absent = p == NULL;
			continue;
		}
		if (ap_has(line, ll, "+++ ")) {
			char *p = ap_side_path(line, ll, "+++ ");

			if (p == (char *)-1) {
				rc = -1;
				continue;
			}
			free(cur->new_path);
			cur->new_path = p;
			cur->new_absent = p == NULL;
			continue;
		}
		if (ap_has(line, ll, "index ") ||
		    ap_has(line, ll, "similarity index ") ||
		    ap_has(line, ll, "dissimilarity index ") ||
		    ap_has(line, ll, "copy from ") ||
		    ap_has(line, ll, "copy to "))
			continue;
		if (ap_has(line, ll, "GIT binary patch") ||
		    ap_has(line, ll, "Binary files ") ||
		    ap_has(line, ll, "literal ") ||
		    ap_has(line, ll, "delta ")) {
			gp_error("apply: binary patches are not supported");
			rc = -1;
			continue;
		}
		if (!ll)
			continue;
		gp_error("apply: cannot read the line: %.*s", (int)ll, line);
		rc = -1;
	}

	free(ls);
	return rc;
}

/* ------------------------------------------------------------------ */
/* applying hunks to one file                                          */

/* the other direction: a file's bytes, as the lines a hunk matches against */
static void ap_split_file(const char *buf, size_t len, struct ap_line **out,
			  size_t *nr)
{
	size_t n = 0, alloc = 0;
	const char *p = buf, *end = buf + len;
	struct ap_line *v = NULL;

	while (p < end) {
		const char *nlp = memchr(p, '\n', (size_t)(end - p));
		size_t l = nlp ? (size_t)(nlp - p) : (size_t)(end - p);

		if (n == alloc) {
			alloc = alloc ? alloc * 2 : 64;
			v = xrealloc(v, alloc * sizeof *v);
		}
		v[n].op = ' ';
		v[n].p = p;
		v[n].len = l;
		v[n].nl = nlp ? 1 : 0;
		n++;
		p = nlp ? nlp + 1 : end;
	}
	*out = v;
	*nr = n;
}

static int ap_line_eq(const struct ap_line *a, const struct ap_line *b)
{
	return a->len == b->len && a->nl == b->nl &&
	       (a->len == 0 || !memcmp(a->p, b->p, a->len));
}

/*
 * Where a hunk's preimage sits.  The place it says it sits is tried first, and
 * from there the search walks forwards and then backwards -- no fuzz, so the
 * lines have to be there, but a hunk above it that shifted the file down is
 * followed rather than refused.
 */
static long ap_find(const struct ap_line *file, size_t nf,
		    const struct ap_line *pre, size_t np, long want)
{
	long i, hi;

	if (np == 0)
		return want < 0 ? 0 : (want > (long)nf ? (long)nf : want);
	hi = (long)nf - (long)np;
	if (want < 0)
		want = 0;
	if (want > hi)
		want = hi;
	for (i = want; i <= hi; i++) {
		size_t k;

		for (k = 0; k < np; k++)
			if (!ap_line_eq(&file[i + k], &pre[k]))
				break;
		if (k == np)
			return i;
	}
	for (i = want - 1; i >= 0; i--) {
		size_t k;

		for (k = 0; k < np; k++)
			if (!ap_line_eq(&file[i + k], &pre[k]))
				break;
		if (k == np)
			return i;
	}
	return -1;
}

/* every hunk, in order, turned into the whole file it produces */
static int ap_apply_hunks(const char *path, struct ap_file *f,
			  const struct buf *old, struct buf *out)
{
	struct ap_line *file = NULL;
	size_t nf = 0, i, k;
	long delta = 0;
	int rc = -1;

	ap_split_file(old->b ? (const char *)old->b : "", old->len, &file, &nf);

	for (i = 0; i < f->nr; i++) {
		struct ap_hunk *h = &f->h[i];
		struct ap_line *pre = NULL, *post = NULL;
		size_t np = 0, npost = 0, cap = h->nr ? h->nr : 1;
		long pos;

		pre = xmalloc(cap * sizeof *pre);
		post = xmalloc(cap * sizeof *post);
		for (k = 0; k < h->nr; k++) {
			if (h->l[k].op != '+')
				pre[np++] = h->l[k];
			if (h->l[k].op != '-')
				post[npost++] = h->l[k];
		}
		if (np != (size_t)h->a_len || npost != (size_t)h->b_len) {
			gp_error("%s: patch is corrupt", path);
			free(pre);
			free(post);
			goto out;
		}

		if (np == 0)
			pos = h->a_start + delta;      /* inserted after */
		else
			pos = ap_find(file, nf, pre, np, h->a_start - 1 + delta);

		if (pos < 0) {
			gp_error("patch failed: %s:%ld", path, h->a_start);
			gp_error("%s: patch does not apply", path);
			free(pre);
			free(post);
			goto out;
		}

		{
			size_t tail = nf - (size_t)pos - np;
			size_t newn = (size_t)pos + npost + tail;
			struct ap_line *nv = xmalloc((newn ? newn : 1) *
						     sizeof *nv);

			memcpy(nv, file, (size_t)pos * sizeof *nv);
			memcpy(nv + pos, post, npost * sizeof *nv);
			memcpy(nv + pos + npost, file + pos + np,
			       tail * sizeof *nv);
			free(file);
			file = nv;
			nf = newn;
		}
		delta += (long)h->b_len - (long)h->a_len;
		free(pre);
		free(post);
	}

	buf_reset(out);
	for (k = 0; k < nf; k++) {
		buf_add(out, file[k].p, file[k].len);
		if (file[k].nl)
			buf_addch(out, '\n');
	}
	rc = 0;
out:
	free(file);
	return rc;
}

static void ap_patch_release(struct ap_patch *p)
{
	size_t i, j;

	for (i = 0; i < p->nr; i++) {
		free(p->f[i].old_path);
		free(p->f[i].new_path);
		for (j = 0; j < p->f[i].nr; j++)
			free(p->f[i].h[j].l);
		free(p->f[i].h);
	}
	free(p->f);
	memset(p, 0, sizeof *p);
}

/* ------------------------------------------------------------------ */
/* paths, which are not allowed to leave the work tree                 */

/*
 * A path with a "..", or one that is absolute, or one that is not written the
 * way a git path is written at all, is refused -- unless --unsafe-paths says
 * to take it, in which case it is normalised against the work tree root rather
 * than written where it says.  Either way the normalised form is what the rest
 * of the command uses, so nothing here can write outside the work tree.
 */
static int ap_check_path(const char *path, int unsafe_paths)
{
	const char *q;
	int bad = 0;

	if (!*path)
		bad = 1;
	if (path[0] == '/' || strchr(path, '\\') ||
	    (isalpha((unsigned char)path[0]) && path[1] == ':'))
		bad = 1;
	for (q = path; *q && !bad;) {
		const char *slash = strchr(q, '/');
		size_t n = slash ? (size_t)(slash - q) : strlen(q);

		if (n == 0 || (n == 2 && q[0] == '.' && q[1] == '.'))
			bad = 1;
		q = slash ? slash + 1 : q + n;
	}
	if (!bad)
		return 0;
	if (!unsafe_paths) {
		gp_error("invalid path '%s'", path);
		return -1;
	}
	return 0;
}

static char *ap_root_path(const char *path)
{
	struct buf b = BUF_INIT;
	char *out;

	path_normalize(path, &b);
	out = xstrdup(buf_cstr(&b));
	buf_release(&b);
	return out;
}

/* ------------------------------------------------------------------ */
/* the command                                                         */

static void ap_slurp_stdin(struct buf *out)
{
	char chunk[65536];
	size_t n;

	buf_reset(out);
	while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0)
		buf_add(out, chunk, n);
}

struct ap_result {
	struct buf content;
	u32 mode;
};

/* one patch: every file in it, applied to memory first and to disk after */
static int ap_apply_patch(struct repo *r, const struct buf *patch,
			  struct index_state *ist, int use_index, int cached,
			  int check, int unsafe_paths, int verbose,
			  int *ist_dirty)
{
	struct ap_patch p;
	struct ap_result *res = NULL;
	size_t i;
	int rc = 0;

	if (ap_parse((const char *)patch->b, patch->len, &p) < 0) {
		ap_patch_release(&p);
		return 1;
	}
	if (p.nr == 0) {
		gp_error("apply: unrecognized input");
		ap_patch_release(&p);
		return 1;
	}

	res = xcalloc(p.nr, sizeof *res);

	/* read every file and match every hunk, touching nothing */
	for (i = 0; i < p.nr && rc == 0; i++) {
		struct ap_file *f = &p.f[i];
		char *opath = NULL;
		struct buf old = BUF_INIT;

		if (f->old_path && ap_check_path(f->old_path, unsafe_paths) < 0) {
			rc = 1;
			break;
		}
		if (f->new_path && ap_check_path(f->new_path, unsafe_paths) < 0) {
			rc = 1;
			break;
		}
		if (f->old_path)
			opath = ap_root_path(f->old_path);

		if (f->old_absent) {
			buf_reset(&old);
		} else if (use_index) {
			struct index_entry *e = index_get(ist, opath);

			if (!e || e->stage) {
				gp_error("%s: does not exist in the index",
					 opath);
				rc = 1;
			} else if (odb_read(&r->odb, &e->oid, NULL, &old) < 0) {
				gp_error("apply: cannot read %s", opath);
				rc = 1;
			}
		} else {
			char *full = xstrfmt("%s/%s", r->root, opath);

			if (read_file(full, &old) < 0) {
				gp_error("%s: does not exist in the working tree",
					 opath);
				rc = 1;
			}
			free(full);
		}

		if (rc == 0) {
			res[i].mode = f->new_mode;
			if (!res[i].mode && use_index && opath) {
				struct index_entry *e = index_get(ist, opath);

				if (e)
					res[i].mode = e->mode;
			}
			if (!res[i].mode)
				res[i].mode = MODE_BLOB;
			if (ap_apply_hunks(opath ? opath : "?", f, &old,
					   &res[i].content) < 0)
				rc = 1;
		}

		buf_release(&old);
		free(opath);
	}

	/* and only now, with every hunk matched, write anything */
	for (i = 0; i < p.nr && rc == 0 && !check; i++) {
		struct ap_file *f = &p.f[i];
		char *opath = f->old_path ? ap_root_path(f->old_path) : NULL;
		char *npath = f->new_path ? ap_root_path(f->new_path) : NULL;
		u32 mode = res[i].mode;

		if (!cached && r->root) {
			if (f->new_absent) {
				if (opath) {
					char *full = xstrfmt("%s/%s", r->root,
							     opath);

					remove_file(full);
					free(full);
				}
			} else {
				char *full = xstrfmt("%s/%s", r->root, npath);
				char *dir = xstrdup(full);
				char *slash = strrchr(dir, '/');

				if (slash) {
					*slash = '\0';
					mkdir_p(dir);
				}
				free(dir);
				if (write_file(full, res[i].content.b,
					       res[i].content.len) < 0) {
					gp_error("apply: cannot write %s",
						 npath);
					rc = 1;
				} else {
					ap_chmod(full, (mode & 0100) ? 0755
								     : 0644);
				}
				free(full);
				if (opath && strcmp(opath, npath)) {
					char *o = xstrfmt("%s/%s", r->root,
							  opath);

					remove_file(o);
					free(o);
				}
			}
		}

		if (use_index && rc == 0) {
			if (f->new_absent) {
				if (opath)
					index_remove(ist, opath);
			} else {
				oid_t oid;
				struct index_entry e;

				if (odb_write(&r->odb, OBJ_BLOB,
					      res[i].content.b,
					      res[i].content.len,
					      &oid) < 0) {
					gp_error("apply: cannot write the "
						 "blob for %s", npath);
					rc = 1;
				} else {
					memset(&e, 0, sizeof e);
					if (!cached && r->root) {
						char *full = xstrfmt("%s/%s",
								     r->root,
								     npath);

						index_fill_stat(&e, full);
						free(full);
					}
					e.mode = mode;
					e.oid = oid;
					e.path = npath;
					e.flags = 0;
					if (opath && strcmp(opath, npath))
						index_remove(ist, opath);
					index_add(ist, &e);
					*ist_dirty = 1;
				}
			}
		}

		if (rc == 0 && verbose)
			printf("Applied patch %s cleanly.\n",
			       npath ? npath : opath);
		free(opath);
		free(npath);
	}

	for (i = 0; i < p.nr; i++)
		buf_release(&res[i].content);
	free(res);
	ap_patch_release(&p);
	return rc;
}

int cmd_apply(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct index_state ist;
	int ist_loaded = 0, ist_dirty = 0;
	int verbose, check, cached, use_index, unsafe;
	int rc = 0, npatches, i;

	opts_init(&o, argc, argv, (const char *const[]){
		"-v", "--verbose", "--check", "--index", "--cached",
		"--unsafe-paths", NULL });

	verbose = opts_flag(&o, "-v") || opts_flag(&o, "--verbose");
	check = opts_flag(&o, "--check");
	cached = opts_flag(&o, "--cached");
	use_index = cached || opts_flag(&o, "--index");
	unsafe = opts_flag(&o, "--unsafe-paths");

	if (!cached && !r->root)
		gp_die("apply: this operation must be run in a work tree");

	if (use_index) {
		memset(&ist, 0, sizeof ist);
		index_read(&ist, repo_index_path(r));
		ist_loaded = 1;
		if (index_has_unmerged(&ist)) {
			gp_error("apply: the index has unmerged entries");
			index_release(&ist);
			return 1;
		}
	}

	npatches = opts_count(&o);
	if (npatches == 0) {
		struct buf patch = BUF_INIT;

		ap_slurp_stdin(&patch);
		rc = ap_apply_patch(r, &patch, &ist, use_index, cached, check,
				    unsafe, verbose, &ist_dirty);
		buf_release(&patch);
	} else {
		for (i = 0; i < npatches && rc == 0; i++) {
			struct buf patch = BUF_INIT;

			if (read_file(opts_arg(&o, i), &patch) < 0) {
				gp_error("apply: cannot read %s",
					 opts_arg(&o, i));
				rc = 1;
				break;
			}
			rc = ap_apply_patch(r, &patch, &ist, use_index, cached,
					    check, unsafe, verbose, &ist_dirty);
			buf_release(&patch);
		}
	}

	if (rc == 0 && ist_dirty)
		index_write(&ist, repo_index_path(r));
	if (ist_loaded)
		index_release(&ist);
	return rc;
}
