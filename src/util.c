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

/*
 * A prompt's text on one line: every newline and every run of blanks becomes a
 * single space, and the ends are trimmed.  The history is read at a glance in
 * several places -- `log-prompt --oneline`, `timeline` -- and a prompt is a
 * paragraph, so without this the second line of one pushes the entry it belongs
 * to off the screen.  The text itself is never cut: what is shown is the whole
 * thing, folded.
 */
void body_oneline(const char *body, struct buf *out)
{
	const char *p;

	buf_reset(out);
	if (!body)
		return;
	for (p = body; *p; p++) {
		if (*p == '\n' || *p == '\r' || *p == '\t' || *p == ' ') {
			if (out->len && out->b[out->len - 1] != ' ')
				buf_addch(out, ' ');
			continue;
		}
		buf_addch(out, *p);
	}
	while (out->len && out->b[out->len - 1] == ' ')
		out->len--;
	if (out->len)
		out->b[out->len] = '\0';
}

/*
 * A body printed as the paragraph it is, one line per line, each behind
 * `indent`.  Cutting at the first newline loses the rest of what somebody
 * wrote; this is how the full text is shown without reflowing it.
 */
void body_print_indented(const char *body, const char *indent)
{
	const char *p = body ? body : "";

	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);

		printf("%s%.*s\n", indent, (int)len, p);
		if (!nl)
			break;
		p = nl + 1;
	}
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

/*
 * Files git writes into a store come back read-only, and Windows refuses to
 * delete a read-only file.  Without this, `gc` could not drop the pack a
 * `git fetch` left behind -- the deletion would fail, silently, and the store
 * would keep the pack it had just superseded.  Clear the attribute and try
 * once more, which is what git does in the same place.
 */
static int make_writable(const char *path)
{
#ifdef _WIN32
	return _chmod(path, _S_IREAD | _S_IWRITE) == 0 ? 0 : -1;
#else
	return chmod(path, 0644) == 0 ? 0 : -1;
#endif
}

