/*
 * cmd_remote.c - remotes, and moving objects between repositories.
 *
 * Three kinds of remote are recognised:
 *
 *   /path, file:///path, C:/path
 *       another gitprompt repository on this machine.  Handled here, by
 *       copying loose objects and writing refs -- no pack format, no
 *       protocol, no other program involved.
 *
 *   http://, https://, git://, ssh://, user@host:path
 *       a *git* repository used as a carrier, which is how prompts get
 *       hosted on GitHub.  Since a gitprompt repository holds nothing but
 *       ordinary git objects, git can push and fetch it as it stands: the
 *       object store is byte-compatible, so this delegates only the wire
 *       protocol to the git binary and does everything else itself.
 *
 *   gp://host:port/path
 *       gitprompt's own transport.  Reserved; `serve` reports that it is
 *       not implemented in this build rather than pretending otherwise.
 *
 * Everything a prompt needs to be reconstructed travels inside the prompt
 * files themselves, so a carrier repository needs no side channel: a plain
 * `git clone` of the GitHub repository hands an agent the whole history.
 */
#include "gp.h"

#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* remote configuration                                                */

void remote_list_release(struct remote_list *l)
{
	size_t i;
	for (i = 0; i < l->nr; i++) {
		free(l->e[i].name);
		free(l->e[i].url);
	}
	free(l->e);
	l->e = NULL;
	l->nr = l->alloc = 0;
}

static void remote_push(struct remote_list *l, const char *name, const char *url)
{
	if (l->nr == l->alloc) {
		l->alloc = l->alloc ? l->alloc * 2 : 4;
		l->e = xrealloc(l->e, l->alloc * sizeof(*l->e));
	}
	l->e[l->nr].name = xstrdup(name);
	l->e[l->nr].url = xstrdup(url);
	l->nr++;
}

struct remote_collect {
	struct remote_list *out;
};

static void remote_config_cb(const char *k, const char *v, void *ud)
{
	struct remote_collect *c = ud;
	const char *p;

	if (strncmp(k, "remote.", 7))
		return;
	p = k + 7;
	{
		const char *dot = strchr(p, '.');
		if (!dot || strcmp(dot, ".url"))
			return;
		remote_push(c->out, xstrndup(p, (size_t)(dot - p)), v);
	}
}

void remote_list_load(struct repo *r, struct remote_list *out)
{
	struct remote_collect c;
	c.out = out;
	repo_config_list(r, 0, remote_config_cb, &c);
}

void remotes_of(struct repo *r, struct remote_list *out)
{
	remote_list_load(r, out);
}

int remote_get_url(struct repo *r, const char *name, char **out)
{
	char *key = xstrfmt("remote.%s.url", name);
	int rc = repo_config_get(r, key, out);
	free(key);
	return rc;
}

int remote_add(struct repo *r, const char *name, const char *url)
{
	char *k = xstrfmt("remote.%s.url", name);
	int rc = repo_config_set(r, k, url, 0);
	free(k);
	return rc;
}

int remote_remove(struct repo *r, const char *name)
{
	char *k = xstrfmt("remote.%s.url", name);
	int rc = repo_config_unset(r, k);
	char *dir = repo_git_path(r, "refs/remotes/%s", name);
	free(k);
	if (dir) {
		if (is_directory(dir))
			remove_dir_recursive(dir);
		free(dir);
	}
	return rc;
}

int remote_set_url(struct repo *r, const char *name, const char *url)
{
	return remote_add(r, name, url);
}

void remote_rename(struct repo *r, const char *from, const char *to)
{
	char *url = NULL;
	if (remote_get_url(r, from, &url) == 0) {
		remote_add(r, to, url);
		remote_remove(r, from);
		free(url);
	}
}

/* ------------------------------------------------------------------ */
/* classifying a url                                                   */

static int url_is_local(const char *url)
{
	if (!strncmp(url, "file://", 7))
		return 1;
	if (strstr(url, "://"))
		return 0;
	if (!strncmp(url, "gp:", 3))
		return 0;
	/* "user@host:path" is ssh; a bare "C:/x" or "/x" or "./x" is local */
	{
		const char *colon = strchr(url, ':');
		const char *slash = strchr(url, '/');
		if (colon && (!slash || colon < slash) && colon != url + 1)
			return 0;
	}
	return 1;
}

static int url_is_gp(const char *url)
{
	return !strncmp(url, "gp://", 5) || !strncmp(url, "gp:", 3);
}

static const char *url_without_file_scheme(const char *url)
{
	if (!strncmp(url, "file://", 7))
		return url + 7;
	return url;
}

/* ------------------------------------------------------------------ */
/* running git, for the carrier transports only                        */

#include <process.h>

/*
 * Quote one argument for cmd.exe; refuse anything that could break out.
 *
 * Backslashes are path separators on Windows and cmd.exe does not treat them
 * as escapes, so they are normalised to forward slashes, which git and Windows
 * both accept.  That is not cosmetic: without it every delegated transport
 * fails on any Windows checkout, because --git-dir is an absolute path with
 * backslashes in it.  It also removes the "\\\"" sequence cmd's quoting is
 * known to mishandle.  What is left -- a quote, a percent sign, a newline --
 * is refused rather than escaped, since a path containing one of those is
 * rare enough that saying no beats guessing.
 */
