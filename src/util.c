/*
 * util.c - buffers, allocation, diagnostics, paths and the filesystem.
 */
#include "gp.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define gp_mkdir(p) _mkdir(p)
#define gp_rmdir(p) _rmdir(p)
#define gp_unlink(p) _unlink(p)
#define gp_stat _stat64
#define gp_stat_t struct _stat64
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#else
#include <unistd.h>
#define gp_mkdir(p) mkdir((p), 0777)
#define gp_rmdir(p) rmdir(p)
#define gp_unlink(p) unlink(p)
#define gp_stat stat
#define gp_stat_t struct stat
#endif

/* ------------------------------------------------------------------ */
/* allocation                                                          */

void gp_die(const char *fmt, ...)
{
	va_list ap;
	fputs("fatal: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(128);
}

void gp_error(const char *fmt, ...)
{
	va_list ap;
	fputs("error: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void gp_warn(const char *fmt, ...)
{
	va_list ap;
	fputs("warning: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		gp_die("out of memory (%lu bytes)", (unsigned long)n);
	return p;
}

void *xcalloc(size_t n, size_t sz)
{
	void *p = calloc(n ? n : 1, sz ? sz : 1);
	if (!p)
		gp_die("out of memory");
	return p;
}

void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);
	if (!q)
		gp_die("out of memory (%lu bytes)", (unsigned long)n);
	return q;
}

char *xstrdup(const char *s)
{
	size_t n;
	char *p;
	if (!s)
		return NULL;
	n = strlen(s);
	p = xmalloc(n + 1);
	memcpy(p, s, n + 1);
	return p;
}

char *xstrndup(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

char *xstrfmt(const char *fmt, ...)
{
	va_list ap;
	int n;
	char *p;
	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		gp_die("vsnprintf failed");
	p = xmalloc((size_t)n + 1);
	va_start(ap, fmt);
	vsnprintf(p, (size_t)n + 1, fmt, ap);
	va_end(ap);
	return p;
}

/* ------------------------------------------------------------------ */
/* buffers                                                             */

void buf_init(struct buf *b)
{
	b->b = NULL;
	b->len = b->cap = 0;
}

void buf_release(struct buf *b)
{
	free(b->b);
	b->b = NULL;
	b->len = b->cap = 0;
}

void buf_reset(struct buf *b)
{
	b->len = 0;
	if (b->b)
		b->b[0] = '\0';
}

void buf_grow(struct buf *b, size_t need)
{
	size_t want = b->len + need + 1;
	size_t cap;
	if (want <= b->cap)
		return;
	cap = b->cap ? b->cap : 64;
	while (cap < want)
		cap = cap * 2;
	b->b = xrealloc(b->b, cap);
	b->cap = cap;
}

void buf_add(struct buf *b, const void *data, size_t len)
{
	if (!len)
		return;
	buf_grow(b, len);
	memcpy(b->b + b->len, data, len);
	b->len += len;
	b->b[b->len] = '\0';
}

void buf_addstr(struct buf *b, const char *s)
{
	if (s)
		buf_add(b, s, strlen(s));
}

void buf_addch(struct buf *b, int c)
{
	u8 ch = (u8)c;
	buf_add(b, &ch, 1);
}

void buf_addf(struct buf *b, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		gp_die("vsnprintf failed");
	buf_grow(b, (size_t)n);
	va_start(ap, fmt);
	vsnprintf((char *)b->b + b->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b->len += (size_t)n;
}

void buf_insert(struct buf *b, size_t pos, const void *data, size_t len)
{
	if (pos > b->len)
		pos = b->len;
	if (!len)
		return;
	buf_grow(b, len);
	memmove(b->b + pos + len, b->b + pos, b->len - pos);
	memcpy(b->b + pos, data, len);
	b->len += len;
	b->b[b->len] = '\0';
}

void buf_splice(struct buf *b, size_t pos, size_t len)
{
	if (pos > b->len)
		return;
	if (pos + len > b->len)
		len = b->len - pos;
	memmove(b->b + pos, b->b + pos + len, b->len - pos - len);
	b->len -= len;
	b->b[b->len] = '\0';
}

void buf_swap(struct buf *a, struct buf *b)
{
	struct buf t = *a;
	*a = *b;
	*b = t;
}

char *buf_detach(struct buf *b, size_t *lenp)
{
	char *p;
	buf_grow(b, 0);
	p = (char *)b->b;
	if (lenp)
		*lenp = b->len;
	b->b = NULL;
	b->len = b->cap = 0;
	return p;
}

const char *buf_cstr(struct buf *b)
{
	buf_grow(b, 0);
	b->b[b->len] = '\0';
	return (const char *)b->b;
}

/* ------------------------------------------------------------------ */
/* paths                                                               */

int is_directory(const char *path)
{
	gp_stat_t st;
	if (gp_stat(path, &st) != 0)
		return 0;
	return S_ISDIR(st.st_mode);
}

int is_file(const char *path)
{
	gp_stat_t st;
	if (gp_stat(path, &st) != 0)
		return 0;
	return !S_ISDIR(st.st_mode);
}

void mkdir_one(const char *path)
{
	gp_mkdir(path);
}

/*
 * Create every component of `path`.  A trailing component that already
 * exists as a directory is fine; anything else that fails is fatal, so a
 * half-created directory never goes unnoticed.
 */
int mkdir_p(const char *path)
{
	char *copy = xstrdup(path);
	size_t i, n = strlen(copy);
	int rc = 0;

	for (i = 1; i <= n; i++) {
		if (copy[i] == '/' || copy[i] == '\0') {
			char save = copy[i];
			copy[i] = '\0';
			if (copy[0] && !is_directory(copy)) {
				if (gp_mkdir(copy) != 0 && errno != EEXIST) {
					rc = -1;
					copy[i] = save;
					break;
				}
			}
			copy[i] = save;
		}
	}
	free(copy);
	return rc;
}

int read_file(const char *path, struct buf *out)
{
	FILE *f = fopen(path, "rb");
	char chunk[65536];
	size_t n;

	if (!f)
		return -1;
	buf_reset(out);
	while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
		buf_add(out, chunk, n);
	fclose(f);
	return 0;
}

int write_file(const char *path, const void *data, size_t len)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	if (len && fwrite(data, 1, len, f) != len) {
		fclose(f);
		return -1;
	}
	if (fclose(f) != 0)
		return -1;
	return 0;
}

int remove_file(const char *path)
{
	if (gp_unlink(path) != 0 && errno != ENOENT)
		return -1;
	return 0;
}

int remove_dir_recursive(const char *path)
{
	DIR *d;
	struct dirent *de;
	char *sub;

	if (!is_directory(path))
		return remove_file(path);

	d = opendir(path);
	if (!d)
		return -1;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		sub = xstrfmt("%s/%s", path, de->d_name);
		if (is_directory(sub))
			remove_dir_recursive(sub);
		else
			remove_file(sub);
		free(sub);
	}
	closedir(d);
	return gp_rmdir(path);
}

int copy_file(const char *src, const char *dst)
{
	struct buf b;
	int rc;
	buf_init(&b);
	if (read_file(src, &b) < 0) {
		buf_release(&b);
		return -1;
	}
	rc = write_file(dst, b.b, b.len);
	buf_release(&b);
	return rc;
}

/*
 * Normalise a path the way a tree stores it: forward slashes, no leading
 * "./" or "/", no trailing "/", no "//", no "." or ".." components.  The
 * result is what index and tree paths are compared against.
 */
void path_normalize(const char *in, struct buf *out)
{
	const char *p = in;
	buf_reset(out);
	while (*p == '/')
		p++;
	while (*p) {
		const char *slash = strchr(p, '/');
		size_t n = slash ? (size_t)(slash - p) : strlen(p);
		if (n == 1 && p[0] == '.') {
			/* drop */
		} else if (n == 2 && p[0] == '.' && p[1] == '.') {
			/* drop the last component already emitted */
			size_t cut = out->len;
			while (cut > 0 && out->b[cut - 1] != '/')
				cut--;
			if (cut > 0)
				cut--;
			out->len = cut;
			if (out->b)
				out->b[out->len] = '\0';
		} else if (n > 0) {
			if (out->len)
				buf_addch(out, '/');
			buf_add(out, p, n);
		}
		if (!slash)
			break;
		p = slash + 1;
		while (*p == '/')
			p++;
	}
}

/* ------------------------------------------------------------------ */
/* time                                                                */

i64 now_epoch(void)
{
	return (i64)time(NULL);
}

static void gmtime_fields(i64 t, int *y, int *mo, int *d, int *h, int *mi,
			  int *s, int *wday)
{
	time_t tt = (time_t)t;
	struct tm tmv;
#ifdef _WIN32
	gmtime_s(&tmv, &tt);
#else
	gmtime_r(&tt, &tmv);
#endif
	*y = tmv.tm_year + 1900;
	*mo = tmv.tm_mon + 1;
	*d = tmv.tm_mday;
	*h = tmv.tm_hour;
	*mi = tmv.tm_min;
	*s = tmv.tm_sec;
	if (wday)
		*wday = tmv.tm_wday;
}

/* the local UTC offset, as git writes it: "+0800" */
void local_tz_offset(int *sign, int *hours, int *mins)
{
	time_t t = time(NULL);
	struct tm local, utc;
#ifdef _WIN32
	localtime_s(&local, &t);
	gmtime_s(&utc, &t);
#else
	localtime_r(&t, &local);
	gmtime_r(&t, &utc);
#endif
	{
		long diff = (long)(mktime(&local) - mktime(&utc));
		int off_min = (int)(diff / 60);
		*sign = off_min < 0 ? -1 : 1;
		if (off_min < 0)
			off_min = -off_min;
		*hours = off_min / 60;
		*mins = off_min % 60;
	}
}

void epoch_to_iso8601(i64 t, struct buf *out)
{
	int y, mo, d, h, mi, s, sgn, oh, om;
	gmtime_fields(t, &y, &mo, &d, &h, &mi, &s, NULL);
	local_tz_offset(&sgn, &oh, &om);
	buf_reset(out);
	buf_addf(out, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
		 y, mo, d, h, mi, s, sgn < 0 ? '-' : '+', oh, om);
}

void now_iso8601(struct buf *out)
{
	int sgn, oh, om;
	struct tm lc;
	time_t t = time(NULL);
#ifdef _WIN32
	localtime_s(&lc, &t);
#else
	localtime_r(&t, &lc);
#endif
	local_tz_offset(&sgn, &oh, &om);
	buf_reset(out);
	buf_addf(out, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
		 lc.tm_year + 1900, lc.tm_mon + 1, lc.tm_mday,
		 lc.tm_hour, lc.tm_min, lc.tm_sec,
		 sgn < 0 ? '-' : '+', oh, om);
}

/*
 * Parse the ISO 8601 form above, and also the "1234567890 +0800" form git
 * uses inside commit objects, into epoch seconds.  Returns 0 on success.
 */
i64 parse_timestamp(const char *s)
{
	struct tm tmv;
	const char *p = s;
	int y, mo, d, h, mi, sec = 0;
	int sign = 0, oh = 0, om = 0;
	time_t base;

	memset(&tmv, 0, sizeof tmv);
	if (sscanf(p, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) >= 5) {
		tmv.tm_year = y - 1900;
		tmv.tm_mon = mo - 1;
		tmv.tm_mday = d;
		tmv.tm_hour = h;
		tmv.tm_min = mi;
		tmv.tm_sec = sec;
		/* find the offset suffix */
		while (*p && *p != 'Z' && *p != '+' && *p != '-')
			p++;
		if (*p == '+' || *p == '-') {
			sign = (*p == '-') ? -1 : 1;
			sscanf(p + 1, "%2d:%2d", &oh, &om);
		}
		base = mktime(&tmv);
		/* mktime reads the struct as local time; the suffix is the
		 * local offset, so subtract it to land on the true epoch */
		return (i64)base - (i64)sign * (oh * 3600 + om * 60);
	}
	/* git ident form: "<epoch> <+hhmm>" -- strtoll rather than %lld,
	 * which msvcrt's sscanf does not accept */
	{
		char *end;
		long long e = strtoll(p, &end, 10);
		if (end != p)
			return (i64)e;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* worktree walking                                                    */

/*
 * Sorted, so a caller building a tree sees entries in a stable order and
 * two runs over the same work tree produce the same tree.
 */
struct walk_entry {
	char *path;
};
struct walk_ctx {
	struct walk_entry *e;
	size_t nr, alloc;
	const char *skip;
};

static void walk_add(struct walk_ctx *c, const char *path)
{
	if (c->nr == c->alloc) {
		c->alloc = c->alloc ? c->alloc * 2 : 32;
		c->e = xrealloc(c->e, c->alloc * sizeof(*c->e));
	}
	c->e[c->nr++].path = xstrdup(path);
}

static int walk_cmp(const void *a, const void *b)
{
	const struct walk_entry *x = a, *y = b;
	return strcmp(x->path, y->path);
}

static void walk_recurse(struct repo *r, const char *rel, const char *abs,
			 struct walk_ctx *c)
{
	DIR *d = opendir(abs);
	struct dirent *de;

	if (!d)
		return;
	while ((de = readdir(d))) {
		char *subrel, *subabs;
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (!rel[0] && !strcmp(de->d_name, ".gitprompt"))
			continue;
		if (!rel[0] && !strcmp(de->d_name, ".git"))
			continue;
		subrel = rel[0] ? xstrfmt("%s/%s", rel, de->d_name)
				: xstrdup(de->d_name);
		subabs = xstrfmt("%s/%s", abs, de->d_name);
		if (is_directory(subabs)) {
			walk_recurse(r, subrel, subabs, c);
		} else {
			if (r && path_is_ignored(r, subrel)) {
				free(subrel);
				free(subabs);
				continue;
			}
			walk_add(c, subrel);
		}
		free(subrel);
		free(subabs);
	}
	closedir(d);
}

void walk_worktree(struct repo *r, void (*fn)(const char *relpath, void *),
		   void *data)
{
	struct walk_ctx c;
	size_t i;

	memset(&c, 0, sizeof c);
	c.skip = NULL;
	if (!r->root)
		return;
	walk_recurse(r, "", r->root, &c);
	qsort(c.e, c.nr, sizeof(*c.e), walk_cmp);
	for (i = 0; i < c.nr; i++) {
		fn(c.e[i].path, data);
		free(c.e[i].path);
	}
	free(c.e);
}

/* ------------------------------------------------------------------ */
/* ignore rules                                                        */

/*
 * A deliberately small .gitpromptignore: blank lines and #comments are
 * skipped, a trailing slash means directories only, and a pattern with no
 * slash matches a basename at any depth.  No negation, no **.  Enough to
 * keep editor droppings and build output out of a prompt repository.
 */
struct ignore_rule {
	char *pattern;
	int dir_only;
};

static struct ignore_rule *ignore_rules = NULL;
static size_t ignore_nr = 0, ignore_alloc = 0;
static const char *ignore_loaded_from = NULL;

static void ignore_load(struct repo *r)
{
	char *path;
	struct buf b;
	const char *p;

	if (ignore_loaded_from && r->root &&
	    !strcmp(ignore_loaded_from, r->root))
		return;
	free(ignore_rules);
	ignore_rules = NULL;
	ignore_nr = ignore_alloc = 0;
	{
		char *root = xstrdup(r->root ? r->root : "");
		free((void *)ignore_loaded_from);
		ignore_loaded_from = root;
	}
	path = xstrfmt("%s/.gitpromptignore", r->root);
	buf_init(&b);
	if (read_file(path, &b) < 0) {
		free(path);
		buf_release(&b);
		return;
	}
	free(path);
	p = (const char *)b.b;
	while (p && *p) {
		const char *eol = strchr(p, '\n');
		size_t n = eol ? (size_t)(eol - p) : strlen(p);
		char *line = xstrndup(p, n);
		size_t ln = strlen(line);
		while (ln && (line[ln - 1] == '\r' || line[ln - 1] == ' '))
			line[--ln] = '\0';
		if (ln && line[0] != '#') {
			int dir_only = 0;
			if (line[ln - 1] == '/') {
				line[--ln] = '\0';
				dir_only = 1;
			}
			if (ln) {
				if (ignore_nr == ignore_alloc) {
					ignore_alloc = ignore_alloc ? ignore_alloc * 2 : 16;
					ignore_rules = xrealloc(ignore_rules,
						ignore_alloc * sizeof(*ignore_rules));
				}
				ignore_rules[ignore_nr].pattern = line;
				ignore_rules[ignore_nr].dir_only = dir_only;
				ignore_nr++;
				line = NULL;
			}
		}
		free(line);
		if (!eol)
			break;
		p = eol + 1;
	}
	buf_release(&b);
}

int path_is_ignored(struct repo *r, const char *relpath)
{
	size_t i;
	const char *base;

	if (!r || !r->root)
		return 0;
	ignore_load(r);
	base = strrchr(relpath, '/');
	base = base ? base + 1 : relpath;
	for (i = 0; i < ignore_nr; i++) {
		const char *pat = ignore_rules[i].pattern;
		if (ignore_rules[i].dir_only)
			continue;
		if (strchr(pat, '/')) {
			if (!strcmp(pat, relpath))
				return 1;
		} else if (!strcmp(pat, base)) {
			return 1;
		}
	}
	return 0;
}
