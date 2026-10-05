/*
 * repo.c - finding a repository, creating one, and the configuration and
 * identity that hang off it.
 *
 * Configuration lives in <gpdir>/config, and in ~/.gitpromptconfig for
 * --global.  The syntax is git's: [section] and [section "subsection"],
 * then "name = value".
 */
#include "gp.h"

#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define gp_getcwd(b, n) _getcwd((b), (int)(n))
#else
#include <unistd.h>
#define gp_getcwd(b, n) getcwd((b), (n))
#endif

#define GP_DIR ".gitprompt"

/* ------------------------------------------------------------------ */
/* discovery                                                           */

static int looks_like_gpdir(const char *path)
{
	char *probe = xstrfmt("%s/HEAD", path);
	int rc = is_file(probe);
	free(probe);
	return rc;
}

static int path_is_absolute(const char *p)
{
#ifdef _WIN32
	if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
	    p[1] == ':')
		return 1;
#endif
	return p[0] == '/';
}

/*
 * "." and ".." taken out of a path, with a leading "/" kept.  The common
 * directory of a linked worktree is named relative to that worktree's own, so
 * it arrives as something like "<main>/.gitprompt/worktrees/one/../..", and
 * every later comparison here is a plain string one.
 *
 * The separators are made "/" first, and that has to happen before any ".." is
 * folded rather than as the scan goes: a path from the system may be written
 * with "\\" throughout, and a component is only a component if the scan can
 * see where it ends.  Folding as we went would take "C:\\a\\b" for one name and
 * let the first ".." pop the whole of it.
 */