static char *quote_arg(const char *s)
{
	char *slashed;
	char *q;
	char *p;

#ifdef _WIN32
	slashed = xstrdup(s);
	for (p = slashed; *p; p++)
		if (*p == '\\')
			*p = '/';
#else
	slashed = xstrdup(s);
#endif

	if (strpbrk(slashed, "\"\r\n%")) {
		gp_error("refusing to pass '%s' to the shell", s);
		free(slashed);
		return NULL;
	}
	q = xstrfmt("\"%s\"", slashed);
	free(slashed);
	return q;
}

static int run_git(const char *gpdir, const char *const *args, size_t nargs,
		   struct buf *capture)
{
	struct buf cmd;
	size_t i;
	int rc;

	buf_init(&cmd);
	buf_addstr(&cmd, "git");
	if (gpdir) {
		char *q = quote_arg(gpdir);
		if (!q) {
			buf_release(&cmd);
			return -1;
		}
		buf_addf(&cmd, " --git-dir=%s", q);
		free(q);
	}
	for (i = 0; i < nargs; i++) {
		char *q = quote_arg(args[i]);
		if (!q) {
			buf_release(&cmd);
			return -1;
		}
		buf_addf(&cmd, " %s", q);
		free(q);
	}
	if (capture)
		buf_addstr(&cmd, " 2>&1");

	if (capture) {
		FILE *p = popen(buf_cstr(&cmd), "r");
		char chunk[4096];
		size_t n;
		if (!p) {
			buf_release(&cmd);
			return -1;
		}
		/* capture is reset rather than initialised, so callers pass a
		 * buffer that is already empty -- BUF_INIT, not a bare
		 * declaration */
		buf_reset(capture);
		while ((n = fread(chunk, 1, sizeof chunk, p)) > 0)
			buf_add(capture, chunk, n);
		rc = pclose(p);
	} else {
		rc = system(buf_cstr(&cmd));
	}

	buf_release(&cmd);
	return rc;
}

static int git_available(void)
{
	struct buf out = BUF_INIT;
	int rc = run_git(NULL, (const char *const[]){ "--version", NULL }, 1, &out);
	buf_release(&out);
	return rc == 0;
}

/* ------------------------------------------------------------------ */
/* loose-object copying, for local remotes                             */

struct copy_ctx {
	const char *src_objects;
	const char *dst_objects;
	size_t copied, skipped;
	int failed;
};

static int copy_one_object(const oid_t *oid, void *ud)
{
	struct copy_ctx *c = ud;
	char hex[GP_SHA1_HEXSZ + 1];
	char *src, *dst, *dir, *slash;

	oid_hex(oid, hex);
	src = xstrfmt("%s/%c%c/%s", c->src_objects, hex[0], hex[1], hex + 2);
	dst = xstrfmt("%s/%c%c/%s", c->dst_objects, hex[0], hex[1], hex + 2);

	if (is_file(dst)) {
		c->skipped++;
	} else {
		dir = xstrdup(dst);
		slash = strrchr(dir, '/');
		if (slash) {
			*slash = '\0';
			mkdir_p(dir);
		}
		free(dir);
		if (copy_file(src, dst) < 0)
			c->failed++;
		else
			c->copied++;
	}
	free(src);
	free(dst);
	return 0;
}

/*
 * Copy every loose object across.  gitprompt never packs, so the loose
 * store is the whole store and this is a complete transfer.  Objects
 * already present are left alone.
 */
static void copy_all_objects(struct odb *from, struct odb *to, size_t *copied,
			     size_t *skipped)
{
	struct copy_ctx c;
	c.src_objects = from->dir;
	c.dst_objects = to->dir;
	c.copied = c.skipped = c.failed = 0;
	odb_foreach_loose(from, copy_one_object, &c);
	if (copied)
		*copied = c.copied;
	if (skipped)
		*skipped = c.skipped;
}

/* ------------------------------------------------------------------ */
/* reading a remote's refs                                             */

void remote_read_refs(struct repo *r, const char *url, const char *name,
		      void (*fn)(const char *refname, const oid_t *oid,
				 void *data), void *data)
{
	(void)r;
	(void)name;

	if (url_is_gp(url)) {
		gp_error("gp:// is not implemented in this build; use a local path "
			 "or an https:// carrier repository");
		return;
	}

	if (url_is_local(url)) {
		struct repo other;
		memset(&other, 0, sizeof other);
		if (repo_open(&other, url_without_file_scheme(url)) < 0) {
			gp_error("'%s' does not look like a gitprompt repository",
				 url);
			return;
		}
		refs_list(&other.refs, "refs/heads/", fn, data);
		refs_list(&other.refs, "refs/tags/", fn, data);
		repo_release(&other);
		return;
	}

	/* a carrier repository: ask git */
	{
		struct buf out = BUF_INIT;
		const char *args[] = { "ls-remote", "--heads", "--tags", url };
		const char *p;

		if (run_git(NULL, args, 4, &out) != 0) {
			gp_error("ls-remote failed for %s:\n%s", url,
				 buf_cstr(&out));
			buf_release(&out);
			return;
		}
		p = (const char *)out.b;
		while (p && *p) {
			const char *nl = strchr(p, '\n');
			size_t len = nl ? (size_t)(nl - p) : strlen(p);
			const char *tab = memchr(p, '\t', len);
			if (tab) {
				char *hex = xstrndup(p, (size_t)(tab - p));
				char *refname = xstrndup(tab + 1,
							 len - (size_t)(tab - p) - 1);
				oid_t oid;
				if (oid_parse(&oid, hex) == 0)
					fn(refname, &oid, data);
				free(hex);
				free(refname);
			}
			p = nl ? nl + 1 : NULL;
		}
		buf_release(&out);
	}
}