int remove_file(const char *path)
{
	if (gp_unlink(path) == 0 || errno == ENOENT)
		return 0;
	if (errno != EACCES && errno != EPERM)
		return -1;
	if (make_writable(path) != 0)
		return -1;
	if (gp_unlink(path) == 0 || errno == ENOENT)
		return 0;
	return -1;
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

/*
 * How far this machine's clock is ahead of UTC at a given instant, in
 * seconds.
 *
 * Asked about the instant in question rather than about now, because the two
 * are an hour apart either side of a daylight-saving change and a date from
 * last winter has to be read in the offset that was in force last winter.
 * mktime reads a broken-down time as local and gmtime gives UTC, so putting
 * the same instant through both and subtracting leaves the offset.
 */
static long tz_offset_secs(i64 t)
{
	time_t tt = (time_t)t;
	struct tm lc, uc;

#ifdef _WIN32
	localtime_s(&lc, &tt);
	gmtime_s(&uc, &tt);
#else
	localtime_r(&tt, &lc);
	gmtime_r(&tt, &uc);
#endif
	return (long)(mktime(&lc) - mktime(&uc));
}

/* the local UTC offset, as git writes it: "+0800" */
void local_tz_offset(int *sign, int *hours, int *mins)
{
	int off_min = (int)(tz_offset_secs(now_epoch()) / 60);

	*sign = off_min < 0 ? -1 : 1;
	if (off_min < 0)
		off_min = -off_min;
	*hours = off_min / 60;
	*mins = off_min % 60;
}

static void iso8601_write(struct buf *out, int y, int mo, int d, int h, int mi,
			  int s, int sgn, int oh, int om)
{
	buf_reset(out);
	buf_addf(out, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
		 y, mo, d, h, mi, s, sgn < 0 ? '-' : '+', oh, om);
}

/*
 * An instant as the clock at a given offset reads at that moment:
 * "2026-06-03T10:00:00+08:00".
 *
 * The fields are the local ones, and the offset beside them says which local.
 * Shifting the epoch by the offset before breaking it down is what makes the
 * two agree; without that shift the pair names an instant as many hours away
 * as the offset is wide, and something written at four in the afternoon here
 * was printed as eight the same morning.
 */
static void iso8601_at(i64 t, int sgn, int oh, int om, struct buf *out)
{
	int y, mo, d, h, mi, s;

	gmtime_fields(t + (i64)sgn * (oh * 3600 + om * 60), &y, &mo, &d, &h,
		      &mi, &s, NULL);
	iso8601_write(out, y, mo, d, h, mi, s, sgn, oh, om);
}

void epoch_to_iso8601(i64 t, struct buf *out)
{
	int sgn, oh, om;

	local_tz_offset(&sgn, &oh, &om);
	iso8601_at(t, sgn, oh, om, out);
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
	iso8601_write(out, lc.tm_year + 1900, lc.tm_mon + 1, lc.tm_mday,
		      lc.tm_hour, lc.tm_min, lc.tm_sec, sgn, oh, om);
}

/* a date that has been read, and the offset it was written in */
struct civil_time {
	i64 epoch;
	int sgn, oh, om;
	int have_off;
};

/*
 * Read one of the dates git writes: ISO 8601 with an offset, a bare day, or
 * an epoch with the offset after it ("1700000000 +0800", with the "@" that
 * git also allows).  The last of those is what a commit object carries and
 * what GIT_AUTHOR_DATE is usually given as.  Returns 0, or -1 if the text is
 * not a date at all.
 *
 * `have_off` says whether the text named an offset.  When it did, the fields
 * become the instant that offset names; when it did not, they are read as
 * this machine's local time, which is the only reading available.
 */
static int read_date(const char *s, struct civil_time *out)
{
	struct tm tmv;
	const char *p = s;
	int y, mo, d, h = 0, mi = 0, sec = 0, n = 0;

	out->epoch = 0;
	out->sgn = 1;
	out->oh = out->om = 0;
	out->have_off = 0;

	while (*p == ' ' || *p == '\t')
		p++;

	memset(&tmv, 0, sizeof tmv);
	tmv.tm_isdst = -1;

	if (sscanf(p, "%d-%d-%dT%d:%d:%d%n", &y, &mo, &d, &h, &mi, &sec,
		   &n) >= 5) {
		/*
		 * The offset is looked for after the seconds, never from the
		 * front: the first '+' or '-' in the text is the date's own
		 * separator, and a scan from the front read the "-06" of
		 * "2026-06-03" as an offset and put the date six hours from
		 * where it belonged.
		 */
		const char *q = p + n;

		if (*q == 'Z') {
			out->have_off = 1;
		} else if (*q == '+' || *q == '-') {
			out->sgn = (*q == '-') ? -1 : 1;
			out->have_off = sscanf(q + 1, "%2d:%2d", &out->oh,
					       &out->om) == 2 ||
					sscanf(q + 1, "%2d%2d", &out->oh,
					       &out->om) == 2;
		}
	} else if (strlen(p) == 10 &&
		   sscanf(p, "%d-%d-%d", &y, &mo, &d) == 3) {
		/* a bare day: midnight, with no offset to say whose */
		;
	} else {
		char *end;
		long long e;

		if (*p == '@')
			p++;
		e = strtoll(p, &end, 10);
		/* the digits have to be all of it, or "2026-06-03" would
		 * read as the epoch 2026 and pass for a date */
		if (end == p || (*end && *end != ' ' && *end != '\t'))
			return -1;
		while (*end == ' ' || *end == '\t')
			end++;
		if ((*end == '+' || *end == '-') &&
		    sscanf(end + 1, "%2d%2d", &out->oh, &out->om) == 2) {
			out->sgn = (*end == '-') ? -1 : 1;
			out->have_off = 1;
		}
		out->epoch = (i64)e;
		return 0;
	}

	tmv.tm_year = y - 1900;
	tmv.tm_mon = mo - 1;
	tmv.tm_mday = d;
	tmv.tm_hour = h;
	tmv.tm_min = mi;
	tmv.tm_sec = sec;

	/*
	 * mktime reads the fields as this machine's local time, so what it
	 * returns is the instant at which this machine's clock reads them.
	 * When the text named an offset, the instant meant is the one at
	 * which a clock at that offset reads them, which is a different
	 * instant whenever the two offsets differ -- so this machine's own
	 * offset has to be added back before the named one is taken off.
	 * Taking the named one off by itself left every date written in the
	 * offset this machine is already in shifted by that offset.
	 */
	out->epoch = (i64)mktime(&tmv);
	if (out->have_off)
		out->epoch += tz_offset_secs(out->epoch) -
			      (i64)out->sgn * (out->oh * 3600 + out->om * 60);
	return 0;
}

/*
 * A date as a person writes one, in the form the store keeps: the ISO 8601
 * that `--date` takes, or the epoch forms git uses for GIT_AUTHOR_DATE.  A
 * date given without an offset is read as one where this machine is, which is
 * the only reading that could be meant, and is written back out in that
 * offset.  Returns 0 on success, -1 if the text cannot be read as a date at
 * all -- the caller reports that, rather than quietly recording now instead.
 */
int date_to_iso8601(const char *s, struct buf *out)
{
	struct civil_time c;

	if (read_date(s, &c) < 0)
		return -1;
	if (!c.have_off)
		local_tz_offset(&c.sgn, &c.oh, &c.om);
	iso8601_at(c.epoch, c.sgn, c.oh, c.om, out);
	return 0;
}

/*
 * Parse the ISO 8601 form above, and also the "1234567890 +0800" form git
 * uses inside commit objects, into epoch seconds.  Returns 0 for text that is
 * not a date.
 */
i64 parse_timestamp(const char *s)
{
	struct civil_time c;

	if (read_date(s, &c) < 0)
		return 0;
	return c.epoch;
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

static int ignore_match(struct repo *r, const char *relpath, int is_dir)
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
		if (ignore_rules[i].dir_only && !is_dir)
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

int path_is_ignored(struct repo *r, const char *relpath)
{
	return ignore_match(r, relpath, 0);
}

/*
 * The same question asked about a directory.  A pattern may name a directory
 * or a file -- "build/" has to mean the directory, "build" means either --
 * so this is the only form that can honour a trailing slash.
 */
int path_is_ignored_dir(struct repo *r, const char *relpath)
{
	return ignore_match(r, relpath, 1);
}