static void collapse_dots(const char *in, struct buf *out)
{
	size_t len = strlen(in);
	char *norm = xmalloc(len + 1);
	size_t i;
	int absolute;
	const char *p;

	for (i = 0; i < len; i++)
		norm[i] = in[i] == '\\' ? '/' : in[i];
	norm[len] = '\0';
	p = norm;

	absolute = p[0] == '/';
	buf_reset(out);
	while (*p == '/')
		p++;
	while (*p) {
		const char *slash = strchr(p, '/');
		size_t n = slash ? (size_t)(slash - p) : strlen(p);

		if (n == 0) {
			/* a doubled separator */
		} else if (n == 1 && p[0] == '.') {
			/* drop */
		} else if (n == 2 && p[0] == '.' && p[1] == '.') {
			size_t cut = out->len;
			while (cut > 0 && out->b[cut - 1] != '/')
				cut--;
			if (cut > 0)
				cut--;
			out->len = cut;
			if (out->b)
				out->b[out->len] = '\0';
		} else {
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
	if (absolute) {
		struct buf head = BUF_INIT;
		buf_addch(&head, '/');
		if (out->len)
			buf_add(&head, out->b, out->len);
		buf_reset(out);
		buf_add(out, head.b, head.len);
		buf_release(&head);
	}
	free(norm);
}

/*
 * A linked worktree keeps its own directory somewhere else and names it from a
 * .git *file* in the worktree: "gitdir: <path>".  Read one, or NULL when this
 * is not that file.
 */
static char *read_gitfile(const char *path)
{
	struct buf b = BUF_INIT;
	char *result = NULL;

	if (read_file(path, &b) >= 0) {
		char *s = (char *)b.b;
		size_t n = b.len;

		while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
			s[--n] = '\0';
		if (n > 8 && !memcmp(s, "gitdir: ", 8))
			result = xstrdup(s + 8);
	}
	buf_release(&b);
	return result;
}

/*
 * Where a worktree's own directory keeps its objects and refs.  A linked
 * worktree names that directory in `commondir`, relative to itself; one with
 * no other worktrees has no such file and is its own.  NULL means "the
 * directory itself".  The caller owns the result.
 */
static char *read_commondir(const char *dir)
{
	char *file = xstrfmt("%s/commondir", dir);
	struct buf b = BUF_INIT;
	struct buf out = BUF_INIT;
	char *result = NULL;

	if (read_file(file, &b) >= 0) {
		char *s = (char *)b.b;
		size_t n = b.len;

		while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
			s[--n] = '\0';
		if (n) {
			char *joined = path_is_absolute(s) ? xstrdup(s) :
				xstrfmt("%s/%s", dir, s);

			collapse_dots(joined, &out);
			result = out.b ? xstrndup((const char *)out.b, out.len) :
				xstrdup("");
			free(joined);
		}
	}
	buf_release(&b);
	buf_release(&out);
	free(file);
	return result;
}

/* the path a .git file names, made absolute and free of "." and ".." */
static char *resolve_gitfile(const char *base, const char *named)
{
	struct buf out = BUF_INIT;
	char *joined = path_is_absolute(named) ? xstrdup(named) :
		xstrfmt("%s/%s", base, named);
	char *result;

	collapse_dots(joined, &out);
	result = out.b ? xstrndup((const char *)out.b, out.len) :
				xstrdup("");
	buf_release(&out);
	free(joined);
	return result;
}

/*
 * The object store and the refs, which are the same two pieces for a repository
 * however it was reached.  The replace refs are named here rather than inside
 * odb_init because they are a property of the repository and not of the store:
 * an odb opened for any other reason reads what is on disk and nothing else.
 */
static void repo_odb_init(struct repo *r)
{
	char *objects = xstrfmt("%s/objects", r->gpdir);
	char *replace = xstrfmt("%s/refs/replace", r->gpdir);

	odb_init(&r->odb, objects);
	odb_set_replace_dir(&r->odb, replace);
	free(objects);
	free(replace);
	refs_init(&r->refs, r->gpdir);
	refs_set_head_dir(&r->refs, r->wt_dir);
}

/*
 * `dir` is the directory that holds HEAD and the index -- the worktree's own
 * when the repository has more than one -- and `common` is where the objects
 * and the refs are.  Both are taken over by the repository.
 */
static void repo_adopt_at(struct repo *r, char *dir, char *common,
			  const char *root)
{
	memset(r, 0, sizeof *r);
	r->gpdir = common;
	r->wt_dir = dir;
	r->root = root ? xstrdup(root) : NULL;
	repo_odb_init(r);
}

/*
 * The repository a working directory belongs to.  A `.gitprompt` directory is
 * one form; a `.git` file naming a git directory elsewhere is the other, which
 * is how a linked worktree is reached.  Either way the answer is a directory
 * holding HEAD and an index, and a common directory holding the objects and
 * the refs -- the same one for both when there are no other worktrees.
 * Returns 0 and fills `r`, or -1.
 */
static int repo_try_dir(struct repo *r, const char *dir, const char *root)
{
	char *common;

	if (!looks_like_gpdir(dir))
		return -1;
	common = read_commondir(dir);
	repo_adopt_at(r, xstrdup(dir), common ? common : xstrdup(dir), root);
	return 0;
}

/* the repository whose store is `cur/<name>`, if that is what it is */
static int repo_try_gpdir(struct repo *r, const char *cur, const char *name)
{
	char *path = xstrfmt("%s/%s", cur, name);

	if (is_directory(path) && looks_like_gpdir(path)) {
		int rc = repo_try_dir(r, path, cur);

		free(path);
		return rc;
	}
	free(path);
	return -1;
}

/*
 * The other way in is a .git *file*, which is how a linked worktree names the
 * directory holding its HEAD and index.  A .git directory is deliberately not
 * taken: that is a git repository, and a plain `git clone` of a prompt history
 * leaves one behind -- it is a directory of prompts with no store, which the
 * commands have to say rather than treat as a repository.
 */
static int repo_try_gitfile(struct repo *r, const char *cur)
{
	char *path = xstrfmt("%s/.git", cur);
	int rc = -1;

	if (is_file(path)) {
		char *named = read_gitfile(path);

		if (named) {
			char *real = resolve_gitfile(cur, named);

			free(named);
			if (is_directory(real) && looks_like_gpdir(real)) {
				char *common = read_commondir(real);

				repo_adopt_at(r, real,
					      common ? common : xstrdup(real), cur);
				rc = 0;
			} else {
				free(real);
			}
		}
	}
	free(path);
	return rc;
}

/*
 * Walk up from `start` until a .gitprompt directory or a .git file turns up.
 * The search runs on an absolute path, which makes "reached the filesystem
 * root" the single termination condition -- a relative path would loop forever
 * at ".".
 */
int repo_find(struct repo *r, const char *start)
{
	char cwd[4096];
	char *cur;
	size_t n;

	if (!gp_getcwd(cwd, sizeof cwd))
		cwd[0] = '\0';

	if (!start || !*start)
		cur = xstrdup(cwd[0] ? cwd : ".");
	else if (path_is_absolute(start))
		cur = xstrdup(start);
	else
		cur = xstrfmt("%s/%s", cwd[0] ? cwd : ".", start);

	n = strlen(cur);
	while (n > 1 && cur[n - 1] == '/')
		cur[--n] = '\0';

	for (;;) {
		if (repo_try_gpdir(r, cur, GP_DIR) == 0) {
			free(cur);
			return 0;
		}
		if (repo_try_gitfile(r, cur) == 0) {
			free(cur);
			return 0;
		}
		{
			char *slash = strrchr(cur, '/');
			if (!slash || slash == cur)
				break;          /* "/" or a bare drive */
			*slash = '\0';
		}
	}
	free(cur);
	return -1;
}

int repo_open(struct repo *r, const char *dir)
{
	/* the directory itself, or a worktree whose .git names it */
	if (is_directory(dir) && looks_like_gpdir(dir))
		return repo_try_dir(r, dir, NULL);
	if (repo_try_gpdir(r, dir, GP_DIR) == 0)
		return 0;
	if (repo_try_gitfile(r, dir) == 0)
		return 0;
	return -1;
}

void repo_release(struct repo *r)
{
	free(r->root);
	free(r->gpdir);
	free(r->wt_dir);
	odb_release(&r->odb);
	refs_release(&r->refs);
	r->root = r->gpdir = r->wt_dir = NULL;
}

/*
 * A path with "." and ".." taken out, separators folded to "/", and nothing
 * else changed -- no lookup, no symlink resolved, so it works on a directory
 * that is not there yet.  Discovery needs this for the directories it reads
 * out of files; a command storing a path of its own wants the same answer, so
 * it is exported rather than written a second time.
 */
char *gp_clean_path(const char *in)
{
	struct buf out = BUF_INIT;
	char *result;

	collapse_dots(in, &out);
	result = out.b ? xstrndup((const char *)out.b, out.len) : xstrdup("");
	buf_release(&out);
	return result;
}

static char *gp_path_at(const char *base, const char *fmt, va_list ap)
{
	va_list ap2;
	int n;
	char *rest, *full;

	va_copy(ap2, ap);
	n = vsnprintf(NULL, 0, fmt, ap2);
	va_end(ap2);
	rest = xmalloc((size_t)n + 1);
	vsnprintf(rest, (size_t)n + 1, fmt, ap);

	full = xstrfmt("%s/%s", base, rest);
	free(rest);
	return full;
}

char *repo_git_path(struct repo *r, const char *fmt, ...)
{
	va_list ap;
	char *full;

	va_start(ap, fmt);
	full = gp_path_at(r->gpdir, fmt, ap);
	va_end(ap);
	return full;
}

char *repo_worktree_path(struct repo *r, const char *fmt, ...)
{
	va_list ap;
	char *full;

	va_start(ap, fmt);
	full = gp_path_at(r->wt_dir, fmt, ap);
	va_end(ap);
	return full;
}

const char *repo_index_path(struct repo *r)
{
	static char *cached = NULL;
	free(cached);
	cached = repo_worktree_path(r, "index");
	return cached;
}

const char *repo_head_path(struct repo *r)
{
	static char *cached = NULL;
	free(cached);
	cached = repo_worktree_path(r, "HEAD");
	return cached;
}

/* ------------------------------------------------------------------ */
/* configuration                                                       */

static char *global_config_path(void)
{
	const char *home = getenv("HOME");
	if (!home || !*home)
		home = getenv("USERPROFILE");
	if (!home || !*home)
		return NULL;
	return xstrfmt("%s/.gitpromptconfig", home);
}

static char *local_config_path(struct repo *r)
{
	return r ? xstrfmt("%s/config", r->gpdir) : NULL;
}

struct lines {
	char **v;
	size_t nr, alloc;
};

static void lines_load(struct lines *l, const char *path)
{
	struct buf b;
	const char *p;

	memset(l, 0, sizeof *l);
	buf_init(&b);
	if (read_file(path, &b) < 0) {
		buf_release(&b);
		return;
	}
	p = (const char *)b.b;
	{
		const char *start = p;
		const char *end = p + b.len;
		while (start < end) {
			const char *eol = memchr(start, '\n', (size_t)(end - start));
			size_t n = eol ? (size_t)(eol - start) : (size_t)(end - start);
			if (l->nr == l->alloc) {
				l->alloc = l->alloc ? l->alloc * 2 : 16;
				l->v = xrealloc(l->v, l->alloc * sizeof(char *));
			}
			l->v[l->nr++] = xstrndup(start, n);
			start = eol ? eol + 1 : end;
		}
	}
	buf_release(&b);
}

static void lines_free(struct lines *l)
{
	size_t i;
	for (i = 0; i < l->nr; i++)
		free(l->v[i]);
	free(l->v);
	memset(l, 0, sizeof *l);
}

static void lines_save(struct lines *l, const char *path)
{
	struct buf b;
	size_t i;
	buf_init(&b);
	for (i = 0; i < l->nr; i++) {
		buf_addstr(&b, l->v[i]);
		buf_addch(&b, '\n');
	}
	{
		char *dir = xstrdup(path);
		char *slash = strrchr(dir, '/');
		if (slash) {
			*slash = '\0';
			mkdir_p(dir);
		}
		free(dir);
	}
	write_file(path, b.b, b.len);
	buf_release(&b);
}

/*
 * Split a configuration key into the three parts a section header needs.
 * "user.name" -> section "user", no subsection, name "name".  A key with
 * two or more dots names a subsection: "remote.origin.url" -> section
 * "remote", subsection "origin", name "url".
 */
static void split_key(const char *key, char **section, char **subsection,
		      char **name)
{
	const char *first = strchr(key, '.');
	const char *second;

	*subsection = NULL;
	if (!first) {
		*section = xstrdup(key);
		*name = xstrdup("");
		return;
	}
	*section = xstrndup(key, (size_t)(first - key));
	second = strchr(first + 1, '.');
	if (!second) {
		*name = xstrdup(first + 1);
		return;
	}
	*subsection = xstrndup(first + 1, (size_t)(second - first - 1));
	*name = xstrdup(second + 1);
}

static int stricmp_ascii(const char *a, const char *b)
{
	while (*a && *b) {
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
		if (ca != cb)
			return ca - cb;
		a++;
		b++;
	}
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/*
 * Read a header line, e.g. "[user]" or "[remote \"origin\"]", into section
 * and subsection.  Returns 0 if the line is a header.
 */
static int parse_header(const char *line, char **section, char **subsection)
{
	const char *p = line, *close;
	char *s;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p != '[')
		return -1;
	p++;
	close = strchr(p, ']');
	if (!close)
		return -1;
	s = xstrndup(p, (size_t)(close - p));
	*subsection = NULL;
	{
		char *sp = strchr(s, ' ');
		if (sp) {
			char *q;
			*sp = '\0';
			q = sp + 1;
			while (*q == ' ')
				q++;
			if (*q == '"') {
				char *endq = strrchr(q + 1, '"');
				if (endq)
					*subsection = xstrndup(q + 1,
						(size_t)(endq - q - 1));
			} else {
				*subsection = xstrdup(q);
			}
		}
	}
	*section = s;
	return 0;
}

static int parse_key_value(const char *line, char **key, char **value)
{
	const char *p = line;
	const char *eq;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '#' || *p == ';' || !*p)
		return -1;
	eq = strchr(p, '=');
	if (!eq)
		return -1;
	*key = xstrndup(p, (size_t)(eq - p));
	{
		char *k = *key;
		size_t n = strlen(k);
		while (n && (k[n - 1] == ' ' || k[n - 1] == '\t'))
			k[--n] = '\0';
	}
	*value = xstrdup(eq + 1);
	{
		char *v = *value;
		while (*v == ' ' || *v == '\t')
			v++;
		{
			size_t n = strlen(v);
			while (n && (v[n - 1] == ' ' || v[n - 1] == '\t' ||
				     v[n - 1] == '\r'))
				v[--n] = '\0';
			if (v != *value)
				memmove(*value, v, n + 1);
		}
	}
	return 0;
}

/*
 * The names of the subsections of `section` -- the `sub` in `[section "sub"]`
 * -- in the order they appear.  A repeated subsection is named once.  This is
 * how the entries of `.gitmodules` are enumerated, since it is the subsection
 * that names a submodule and the keys under it are what describe one.
 */
struct slist *config_file_subsections(const char *path, const char *section)
{
	struct lines l;
	struct slist *out = xcalloc(1, sizeof(*out));
	size_t i, j;

	lines_load(&l, path);
	for (i = 0; i < l.nr; i++) {
		char *s, *sub;
		int seen = 0;

		if (parse_header(l.v[i], &s, &sub) != 0)
			continue;
		if (sub && !stricmp_ascii(s, section)) {
			for (j = 0; j < out->nr; j++)
				if (!strcmp(out->v[j], sub))
					seen = 1;
			if (!seen)
				slist_push(out, sub);
		}
		free(s);
		free(sub);
	}
	lines_free(&l);
	return out;
}

static int config_lookup_file(const char *path, const char *key, char **out)
{
	struct lines l;
	char *want_section, *want_sub, *want_name;
	char *cur_section = NULL, *cur_sub = NULL;
	size_t i;
	int found = -1;

	split_key(key, &want_section, &want_sub, &want_name);
	lines_load(&l, path);

	for (i = 0; i < l.nr; i++) {
		char *section, *subsection;
		if (parse_header(l.v[i], &section, &subsection) == 0) {
			free(cur_section);
			free(cur_sub);
			cur_section = section;
			cur_sub = subsection;
			continue;
		}
		if (!cur_section)
			continue;
		{
			char *k, *v;
			if (parse_key_value(l.v[i], &k, &v) == 0) {
				int match = !stricmp_ascii(cur_section, want_section) &&
					    !stricmp_ascii(k, want_name) &&
					    ((!want_sub && !cur_sub) ||
					     (want_sub && cur_sub &&
					      !stricmp_ascii(cur_sub, want_sub)));
				free(k);
				if (match) {
					if (out)
						*out = xstrdup(v);
					free(v);
					found = 0;
					break;
				}
				free(v);
			}
		}
	}
	free(cur_section);
	free(cur_sub);
	free(want_section);
	free(want_sub);
	free(want_name);
	lines_free(&l);
	return found;
}

/*
 * One piece of how an agent is driven: `gitprompt.agent.<name>.<field>`.
 *
 * The command line an agent answers to is a fact about the machine it is
 * installed on rather than about the history being replayed -- which is why the
 * agent is named at all -- and the same agent is installed differently, wrapped
 * in something, or older than the flags this tree ships.  So every piece of it
 * can be replaced here, and `builtin` is only what the shipped table says.
 *
 * A key set to the empty string means the agent has no such piece, which is not
 * the same as the key being absent: absence falls back to `builtin`, emptiness
 * takes it away.  The result is the caller's to free, and NULL means nothing --
 * neither config nor the table -- says what this piece is.
 */
char *repo_agent_setting(struct repo *r, const char *agent, const char *field,
			 const char *builtin)
{
	char *key = xstrfmt("gitprompt.agent.%s.%s", agent, field);
	char *v = NULL;

	if (repo_config_get(r, key, &v) == 0) {
		free(key);
		return v;
	}
	free(key);
	return builtin ? xstrdup(builtin) : NULL;
}

int repo_config_get(struct repo *r, const char *key, char **out)
{
	char *path;

	if (out)
		*out = NULL;
	path = local_config_path(r);
	if (path) {
		if (config_lookup_file(path, key, out) == 0) {
			free(path);
			return 0;
		}
		free(path);
	}
	path = global_config_path();
	if (path) {
		if (config_lookup_file(path, key, out) == 0) {
			free(path);
			return 0;
		}
		free(path);
	}
	return -1;
}

/* the same reader, opened to a named file: `.gitmodules` is read this way */
int config_file_get(const char *path, const char *key, char **out)
{
	return config_lookup_file(path, key, out);
}

int config_file_set(const char *path, const char *key, const char *value)
{
	struct lines l;
	char *want_section, *want_sub, *want_name;
	char *cur_section = NULL, *cur_sub = NULL;
	size_t i;
	int done = 0, insert_at = -1;

	split_key(key, &want_section, &want_sub, &want_name);
	lines_load(&l, path);

	for (i = 0; i < l.nr && !done; i++) {
		char *section, *subsection;
		if (parse_header(l.v[i], &section, &subsection) == 0) {
			/*
			 * A header opens a new section, so the previous one
			 * ended here.  If that was the section the key
			 * belongs to and the key was not in it, the key goes
			 * at the end of that section -- not at the end of the
			 * file, which is where continuing to scan would put
			 * it, under whatever section came last.
			 */
			if (insert_at >= 0) {
				free(section);
				free(subsection);
				break;
			}
			free(cur_section);
			free(cur_sub);
			cur_section = section;
			cur_sub = subsection;
			if (!stricmp_ascii(section, want_section) &&
			    ((!want_sub && !subsection) ||
			     (want_sub && subsection &&
			      !stricmp_ascii(subsection, want_sub))))
				insert_at = (int)i + 1;
			continue;
		}
		if (insert_at < 0)
			continue;
		{
			char *k, *v;
			if (parse_key_value(l.v[i], &k, &v) == 0) {
				int match = !stricmp_ascii(k, want_name);
				free(k);
				free(v);
				if (match) {
					free(l.v[i]);
					l.v[i] = xstrfmt("%s = %s", want_name, value);
					done = 1;
					break;
				}
			}
			insert_at = (int)i + 1;
		}
	}
	if (!done && insert_at >= 0) {
		l.v = xrealloc(l.v, (l.nr + 1) * sizeof(char *));
		memmove(l.v + insert_at + 1, l.v + insert_at,
			(l.nr - (size_t)insert_at) * sizeof(char *));
		l.v[insert_at] = xstrfmt("%s = %s", want_name, value);
		l.nr++;
		done = 1;
	}
	if (!done) {
		l.v = xrealloc(l.v, (l.nr + 3) * sizeof(char *));
		if (l.nr && l.v[l.nr - 1][0] != '\0')
			l.v[l.nr++] = xstrdup("");
		l.v[l.nr++] = want_sub
			? xstrfmt("[%s \"%s\"]", want_section, want_sub)
			: xstrfmt("[%s]", want_section);
		l.v[l.nr++] = xstrfmt("%s = %s", want_name, value);
	}

	free(cur_section);
	free(cur_sub);
	free(want_section);
	free(want_sub);
	free(want_name);
	lines_save(&l, path);
	lines_free(&l);
	return 0;
}

int repo_config_set(struct repo *r, const char *key, const char *value,
		    int global)
{
	char *path = global ? global_config_path() : local_config_path(r);
	int rc;

	if (!path)
		return -1;
	rc = config_file_set(path, key, value);
	free(path);
	return rc;
}

int repo_config_unset(struct repo *r, const char *key)
{
	char *path = local_config_path(r);
	struct lines l;
	char *want_section, *want_sub, *want_name;
	char *cur_section = NULL, *cur_sub = NULL;
	size_t i;
	int removed = 0;

	if (!path)
		return -1;
	split_key(key, &want_section, &want_sub, &want_name);
	lines_load(&l, path);

	for (i = 0; i < l.nr; i++) {
		char *section, *subsection;
		if (parse_header(l.v[i], &section, &subsection) == 0) {
			free(cur_section);
			free(cur_sub);
			cur_section = section;
			cur_sub = subsection;
			continue;
		}
		if (!cur_section)
			continue;
		{
			char *k, *v;
			if (parse_key_value(l.v[i], &k, &v) == 0) {
				int match = !stricmp_ascii(cur_section, want_section) &&
					    !stricmp_ascii(k, want_name) &&
					    ((!want_sub && !cur_sub) ||
					     (want_sub && cur_sub &&
					      !stricmp_ascii(cur_sub, want_sub)));
				free(k);
				free(v);
				if (match) {
					free(l.v[i]);
					memmove(l.v + i, l.v + i + 1,
						(l.nr - i - 1) * sizeof(char *));
					l.nr--;
					removed = 1;
					i--;
				}
			}
		}
	}
	free(cur_section);
	free(cur_sub);
	free(want_section);
	free(want_sub);
	free(want_name);
	if (removed)
		lines_save(&l, path);
	lines_free(&l);
	free(path);
	return removed ? 0 : -1;
}

struct config_walk {
	void (*fn)(const char *k, const char *v, void *);
	void *data;
};

static void config_walk_file(const char *path, struct config_walk *w)
{
	struct lines l;
	char *cur_section = NULL, *cur_sub = NULL;
	size_t i;

	lines_load(&l, path);
	for (i = 0; i < l.nr; i++) {
		char *section, *subsection;
		if (parse_header(l.v[i], &section, &subsection) == 0) {
			free(cur_section);
			free(cur_sub);
			cur_section = section;
			cur_sub = subsection;
			continue;
		}
		if (!cur_section)
			continue;
		{
			char *k, *v;
			if (parse_key_value(l.v[i], &k, &v) == 0) {
				char *full = cur_sub
					? xstrfmt("%s.%s.%s", cur_section, cur_sub, k)
					: xstrfmt("%s.%s", cur_section, k);
				w->fn(full, v, w->data);
				free(full);
				free(k);
				free(v);
			}
		}
	}
	free(cur_section);
	free(cur_sub);
	lines_free(&l);
}

void repo_config_list(struct repo *r, int global,
		      void (*fn)(const char *k, const char *v, void *),
		      void *data)
{
	struct config_walk w;
	char *path = global ? global_config_path() : local_config_path(r);

	w.fn = fn;
	w.data = data;
	if (!path)
		return;
	if (is_file(path))
		config_walk_file(path, &w);
	free(path);
}

/* ------------------------------------------------------------------ */
/* identity                                                            */

void repo_ident(struct repo *r, struct buf *out)
{
	char *name = NULL, *email = NULL;

	repo_config_get(r, "user.name", &name);
	repo_config_get(r, "user.email", &email);
	buf_reset(out);
	if (name && *name)
		buf_addstr(out, name);
	else
		buf_addstr(out, "gitprompt");
	buf_addstr(out, " <");
	if (email && *email)
		buf_addstr(out, email);
	else
		buf_addstr(out, "gitprompt@localhost");
	buf_addch(out, '>');
	free(name);
	free(email);
}

void repo_ident_with_time(struct repo *r, struct buf *out)
{
	int sgn, oh, om;
	repo_ident(r, out);
	local_tz_offset(&sgn, &oh, &om);
	buf_addf(out, " %lld %c%02d%02d", (long long)now_epoch(),
		 sgn < 0 ? '-' : '+', oh, om);
}

/* ------------------------------------------------------------------ */
/* gitprompt settings                                                  */

/*
 * The work tree's path as it should be shown to a person.  Discovery is handed
 * ".", so the root it settles on carries that on the end, and "<root>/." is a
 * path to the same place that reads as a mistake.  The caller owns the result.
 */
char *repo_root_display(const struct repo *r)
{
	char *p = xstrdup(r->root ? r->root : ".");
	size_t n = strlen(p);

	if (n > 2 && p[n - 1] == '.' && p[n - 2] == '/')
		p[n - 2] = '\0';
	return p;
}

const char *repo_prompt_dir(struct repo *r)
{
	static char *cached = NULL;
	char *v = NULL;
	free(cached);
	cached = NULL;
	if (repo_config_get(r, "gitprompt.promptDir", &v) == 0) {
		cached = v;
		return cached;
	}
	return "prompts";
}

const char *repo_default_branch(struct repo *r)
{
	static char *cached = NULL;
	char *v = NULL;
	free(cached);
	cached = NULL;
	if (repo_config_get(r, "init.defaultBranch", &v) == 0 && v[0]) {
		cached = v;
		return cached;
	}
	free(v);
	return "main";
}

long repo_next_file_seq(struct repo *r)
{
	char *path = repo_git_path(r, "gitprompt-seq");
	struct buf b;
	long n = 1;

	buf_init(&b);
	if (read_file(path, &b) == 0)
		n = strtol((const char *)b.b, NULL, 10);
	if (n < 1)
		n = 1;
	buf_release(&b);
	free(path);
	return n;
}

void repo_bump_file_seq(struct repo *r, long n)
{
	char *path = repo_git_path(r, "gitprompt-seq");
	char *text = xstrfmt("%ld\n", n);
	write_file(path, text, strlen(text));
	free(text);
	free(path);
}

char *repo_current_session(struct repo *r)
{
	char *path = repo_git_path(r, "SESSION");
	struct buf b;
	char *out = NULL;

	buf_init(&b);
	if (read_file(path, &b) == 0) {
		char *s = (char *)b.b;
		size_t n = b.len;
		while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
			s[--n] = '\0';
		if (n)
			out = xstrdup(s);
	}
	buf_release(&b);
	free(path);
	return out;
}

int repo_set_current_session(struct repo *r, const char *id)
{
	char *path = repo_git_path(r, "SESSION");
	int rc;

	if (!id) {
		remove_file(path);
		free(path);
		return 0;
	}
	rc = write_file(path, id, strlen(id));
	free(path);
	return rc;
}