/* ------------------------------------------------------------------ */
/* fetching                                                            */

struct fetch_ref {
	char *refname;
	oid_t oid;
};

struct fetch_collect {
	struct fetch_ref *e;
	size_t nr, alloc;
};

static void fetch_collect_cb(const char *refname, const oid_t *oid, void *ud)
{
	struct fetch_collect *c = ud;
	if (c->nr == c->alloc) {
		c->alloc = c->alloc ? c->alloc * 2 : 8;
		c->e = xrealloc(c->e, c->alloc * sizeof(*c->e));
	}
	c->e[c->nr].refname = xstrdup(refname);
	c->e[c->nr].oid = *oid;
	c->nr++;
}

static void fetch_collect_release(struct fetch_collect *c)
{
	size_t i;
	for (i = 0; i < c->nr; i++)
		free(c->e[i].refname);
	free(c->e);
	c->e = NULL;
	c->nr = c->alloc = 0;
}

void remote_fetch_objects(struct repo *r, const char *url, const char *name,
			  const struct oid_array *wants)
{
	(void)wants;

	if (url_is_gp(url)) {
		gp_error("gp:// is not implemented in this build");
		return;
	}

	if (url_is_local(url)) {
		struct repo other;
		size_t copied = 0, skipped = 0;
		memset(&other, 0, sizeof other);
		if (repo_open(&other, url_without_file_scheme(url)) < 0) {
			gp_error("cannot read the repository at %s", url);
			return;
		}
		copy_all_objects(&other.odb, &r->odb, &copied, &skipped);
		repo_release(&other);
		return;
	}

	/* the carrier: let git fetch into the gitprompt object store, which
	 * it can, because the store is an ordinary git object store */
	{
		struct buf out = BUF_INIT;
		char *spec = xstrfmt("+refs/heads/*:refs/remotes/%s/*", name);
		const char *args[] = { "fetch", "--tags", url, spec };
		int rc = run_git(r->gpdir, args, 4, &out);
		if (rc != 0) {
			gp_error("fetch from %s failed:\n%s", url, buf_cstr(&out));
			buf_release(&out);
			free(spec);
			return;
		}
		buf_release(&out);
		free(spec);
	}
}

/*
 * Where a ref from `name` lands locally once it has been fetched.  A branch
 * becomes a remote-tracking ref under refs/remotes/<name>/; a tag keeps its own
 * namespace, which is where git puts a fetched tag and where `gitprompt tag`,
 * `describe` and the tag listing then look for it.  Fetch, clone and pull all
 * ask for the destination here rather than working it out on their own, which
 * is what let the reporting and the write disagree about tags.
 */
char *remote_tracking_ref(const char *name, const char *refname)
{
	if (!strncmp(refname, "refs/tags/", 10))
		return xstrfmt("refs/tags/%s", refname + 10);
	if (!strncmp(refname, "refs/heads/", 11))
		return xstrfmt("refs/remotes/%s/%s", name, refname + 11);
	/* anything else is kept under the remote by its full name */
	return xstrfmt("refs/remotes/%s/%s", name, refname);
}

void remote_update_local_ref(struct repo *r, const char *name,
			     const char *refname, const oid_t *oid)
{
	char *ref = remote_tracking_ref(name, refname);

	refs_write(&r->refs, ref, oid);
	free(ref);
}

/* ------------------------------------------------------------------ */
/* pushing                                                             */

void remote_push_objects(struct repo *r, const char *url, const char *name,
			 const struct oid_array *haves,
			 const struct oid_array *wants)
{
	(void)haves;
	(void)wants;

	if (url_is_gp(url)) {
		gp_error("gp:// is not implemented in this build");
		return;
	}

	if (url_is_local(url)) {
		struct repo other;
		size_t copied = 0, skipped = 0;
		memset(&other, 0, sizeof other);
		if (repo_open(&other, url_without_file_scheme(url)) < 0) {
			/* pushing to a path that does not exist yet creates it */
			const char *path = url_without_file_scheme(url);
			char *argv[2];
			if (mkdir_p(path) < 0) {
				gp_error("cannot create %s", path);
				return;
			}
			/* cmd_init takes the arguments that follow the command
			 * name, so the path goes first and "init" nowhere */
			argv[0] = (char *)path;
			cmd_init(NULL, 1, argv);
			if (repo_open(&other, path) < 0) {
				gp_error("cannot create a repository at %s", path);
				return;
			}
		}
		copy_all_objects(&r->odb, &other.odb, &copied, &skipped);
		repo_release(&other);
		return;
	}
	/* the carrier case is handled by the push command, which drives git
	 * directly so it can pass the refspec through */
}

/* ------------------------------------------------------------------ */
/* the remote command                                                  */

