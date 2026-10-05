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
}

static void repo_adopt(struct repo *r, char *gpdir, const char *root)
{
	memset(r, 0, sizeof *r);
	r->gpdir = gpdir;
	r->root = xstrdup(root);
	repo_odb_init(r);
}

/*
 * Walk up from `start` until a .gitprompt directory turns up.  The search
 * runs on an absolute path, which makes "reached the filesystem root" the
 * single termination condition -- a relative path would loop forever at ".".
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
		char *candidate = xstrfmt("%s/%s", cur, GP_DIR);

		if (is_directory(candidate) && looks_like_gpdir(candidate)) {
			repo_adopt(r, candidate, cur);
			free(cur);
			return 0;
		}
		free(candidate);
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
	char *gpdir;

	if (is_directory(dir) && looks_like_gpdir(dir))
		gpdir = xstrdup(dir);
	else {
		gpdir = xstrfmt("%s/%s", dir, GP_DIR);
		if (!looks_like_gpdir(gpdir)) {
			free(gpdir);
			return -1;
		}
	}
	memset(r, 0, sizeof *r);
	r->gpdir = gpdir;
	r->root = is_directory(dir) && looks_like_gpdir(dir) ? NULL : xstrdup(dir);
	repo_odb_init(r);
	return 0;
}

void repo_release(struct repo *r)
{
	free(r->root);
	free(r->gpdir);
	odb_release(&r->odb);
	refs_release(&r->refs);
	r->root = r->gpdir = NULL;
}

char *repo_git_path(struct repo *r, const char *fmt, ...)
{
	va_list ap;
	int n;
	char *rest, *full;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	rest = xmalloc((size_t)n + 1);
	va_start(ap, fmt);
	vsnprintf(rest, (size_t)n + 1, fmt, ap);
	va_end(ap);

	full = xstrfmt("%s/%s", r->gpdir, rest);
	free(rest);
	return full;
}

const char *repo_index_path(struct repo *r)
{
	static char *cached = NULL;
	free(cached);
	cached = repo_git_path(r, "index");
	return cached;
}

const char *repo_head_path(struct repo *r)
{
	static char *cached = NULL;
	free(cached);
	cached = repo_git_path(r, "HEAD");
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

int repo_config_set(struct repo *r, const char *key, const char *value,
		    int global)
{
	char *path = global ? global_config_path() : local_config_path(r);
	struct lines l;
	char *want_section, *want_sub, *want_name;
	char *cur_section = NULL, *cur_sub = NULL;
	size_t i;
	int done = 0, insert_at = -1;

	if (!path)
		return -1;

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
	free(path);
	return 0;
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
