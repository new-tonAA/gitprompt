/*
 * refs.c - loose refs, symbolic refs, packed-refs and reflogs.
 *
 * The layout is git's: refs/heads/<branch>, refs/tags/<tag>, HEAD holding
 * either "ref: refs/heads/<name>" or a bare id.  packed-refs is read but
 * never written -- every ref gitprompt creates is loose, which git accepts.
 */
#include "gp.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

void refs_init(struct ref_store *r, const char *gpdir)
{
	r->dir = xstrdup(gpdir);
	r->ident = NULL;
}

void refs_release(struct ref_store *r)
{
	free(r->dir);
	free(r->ident);
	r->dir = NULL;
	r->ident = NULL;
}

static char *ref_path(struct ref_store *r, const char *name)
{
	return xstrfmt("%s/%s", r->dir, name);
}

/* ------------------------------------------------------------------ */
/* names                                                               */

/*
 * git's refname rules, to the extent they matter here: a name is a path
 * under refs/, so it may not contain the things a path may not contain,
 * nor the characters git reserves for its own syntax.
 */
int refs_check_name(const char *name)
{
	const char *p;
	size_t len;

	if (!name || !*name)
		return -1;
	if (name[0] == '/')
		return -1;
	len = strlen(name);
	if (name[len - 1] == '/' || name[len - 1] == '.')
		return -1;
	if (strstr(name, "..") || strstr(name, "//") || strstr(name, "@{"))
		return -1;
	if (!strcmp(name, "@"))
		return -1;
	if (strstr(name, ".lock"))
		return -1;
	for (p = name; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if (c < 0x20 || c == 0x7f)
			return -1;
		if (strchr(" ~^:?*[\\", c))
			return -1;
	}
	/* no component may begin with '.' */
	for (p = name; *p; p++) {
		if (*p == '/' && p[1] == '.')
			return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* reading                                                             */

/*
 * Read a ref, following one level of symbolic indirection.  On success
 * `symref` reports whether the ref was symbolic and `target`, if asked for,
 * receives the name it pointed at.
 */
int refs_read_1(struct ref_store *r, const char *name, oid_t *out, int *symref,
		char **target)
{
	char *path = ref_path(r, name);
	struct buf b;
	int rc = -1;

	buf_init(&b);
	if (read_file(path, &b) < 0)
		goto done;
	free(path);
	path = NULL;
	{
		char *s = (char *)b.b;
		size_t n = b.len;
		while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
			s[--n] = '\0';
		if (n > 5 && !memcmp(s, "ref: ", 5)) {
			char *t = xstrdup(s + 5);
			if (symref)
				*symref = 1;
			if (target)
				*target = t;
			else
				free(t);
			rc = refs_read(r, s + 5, out) == 0 ? 0 : 1;
			goto done;
		}
		if (symref)
			*symref = 0;
		if (target)
			*target = NULL;
		if (n != GP_SHA1_HEXSZ || oid_parse(out, s) < 0)
			goto done;
		rc = 0;
	}
done:
	if (path)
		free(path);
	buf_release(&b);
	return rc;
}

int refs_read(struct ref_store *r, const char *name, oid_t *out)
{
	int rc = refs_read_1(r, name, out, NULL, NULL);
	return rc == 0 ? 0 : -1;
}

int refs_exists(struct ref_store *r, const char *name)
{
	char *path = ref_path(r, name);
	int rc = is_file(path);
	free(path);
	return rc;
}

/* ------------------------------------------------------------------ */
/* writing                                                             */

int refs_write(struct ref_store *r, const char *name, const oid_t *oid)
{
	char *path = ref_path(r, name);
	char *dir;
	char hex[GP_SHA1_HEXSZ + 2];
	int rc;

	if (refs_check_name(name) < 0) {
		gp_error("refusing to write a ref with an invalid name: %s", name);
		free(path);
		return -1;
	}
	dir = xstrdup(path);
	{
		char *slash = strrchr(dir, '/');
		if (slash)
			*slash = '\0';
		if (mkdir_p(dir) < 0) {
			gp_error("cannot create %s", dir);
			free(dir);
			free(path);
			return -1;
		}
	}
	free(dir);

	oid_hex(oid, hex);
	hex[GP_SHA1_HEXSZ] = '\n';
	hex[GP_SHA1_HEXSZ + 1] = '\0';
	rc = write_file(path, hex, GP_SHA1_HEXSZ + 1);
	free(path);
	return rc;
}

int refs_delete(struct ref_store *r, const char *name)
{
	char *path = ref_path(r, name);
	int rc;
	if (!is_file(path)) {
		free(path);
		return -1;
	}
	rc = remove_file(path);
	free(path);
	return rc;
}

int refs_set_head(struct ref_store *r, const char *target)
{
	char *path = ref_path(r, "HEAD");
	char *line = xstrfmt("ref: %s\n", target);
	int rc = write_file(path, line, strlen(line));
	free(line);
	free(path);
	return rc;
}

int refs_set_head_detached(struct ref_store *r, const oid_t *oid)
{
	char *path = ref_path(r, "HEAD");
	char hex[GP_SHA1_HEXSZ + 2];
	oid_hex(oid, hex);
	hex[GP_SHA1_HEXSZ] = '\n';
	hex[GP_SHA1_HEXSZ + 1] = '\0';
	{
		int rc = write_file(path, hex, GP_SHA1_HEXSZ + 1);
		free(path);
		return rc;
	}
}

/*
 * The name HEAD points at, e.g. "refs/heads/main".  A detached HEAD has no
 * symbolic target and yields NULL; callers that need an id use refs_head.
 */
char *refs_head_target(struct ref_store *r)
{
	char *path = ref_path(r, "HEAD");
	struct buf b;
	char *result = NULL;

	buf_init(&b);
	if (read_file(path, &b) >= 0) {
		char *s = (char *)b.b;
		size_t n = b.len;
		while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
			s[--n] = '\0';
		if (n > 5 && !memcmp(s, "ref: ", 5))
			result = xstrdup(s + 5);
	}
	buf_release(&b);
	free(path);
	return result;
}

int refs_head(struct ref_store *r, oid_t *out)
{
	return refs_read(r, "HEAD", out);
}

/* ------------------------------------------------------------------ */
/* reflogs                                                             */

/*
 * Append one reflog line.  Old and new are the values either side of the
 * update; the zero id marks the beginning or the end of a ref's life, the
 * same convention git uses.
 */
void refs_reflog(struct ref_store *r, const char *name, const oid_t *old,
		 const oid_t *new, const char *msg)
{
	char *path = xstrfmt("%s/logs/%s", r->dir, name);
	char *dir = xstrdup(path);
	FILE *f;
	char oldhex[GP_SHA1_HEXSZ + 1], newhex[GP_SHA1_HEXSZ + 1];
	char *slash = strrchr(dir, '/');

	if (slash)
		*slash = '\0';
	if (mkdir_p(dir) < 0) {
		free(dir);
		free(path);
		return;
	}
	free(dir);
	f = fopen(path, "ab");
	free(path);
	if (!f)
		return;
	oid_hex(old ? old : &null_oid, oldhex);
	oid_hex(new, newhex);
	fprintf(f, "%s %s %s\t%s\n", oldhex, newhex,
		r->ident ? r->ident : "gitprompt <gitprompt@localhost>", msg);
	fclose(f);
}

/* ------------------------------------------------------------------ */
/* listing                                                             */

/*
 * Walk loose refs under `prefix`.  The recursion collects files; packed
 * refs are a separate pass so a caller can merge the two and let loose
 * win.
 */
/*
 * Callers pass a prefix the way a ref is written -- "refs/heads/" -- but the
 * names handed back to `fn` are built by appending "/<child>" to it, so a
 * prefix that already ends in a slash would produce "refs//heads/main".  The
 * trailing slashes come off here, once, rather than at every call site.
 */
static char *trim_slashes(const char *prefix)
{
	char *p = xstrdup(prefix);
	size_t n = strlen(p);

	while (n && p[n - 1] == '/')
		p[--n] = '\0';
	return p;
}

static void list_recurse(struct ref_store *r, const char *prefix,
			 const char *rel,
			 void (*fn)(const char *, const oid_t *, void *),
			 void *data)
{
	char *dir = rel ? xstrfmt("%s/%s/%s", r->dir, prefix, rel)
			: xstrfmt("%s/%s", r->dir, prefix);
	DIR *d = opendir(dir);

	free(dir);
	if (!d)
		return;
	for (;;) {
		struct dirent *de = readdir(d);
		char *childrel, *full;

		if (!de)
			break;
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		childrel = rel ? xstrfmt("%s/%s", rel, de->d_name)
			       : xstrdup(de->d_name);
		full = xstrfmt("%s/%s/%s", r->dir, prefix, childrel);
		if (is_directory(full)) {
			/* each level opens its own handle, so recursing with
			 * this one still open is safe */
			list_recurse(r, prefix, childrel, fn, data);
		} else {
			char *refname = xstrfmt("%s/%s", prefix, childrel);
			oid_t oid;
			if (refs_read(r, refname, &oid) == 0)
				fn(refname, &oid, data);
			free(refname);
		}
		free(full);
		free(childrel);
	}
	closedir(d);
}

void refs_list(struct ref_store *r, const char *prefix,
	       void (*fn)(const char *name, const oid_t *oid, void *), void *data)
{
	char *p = trim_slashes(prefix);

	list_recurse(r, p, NULL, fn, data);
	free(p);
}

void refs_list_packed(struct ref_store *r, const char *prefix,
		      void (*fn)(const char *name, const oid_t *oid, void *),
		      void *data)
{
	char *path = xstrfmt("%s/packed-refs", r->dir);
	char *pref = trim_slashes(prefix);
	struct buf b;
	const char *p;

	buf_init(&b);
	if (read_file(path, &b) < 0) {
		free(path);
		free(pref);
		buf_release(&b);
		return;
	}
	free(path);
	p = (const char *)b.b;
	while (p && *p) {
		const char *eol = strchr(p, '\n');
		size_t n = eol ? (size_t)(eol - p) : strlen(p);
		if (n > GP_SHA1_HEXSZ + 1 && p[0] != '#') {
			char hex[GP_SHA1_HEXSZ + 1];
			const char *name = p + GP_SHA1_HEXSZ + 1;
			size_t namelen;
			oid_t oid;

			memcpy(hex, p, GP_SHA1_HEXSZ);
			hex[GP_SHA1_HEXSZ] = '\0';
			namelen = n - GP_SHA1_HEXSZ - 1;
			if (oid_parse(&oid, hex) == 0 && namelen &&
			    !strncmp(name, pref, strlen(pref)) &&
			    (name[strlen(pref)] == '/' ||
			     name[strlen(pref)] == '\0')) {
				char *refname = xstrndup(name, namelen);
				while (namelen && refname[namelen - 1] == '\r')
					refname[--namelen] = '\0';
				fn(refname, &oid, data);
				free(refname);
			}
		}
		if (!eol)
			break;
		p = eol + 1;
	}
	free(pref);
	buf_release(&b);
}