int cmd_remote(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct remote_list rl = REMOTE_LIST_INIT;
	const char *sub;
	int verbose;

	opts_init(&o, argc, argv, NULL, 0);
	verbose = opts_flag(&o, "-v") || opts_flag(&o, "--verbose");
	sub = opts_arg(&o, 0);

	if (!sub) {
		size_t i;
		remote_list_load(r, &rl);
		for (i = 0; i < rl.nr; i++) {
			if (verbose)
				printf("%s\t%s (fetch)\n%s\t%s (push)\n",
				       rl.e[i].name, rl.e[i].url, rl.e[i].name,
				       rl.e[i].url);
			else
				printf("%s\n", rl.e[i].name);
		}
		remote_list_release(&rl);
		return 0;
	}

	if (!strcmp(sub, "add")) {
		const char *name = opts_arg(&o, 1);
		const char *url = opts_arg(&o, 2);
		char *existing = NULL;
		if (!name || !url)
			gp_die("remote add: expected <name> <url>");
		if (remote_get_url(r, name, &existing) == 0) {
			free(existing);
			gp_die("remote %s already exists", name);
		}
		remote_add(r, name, url);
		printf("added remote %s -> %s\n", name, url);
		return 0;
	}
	if (!strcmp(sub, "remove") || !strcmp(sub, "rm")) {
		const char *name = opts_arg(&o, 1);
		if (!name)
			gp_die("remote remove: expected a name");
		remote_remove(r, name);
		return 0;
	}
	if (!strcmp(sub, "set-url")) {
		const char *name = opts_arg(&o, 1);
		const char *url = opts_arg(&o, 2);
		if (!name || !url)
			gp_die("remote set-url: expected <name> <url>");
		remote_set_url(r, name, url);
		return 0;
	}
	if (!strcmp(sub, "rename")) {
		const char *from = opts_arg(&o, 1);
		const char *to = opts_arg(&o, 2);
		if (!from || !to)
			gp_die("remote rename: expected <old> <new>");
		remote_rename(r, from, to);
		return 0;
	}
	if (!strcmp(sub, "show")) {
		const char *name = opts_arg(&o, 1);
		char *url = NULL;
		if (!name)
			gp_die("remote show: expected a name");
		if (remote_get_url(r, name, &url) < 0)
			gp_die("No such remote '%s'", name);
		printf("* remote %s\n  Fetch URL: %s\n  Push  URL: %s\n", name, url,
		       url);
		free(url);
		return 0;
	}
	if (!strcmp(sub, "get-url")) {
		const char *name = opts_arg(&o, 1);
		char *url = NULL;
		if (!name)
			gp_die("remote get-url: expected a name");
		if (remote_get_url(r, name, &url) < 0)
			gp_die("No such remote '%s'", name);
		printf("%s\n", url);
		free(url);
		return 0;
	}

	gp_die("remote: unknown subcommand '%s'", sub);
	return 1;
}

/* ------------------------------------------------------------------ */
/* shared push/fetch plumbing                                          */

static int resolve_remote(struct repo *r, const char *given, char **name,
			  char **url)
{
	struct remote_list rl = REMOTE_LIST_INIT;
	int rc = -1;

	remote_list_load(r, &rl);

	if (given) {
		if (remote_get_url(r, given, url) == 0) {
			*name = xstrdup(given);
			rc = 0;
		} else if (!rl.nr) {
			/* not a configured remote: treat it as a url */
			*name = xstrdup("origin");
			*url = xstrdup(given);
			rc = 0;
		}
	} else if (rl.nr == 1) {
		*name = xstrdup(rl.e[0].name);
		*url = xstrdup(rl.e[0].url);
		rc = 0;
	} else if (remote_get_url(r, "origin", url) == 0) {
		*name = xstrdup("origin");
		rc = 0;
	}

	remote_list_release(&rl);
	return rc;
}

/* one parsed refspec: "src:dst", "src", or ":dst" */
struct refspec {
	const char *src;
	const char *dst;
};

static struct refspec parse_refspec(const char *s)
{
	struct refspec r;
	const char *colon = strchr(s, ':');
	if (colon) {
		r.src = xstrndup(s, (size_t)(colon - s));
		r.dst = colon + 1;
	} else {
		r.src = xstrdup(s);
		r.dst = NULL;
	}
	return r;
}

static char *refspec_src_full(struct refspec *rs)
{
	if (!rs->src[0])
		return NULL;
	if (!strncmp(rs->src, "refs/", 5))
		return xstrdup(rs->src);
	return xstrfmt("refs/heads/%s", rs->src);
}

static char *refspec_dst_full(struct refspec *rs)
{
	const char *d = rs->dst ? rs->dst : rs->src;
	if (!d || !d[0])
		return NULL;
	if (!strncmp(d, "refs/", 5))
		return xstrdup(d);
	return xstrfmt("refs/heads/%s", d);
}

/* ------------------------------------------------------------------ */
/* push                                                                */

/* one ref to be pushed, resolved and ready */
struct push_ref {
	char *src_ref;   /* refs/heads/main, local */
	char *dst_ref;   /* refs/heads/main, on the remote */
	oid_t oid;
};

struct push_list {
	struct push_ref *e;
	size_t nr, alloc;
};

static void push_list_release(struct push_list *l)
{
	size_t i;
	for (i = 0; i < l->nr; i++) {
		free(l->e[i].src_ref);
		free(l->e[i].dst_ref);
	}
	free(l->e);
	l->e = NULL;
	l->nr = l->alloc = 0;
}

static void push_list_push(struct push_list *l, const char *src, const char *dst,
			   const oid_t *oid)
{
	if (l->nr == l->alloc) {
		l->alloc = l->alloc ? l->alloc * 2 : 4;
		l->e = xrealloc(l->e, l->alloc * sizeof(*l->e));
	}
	l->e[l->nr].src_ref = xstrdup(src);
	l->e[l->nr].dst_ref = xstrdup(dst);
	l->e[l->nr].oid = *oid;
	l->nr++;
}

/*
 * `push --tags` means every tag, which a wildcard refspec cannot express: a
 * refspec carries one oid, and a wildcard names no single one, so the tags
 * are enumerated instead.  Ignoring the flag was the alternative, and it is
 * worse -- pushing a tag that never arrives looks like success.
 */
static void push_tag_cb(const char *name, const oid_t *oid, void *ud)
{
	push_list_push(ud, name, name, oid);
}

int cmd_push(struct repo *r, int argc, char **argv)
{
	struct opts o;
	char *name = NULL, *url = NULL;
	const char *given = NULL;
	struct push_list refs = { NULL, 0, 0 };
	int set_upstream, force;
	size_t i;
	int rc = 0;

	opts_init(&o, argc, argv, NULL, 0);
	set_upstream = opts_flag(&o, "-u") || opts_flag(&o, "--set-upstream");
	force = opts_flag(&o, "-f") || opts_flag(&o, "--force");

	given = opts_arg(&o, 0);
	if (resolve_remote(r, given, &name, &url) < 0) {
		gp_error("push: no remote given and none configured\n"
			 "hint: gitprompt remote add origin <url>");
		return 1;
	}

	/* refspecs after the remote; the current branch if none were given */
	if (opts_arg(&o, 1)) {
		int k;
		for (k = 1; opts_arg(&o, k); k++) {
			struct refspec rs = parse_refspec(opts_arg(&o, k));
			char *src = refspec_src_full(&rs);
			char *dst = refspec_dst_full(&rs);
			oid_t oid;

			if (!src || !dst) {
				gp_error("push: cannot parse refspec '%s'",
					 opts_arg(&o, k));
				free(src);
				free(dst);
				free((char *)rs.src);
				rc = 1;
				goto out;
			}
			if (refs_read(&r->refs, src, &oid) < 0) {
				gp_error("push: '%s' does not name a ref",
					 opts_arg(&o, k));
				free(src);
				free(dst);
				free((char *)rs.src);
				rc = 1;
				goto out;
			}
			push_list_push(&refs, src, dst, &oid);
			free(src);
			free(dst);
			free((char *)rs.src);
		}
	} else {
		char *t = refs_head_target(&r->refs);
		oid_t oid;
		char *dst, *src;

		if (!t || strncmp(t, "refs/heads/", 11)) {
			gp_error("push: HEAD is detached; name a refspec explicitly");
			free(t);
			rc = 1;
			goto out;
		}
		src = xstrdup(t);
		dst = xstrdup(t);
		free(t);
		if (refs_read(&r->refs, src, &oid) < 0) {
			gp_error("push: the current branch has no commits yet");
			free(src);
			free(dst);
			rc = 1;
			goto out;
		}
		push_list_push(&refs, src, dst, &oid);
		free(src);
		free(dst);
	}

	/* in addition to whatever was named, never instead of it */
	if (opts_flag(&o, "--tags"))
		refs_list(&r->refs, "refs/tags/", push_tag_cb, &refs);

	/* carriers: git does the wire work over our object store */
	if (!url_is_local(url) && !url_is_gp(url)) {
		struct buf out = BUF_INIT;
		const char **args;
		size_t n = 0;

		if (!git_available()) {
			gp_error("push: %s needs the git binary on PATH to speak "
				 "the wire protocol", url);
			rc = 1;
			goto out;
		}
		args = xmalloc((refs.nr + 3) * sizeof(*args));
		args[n++] = "push";
		args[n++] = url;
		for (i = 0; i < refs.nr; i++) {
			/* rebuild each spec without the trailing space */
			char *one = xstrfmt("%s:%s", refs.e[i].src_ref,
					    refs.e[i].dst_ref);
			args[n++] = one;
		}
		args[n] = NULL;

		rc = run_git(r->gpdir, args, n, &out);
		for (i = 0; i < refs.nr; i++)
			free((char *)args[2 + i]);
		free(args);

		if (rc != 0) {
			gp_error("push failed:\n%s", buf_cstr(&out));
			buf_release(&out);
			rc = 1;
			goto out;
		}
		fwrite(out.b, 1, out.len, stdout);
		buf_release(&out);
		printf("To %s\n", url);
		for (i = 0; i < refs.nr; i++)
			printf("   %s -> %s\n", refs.e[i].src_ref,
			       refs.e[i].dst_ref);
		rc = 0;
	} else {
		struct oid_array haves = OID_ARRAY_INIT, wants = OID_ARRAY_INIT;
		struct fetch_collect fc;
		struct repo other;
		size_t k;
		int opened;

		memset(&fc, 0, sizeof fc);
		for (i = 0; i < refs.nr; i++)
			oid_array_append(&wants, &refs.e[i].oid);

		remote_read_refs(r, url, name, fetch_collect_cb, &fc);
		for (k = 0; k < fc.nr; k++)
			oid_array_append(&haves, &fc.e[k].oid);

		remote_push_objects(r, url, name, &haves, &wants);

		memset(&other, 0, sizeof other);
		opened = repo_open(&other, url_without_file_scheme(url)) == 0;
		if (!opened) {
			gp_error("push: cannot write to %s", url);
			oid_array_clear(&haves);
			oid_array_clear(&wants);
			fetch_collect_release(&fc);
			rc = 1;
			goto out;
		}

		printf("To %s\n", url);
		for (i = 0; i < refs.nr; i++) {
			oid_t before = null_oid;
			int had = refs_read(&other.refs, refs.e[i].dst_ref,
					    &before) == 0;
			const char *shortname = refs.e[i].dst_ref;
			int ff;

			if (!strncmp(shortname, "refs/heads/", 11))
				shortname += 11;

			/*
			 * Refuse to move a branch backwards unless asked.  A push
			 * that rewinds a ref throws away commits, and the other
			 * side may never have seen them -- this check exists
			 * because the first version of push would do exactly
			 * that, quietly.
			 */
			ff = !had || oid_equal(&before, &refs.e[i].oid) ||
			     is_ancestor(&other, &before, &refs.e[i].oid);
			if (!ff && !force) {
				gp_error("non-fast-forward: %s would move %s "
					 "backwards, losing %s\n"
					 "hint: fetch and merge first, or repeat "
					 "the push with --force",
					 shortname, refs.e[i].dst_ref,
					 abbrev_oid(&before));
				rc = 1;
				continue;
			}

			refs_write(&other.refs, refs.e[i].dst_ref, &refs.e[i].oid);
			refs_reflog(&other.refs, refs.e[i].dst_ref,
				    had ? &before : &null_oid, &refs.e[i].oid,
				    force && !ff ? "push --force" : "push");

			if (!had)
				printf(" * [new branch]      %s -> %s\n", shortname,
				       shortname);
			else if (oid_equal(&before, &refs.e[i].oid))
				printf(" = [up to date]      %s -> %s\n", shortname,
				       shortname);
			else if (!ff)
				printf(" + %s...%s  %s -> %s (forced update)\n",
				       abbrev_oid(&before),
				       abbrev_oid(&refs.e[i].oid), shortname,
				       shortname);
			else
				printf("   %s..%s  %s -> %s\n",
				       abbrev_oid(&before),
				       abbrev_oid(&refs.e[i].oid), shortname,
				       shortname);
		}

		oid_array_clear(&haves);
		oid_array_clear(&wants);
		fetch_collect_release(&fc);
		repo_release(&other);
	}

	/*
	 * The remote-tracking refs follow.  A pushed branch gets one; a pushed
	 * tag does not, because the value just pushed is already in the local
	 * tag -- there is nothing for a tracking ref to add.
	 */
	for (i = 0; i < refs.nr; i++) {
		if (strncmp(refs.e[i].dst_ref, "refs/heads/", 11))
			continue;
		remote_update_local_ref(r, name, refs.e[i].dst_ref, &refs.e[i].oid);
	}

	if (set_upstream && refs.nr == 1) {
		const char *shortname = refs.e[0].src_ref;
		char *k, *m, *merge;
		if (!strncmp(shortname, "refs/heads/", 11))
			shortname += 11;
		k = xstrfmt("branch.%s.remote", shortname);
		m = xstrfmt("branch.%s.merge", shortname);
		merge = xstrdup(refs.e[0].src_ref);
		repo_config_set(r, k, name, 0);
		repo_config_set(r, m, merge, 0);
		printf("branch '%s' set up to track '%s/%s'.\n", shortname, name,
		       shortname);
		free(k);
		free(m);
		free(merge);
	}

out:
	push_list_release(&refs);
	free(name);
	free(url);
	return rc;
}

/* ------------------------------------------------------------------ */
/* fetch                                                              */

int cmd_fetch(struct repo *r, int argc, char **argv)
{
	static const char *const takes[] = { "--depth" };
	struct opts o;
	char *name = NULL, *url = NULL;
	struct fetch_collect fc;
	size_t i;
	int updated = 0;

	opts_init(&o, argc, argv, takes, 1);
	if (resolve_remote(r, opts_arg(&o, 0), &name, &url) < 0) {
		gp_error("fetch: no remote given and none configured\n"
			 "hint: gitprompt remote add origin <url>");
		return 1;
	}

	memset(&fc, 0, sizeof fc);
	remote_read_refs(r, url, name, fetch_collect_cb, &fc);
	if (!fc.nr) {
		gp_error("fetch: %s reported no refs", url);
		fetch_collect_release(&fc);
		free(name);
		free(url);
		return 1;
	}

	{
		struct oid_array wants = OID_ARRAY_INIT;
		for (i = 0; i < fc.nr; i++)
			oid_array_append(&wants, &fc.e[i].oid);
		remote_fetch_objects(r, url, name, &wants);
		oid_array_clear(&wants);
	}

	printf("From %s\n", url);
	for (i = 0; i < fc.nr; i++) {
		const char *refname = fc.e[i].refname;
		int is_tag = !strncmp(refname, "refs/tags/", 10);
		const char *shortname = is_tag ? refname + 10
				      : !strncmp(refname, "refs/heads/", 11)
						? refname + 11
						: refname;
		char *ref = remote_tracking_ref(name, refname);
		char label[32];
		const char *shown = ref;
		oid_t before = null_oid;
		int had = refs_read(&r->refs, ref, &before) == 0;

		/* what the line names is the destination as a reader would write
		 * it: a branch tracks under the remote, a tag is just its name */
		if (!strncmp(shown, "refs/remotes/", 13))
			shown += 13;
		else if (!strncmp(shown, "refs/tags/", 10))
			shown += 10;

		if (!had) {
			snprintf(label, sizeof label, " * [new %s]",
				 is_tag ? "tag" : "branch");
			printf("%-21s%s -> %s\n", label, shortname, shown);
			updated = 1;
		} else if (!oid_equal(&before, &fc.e[i].oid)) {
			printf("   %s..%s  %s -> %s\n", abbrev_oid(&before),
			       abbrev_oid(&fc.e[i].oid), shortname, shown);
			updated = 1;
		}

		remote_update_local_ref(r, name, refname, &fc.e[i].oid);
		refs_reflog(&r->refs, ref, had ? &before : &null_oid,
			    &fc.e[i].oid, "fetch");
		free(ref);
	}

	if (!updated)
		printf("   (everything up to date)\n");

	fetch_collect_release(&fc);
	free(name);
	free(url);
	return 0;
}

/* ------------------------------------------------------------------ */
/* clone                                                              */

static const char *url_basename(const char *url)
{
	const char *base = strrchr(url, '/');
	if (!base)
		return url;
	return base + 1;
}

/* the branch a remote's HEAD points at, or NULL */
static char *remote_head_branch(struct repo *r, const char *url,
				const char *name)
{
	char *fallback = NULL;

	if (url_is_local(url)) {
		struct repo other;
		char *t;
		memset(&other, 0, sizeof other);
		if (repo_open(&other, url_without_file_scheme(url)) == 0) {
			t = refs_head_target(&other.refs);
			if (t && !strncmp(t, "refs/heads/", 11))
				fallback = xstrdup(t + 11);
			free(t);
			repo_release(&other);
		}
	} else if (git_available()) {
		struct buf out = BUF_INIT;
		const char *args[] = { "ls-remote", "--symref", url, "HEAD" };
		if (run_git(NULL, args, 4, &out) == 0) {
			const char *p = strstr((const char *)out.b, "ref: refs/heads/");
			if (p) {
				/* strlen("ref: refs/heads/"), and one past it is
				 * the first character of the branch name; counting
				 * to 17 here loses the leading letter and the
				 * clone then checks nothing out. */
				const char *start = p + strlen("ref: refs/heads/");
				const char *end = start;
				while (*end && *end != '\t' && *end != '\n')
					end++;
				fallback = xstrndup(start, (size_t)(end - start));
			}
		}
		buf_release(&out);
	}
	(void)r;
	(void)name;
	return fallback;
}

int cmd_clone(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *url, *dir;
	char *name = NULL, *how = NULL;
	char *branch = NULL;
	struct repo local;
	int rc = 0;

	(void)r;
	opts_init(&o, argc, argv, NULL, 0);
	url = opts_arg(&o, 0);
	if (!url) {
		gp_error("clone: expected a url\nusage: gitprompt clone <url> [<dir>]");
		return 1;
	}
	dir = opts_arg(&o, 1);
	if (!dir) {
		char *base = xstrdup(url_basename(url));
		size_t n = strlen(base);
		if (n > 4 && !strcmp(base + n - 4, ".git"))
			base[n - 4] = '\0';
		dir = base;
	} else {
		(void)0;
	}

	{
		/* init, so the new repository is ours from the start */
		char *a[1];
		a[0] = (char *)dir;
		cmd_init(NULL, 1, a);
	}

	memset(&local, 0, sizeof local);
	if (repo_open(&local, dir) < 0) {
		gp_error("clone: cannot open the new repository at %s", dir);
		if (!opts_arg(&o, 1))
			free((char *)dir);
		return 1;
	}

	{
		struct buf id;
		buf_init(&id);
		/* the identity a reflog line carries, for the entry the clone
		 * leaves behind itself -- repo_release frees it */
		repo_ident_with_time(&local, &id);
		local.refs.ident = xstrdup(buf_cstr(&id));
		buf_release(&id);
	}

	name = xstrdup("origin");
	how = xstrdup(url);

	remote_add(&local, name, how);

	if (!url_is_local(how) && !url_is_gp(how)) {
		struct buf out = BUF_INIT;
		const char *args[4];
		if (!git_available()) {
			gp_error("clone: %s needs the git binary on PATH", how);
			rc = 1;
			goto done;
		}
		args[0] = "fetch";
		args[1] = how;
		args[2] = "+refs/heads/*:refs/remotes/origin/*";
		args[3] = NULL;
		printf("Cloning from %s ...\n", how);
		if (run_git(local.gpdir, args, 3, &out) != 0) {
			gp_error("clone: fetch failed:\n%s", buf_cstr(&out));
			buf_release(&out);
			rc = 1;
			goto done;
		}
		buf_release(&out);
	} else {
		struct fetch_collect fc;
		size_t i;
		struct oid_array wants = OID_ARRAY_INIT;

		memset(&fc, 0, sizeof fc);
		remote_read_refs(&local, how, name, fetch_collect_cb, &fc);
		for (i = 0; i < fc.nr; i++)
			oid_array_append(&wants, &fc.e[i].oid);
		printf("Cloning from %s ...\n", how);
		remote_fetch_objects(&local, how, name, &wants);
		for (i = 0; i < fc.nr; i++)
			remote_update_local_ref(&local, name, fc.e[i].refname,
						&fc.e[i].oid);
		oid_array_clear(&wants);
		fetch_collect_release(&fc);
	}

	branch = remote_head_branch(&local, how, name);

	/* fall back to the usual suspects when the remote's HEAD is unknown */
	if (!branch) {
		oid_t probe;
		char *cand[] = { "main", "master" };
		size_t i;
		for (i = 0; i < 2; i++) {
			char *ref = xstrfmt("refs/remotes/%s/%s", name, cand[i]);
			if (refs_read(&local.refs, ref, &probe) == 0)
				branch = xstrdup(cand[i]);
			free(ref);
			if (branch)
				break;
		}
	}

	if (branch) {
		oid_t oid;
		char *remote_ref = xstrfmt("refs/remotes/%s/%s", name, branch);
		if (refs_read(&local.refs, remote_ref, &oid) == 0) {
			char *head_ref = xstrfmt("refs/heads/%s", branch);
			struct commit c = COMMIT_INIT;
			oid_t tree;

			refs_write(&local.refs, head_ref, &oid);
			/* git's clone leaves a reflog behind, and without
			 * one `gitprompt reflog` in a fresh clone fails on a
			 * branch that the clone itself just created. */
			{
				char *msg = xstrfmt("clone: from %s", how);
				refs_reflog(&local.refs, head_ref, &null_oid, &oid,
					    msg);
				free(msg);
			}
			refs_set_head(&local.refs, head_ref);
			read_commit(&local, &oid, &c);
			tree = c.tree;
			commit_release(&c);
			checkout_tree(&local, &tree, 1, 1);
			free(head_ref);
		} else {
			/* the branch was named but never arrived: saying
			 * "done." here would hand back an empty checkout */
			gp_error("clone: fetched no %s, so there is nothing to "
				 "check out", remote_ref);
			rc = 1;
		}
		free(remote_ref);
	} else {
		gp_warn("clone: the remote has no branches to check out");
	}

	{
		char *k = xstrfmt("branch.%s.remote", branch ? branch : "main");
		char *m = xstrfmt("branch.%s.merge", branch ? branch : "main");
		char *merge = xstrfmt("refs/heads/%s", branch ? branch : "main");
		repo_config_set(&local, k, name, 0);
		repo_config_set(&local, m, merge, 0);
		free(k);
		free(m);
		free(merge);
	}

	printf("done.\n");

done:
	free(branch);
	free(name);
	free(how);
	repo_release(&local);
	if (!opts_arg(&o, 1))
		free((char *)dir);
	return rc;
}

/* ------------------------------------------------------------------ */
/* pull                                                               */

int cmd_pull(struct repo *r, int argc, char **argv)
{
	struct opts o;
	char *name = NULL, *url = NULL;
	const char *remote_arg, *branch_arg;
	struct fetch_collect fc;
	size_t i;
	oid_t fetched = null_oid;
	int got = 0;

	opts_init(&o, argc, argv, NULL, 0);
	remote_arg = opts_arg(&o, 0);
	branch_arg = opts_arg(&o, 1);

	if (resolve_remote(r, remote_arg, &name, &url) < 0) {
		gp_error("pull: no remote given and none configured");
		return 1;
	}

	memset(&fc, 0, sizeof fc);
	remote_read_refs(r, url, name, fetch_collect_cb, &fc);

	{
		struct oid_array wants = OID_ARRAY_INIT;
		for (i = 0; i < fc.nr; i++)
			oid_array_append(&wants, &fc.e[i].oid);
		remote_fetch_objects(r, url, name, &wants);
		oid_array_clear(&wants);
	}

	/* which branch did we pull? */
	{
		char *wanted = NULL;
		if (branch_arg)
			wanted = xstrdup(branch_arg);
		else {
			char *t = refs_head_target(&r->refs);
			if (t && !strncmp(t, "refs/heads/", 11))
				wanted = xstrdup(t + 11);
			free(t);
		}

		for (i = 0; i < fc.nr; i++) {
			const char *shortname = fc.e[i].refname;
			if (!strncmp(shortname, "refs/heads/", 11))
				shortname += 11;
			if (wanted && strcmp(shortname, wanted))
				continue;
			remote_update_local_ref(r, name, fc.e[i].refname, &fc.e[i].oid);
			refs_reflog(&r->refs, fc.e[i].refname, &null_oid, &fc.e[i].oid,
				    "pull");
			fetched = fc.e[i].oid;
			got = 1;
			break;
		}
		free(wanted);
	}

	if (!got) {
		gp_error("pull: nothing to merge from %s", url);
		fetch_collect_release(&fc);
		free(name);
		free(url);
		return 1;
	}

	/* merge it, the same way `merge` would.  A command takes its own
	 * arguments only -- the dispatcher has already stripped the command
	 * name -- so this is the id alone, not "merge" followed by the id. */
	{
		char hex[GP_SHA1_HEXSZ + 1];
		char *argv2[1];
		oid_hex(&fetched, hex);
		argv2[0] = hex;
		printf("From %s\n", url);
		fetch_collect_release(&fc);
		free(name);
		free(url);
		return cmd_merge(r, 1, argv2);
	}
}

/* ------------------------------------------------------------------ */
/* serve                                                              */

int cmd_serve(struct repo *r, int argc, char **argv)
{
	static const char *const takes[] = { "--port", "--dir" };
	struct opts o;

	(void)r;
	opts_init(&o, argc, argv, takes, 2);

	gp_error("serve: gitprompt's own transport (gp://) is not implemented in "
		 "this build.\n"
		 "The local and carrier transports are: point a remote at a "
		 "path or at an https:// repository.");
	return 1;
}
