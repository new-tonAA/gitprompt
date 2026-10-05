/*
 * submodule: a repository inside a repository.
 *
 * A submodule is two things that have to agree.  In the tree that holds it,
 * the path is a single entry of mode 160000 whose id is a commit -- a
 * "gitlink" -- and `.gitmodules` at the root says where that commit comes
 * from.  In the working directory, the same path is a checkout of that commit,
 * made by a repository of its own whose store lives under the parent's
 * `modules/`.
 *
 * That second half is why this command is built on the same machinery as
 * `worktree`: a `.git` file naming a store somewhere else is a form this tool
 * already reads, and the directory holding the file is taken as the work tree.
 * The difference is that a linked worktree's store shares the parent's objects
 * and refs, and a submodule's does not -- it is a repository in its own right,
 * which is what makes it a submodule rather than a second place to work.
 *
 * What is here: add, status, init, update, sync, deinit and foreach.  What is
 * not: `--recursive`, `absorbgitdirs`, `.gitmodules` merges and `summary`, each
 * of which is refused rather than guessed at.  The reading side of `status` --
 * whether a submodule's own working tree has moved on -- is not looked at
 * either, since that is a question for the submodule's repository and this one
 * does not open it.
 */

#include "gp.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define sm_chdir  _chdir
#define sm_getcwd _getcwd
#else
#include <unistd.h>
#include <sys/wait.h>
#define sm_chdir  chdir
#define sm_getcwd getcwd
#endif

/* the same spelling of this that odb.c and json.c use, for a file's name */
#ifdef _WIN32
#include <process.h>
#define gp_getpid() ((long)_getpid())
#else
#define gp_getpid() ((long)getpid())
#endif

/* system() reports a wait status on Unix and the exit code itself on Windows */
static int sm_wait_status(int raw)
{
#ifdef _WIN32
	return raw;
#else
	if (raw < 0)
		return -1;
	return WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
#endif
}

/* ------------------------------------------------------------------ */
/* where things are                                                    */

/* the store of the submodule called `name` -- beside objects and refs, under
 * `modules/`, which is where git keeps an absorbed submodule's too */
static char *sm_store(struct repo *r, const char *name)
{
	return xstrfmt("%s/modules/%s", r->gpdir, name);
}

static char *sm_gitmodules(struct repo *r)
{
	return xstrfmt("%s/.gitmodules", r->root);
}

/*
 * One field of one entry of `.gitmodules`, or NULL.  The file is read with the
 * same reader the repository's own configuration is read with, so a header
 * that this writes and a header git writes are read back the same way.
 */
static char *sm_entry(struct repo *r, const char *name, const char *field)
{
	char *file = sm_gitmodules(r);
	char *key = xstrfmt("submodule.%s.%s", name, field);
	char *out = NULL;

	config_file_get(file, key, &out);
	free(key);
	free(file);
	return out;
}

/*
 * The names of the submodules `.gitmodules` names, in the order they appear.
 * A submodule is a subsection of `[submodule "..."]` there, which is why this
 * asks for the subsections rather than for a key.
 */
static struct slist *sm_names(struct repo *r)
{
	char *file = sm_gitmodules(r);
	struct slist *l = config_file_subsections(file, "submodule");

	free(file);
	return l;
}

/* the last component of a path, which is what names a submodule */
static char *sm_basename(const char *path)
{
	const char *slash = strrchr(path, '/');

	return xstrdup(slash ? slash + 1 : path);
}

/*
 * The path a new submodule should take when the command did not name one: the
 * last component of the url, with a `.git` suffix taken off.  git reads it the
 * same way.
 */
static char *sm_path_from_url(const char *url)
{
	size_t n;
	char *base = sm_basename(url);

	n = strlen(base);
	if (n > 4 && !strcmp(base + n - 4, ".git"))
		base[n - 4] = '\0';
	return base;
}

/*
 * Whether a path is fit to be a submodule's: relative to the root, with no
 * step that climbs out of it.  A submodule is a directory of this work tree, so
 * a path that reaches outside would put one repository's files in another's.
 */
static int sm_path_ok(const char *path)
{
	const char *p = path;

	if (!path[0] || path[0] == '/' || path[0] == '\\')
		return 0;
#ifdef _WIN32
	if (path[1] == ':')
		return 0;
#endif
	while (*p) {
		if (!strncmp(p, "../", 3) || !strcmp(p, ".."))
			return 0;
		if (p[0] == '/' && p[1] == '/')
			return 0;
		p++;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* reading an entry's recorded commit                                  */

/*
 * The commit the index records for `path`, or NULL when the path is not in the
 * index or is in it as something other than a gitlink.  This is the id a
 * `status` line and an `update` both start from.
 */
static char *sm_recorded(struct repo *r, const char *path)
{
	struct index_state ist;
	const struct index_entry *e;
	char *hex = NULL;

	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));
	e = index_get(&ist, path);
	if (e && (e->mode & 0170000) == 0160000) {
		hex = xmalloc(GP_SHA1_HEXSZ + 1);
		oid_hex(&e->oid, hex);
	}
	index_release(&ist);
	return hex;
}

/*
 * The repository a checked-out submodule is, opened from its own directory.
 * The `.git` file there names the store and the directory holding the file is
 * the work tree, which is the same discovery any command run inside a
 * submodule would do.  Returns 0 and fills `sub`, or -1 with a message.
 */
static int sm_open(struct repo *sub, const char *path)
{
	memset(sub, 0, sizeof *sub);
	if (repo_open(sub, path) < 0) {
		gp_error("cannot open the submodule at %s", path);
		return -1;
	}
	return 0;
}

/* the commit a checked-out submodule stands on, or NULL */
static char *sm_head_of(struct repo *sub)
{
	oid_t oid;
	char *hex;

	if (refs_head(&sub->refs, &oid) < 0)
		return NULL;
	hex = xmalloc(GP_SHA1_HEXSZ + 1);
	oid_hex(&oid, hex);
	return hex;
}

/* ------------------------------------------------------------------ */
/* the store and the .git file                                         */

/*
 * Put a submodule's store and its `.git` file in place -- making what is not
 * there yet -- and open the repository the two describe.  `made` is set when
 * the store was created here, so a caller that fails further on can take back
 * what this made rather than leaving it behind.
 *
 * Both halves have to be made before anything is opened: the store is what a
 * submodule keeps in, and the `.git` file is the only way in, so a repository
 * opened any other way would be a second entrance that nothing else uses.
 */
static int sm_prepare(struct repo *r, const char *name, const char *path,
		      struct repo *sub, int *made)
{
	char *store = sm_store(r, name);
	char *abs_path = xstrfmt("%s/%s", r->root, path);
	char *gitfile = NULL, *probe;
	int rc = -1;

	*made = 0;

	/*
	 * A store already there and holding a repository is left as it is: git
	 * reactivates a local directory rather than making a second one, and
	 * re-initialising would throw away whatever is in it.
	 */
	probe = xstrfmt("%s/HEAD", store);
	if (is_file(probe)) {
		printf("Reactivating local git directory for '%s'.\n", name);
	} else {
		char *args[3];

		args[0] = "--bare";
		args[1] = "-q";
		args[2] = store;
		if (cmd_init(NULL, 3, args) != 0) {
			free(probe);
			goto out;
		}
		*made = 1;
	}
	free(probe);

	mkdir_p(abs_path);
	gitfile = xstrfmt("%s/.git", abs_path);
	{
		/*
		 * The path is written the way every other stored path here is --
		 * absolute, with the separators made "/" and the "." and ".."
		 * folded out -- so that the store the file names is the same
		 * string however the repository was reached.
		 */
		char *clean = gp_clean_path(store);
		char *line = xstrfmt("gitdir: %s\n", clean);

		write_file(gitfile, line, strlen(line));
		free(line);
		free(clean);
	}

	if (sm_open(sub, abs_path) < 0)
		goto out;

	/*
	 * The store names the directory it is the store of.  Nothing here needs
	 * that -- a submodule is found from its own directory, through the .git
	 * file -- but git does, and a submodule a reader cannot see into is not
	 * the ordinary layout this is meant to write.
	 */
	{
		char *clean = gp_clean_path(abs_path);

		repo_config_set(sub, "core.worktree", clean, 0);
		free(clean);
	}

	rc = 0;
out:
	free(gitfile);
	free(abs_path);
	free(store);
	return rc;
}

/* ------------------------------------------------------------------ */
/* which submodules a command was pointed at                           */

/*
 * Whether `name` is one of the ones named after the subcommand.  With nothing
 * named, every submodule is meant, which is what git does too.
 */
static int sm_wanted(struct opts *o, const char *name)
{
	int i;

	if (!opts_arg(o, 1))
		return 1;
	for (i = 1; opts_arg(o, i); i++)
		if (!strcmp(opts_arg(o, i), name))
			return 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* add                                                                 */

/*
 * Put the submodule `url` at `path`: make the store, check the submodule out
 * there, and record the two halves in the parent -- the entry in the index and
 * the entry in `.gitmodules`.
 *
 * The order matters.  The store is made first, then the `.git` file, and only
 * then is the submodule opened -- through that file, which is how it will be
 * found from now on.
 */
static int sm_add(struct repo *r, struct opts *o)
{
	/* argv[0] is the subcommand itself, so the url is the next one along */
	const char *url = opts_arg(o, 1);
	char *path = NULL, *name = NULL, *existing = NULL, *head = NULL;
	struct repo sub;
	struct index_entry e;
	struct index_state ist;
	int rc = 1, made_store = 0;

	if (!url) {
		gp_error("submodule add: expected a url\n"
			 "usage: gitprompt submodule add <url> [<path>]");
		return 1;
	}

	path = opts_arg(o, 2) ? xstrdup(opts_arg(o, 2)) : sm_path_from_url(url);
	/* a trailing slash names the same directory, and the name has to be one */
	{
		size_t n = strlen(path);
		while (n > 1 && path[n - 1] == '/')
			path[--n] = '\0';
	}
	if (!strcmp(path, "."))
		gp_die("submodule add: '%s' is the repository itself", path);
	if (!sm_path_ok(path))
		gp_die("submodule add: '%s' is not a path inside the work tree",
		       path);

	/*
	 * The name of a submodule is its path, which is what git uses when
	 * nothing names it otherwise.  It is not the last component of the path:
	 * two submodules can sit at `libs/foo` and `other/foo`, and giving both
	 * the name `foo` would have them share a store.
	 */
	name = xstrdup(path);

	existing = sm_entry(r, name, "path");
	if (existing) {
		gp_error("'%s' already exists in .gitmodules", path);
		free(existing);
		goto out;
	}

	if (sm_prepare(r, name, path, &sub, &made_store) < 0)
		goto out;

	printf("Cloning into '%s'...\n", path);
	if (remote_clone_into(&sub, "origin", url) < 0) {
		repo_release(&sub);
		goto out;
	}
	head = sm_head_of(&sub);
	repo_release(&sub);

	if (!head) {
		gp_error("the submodule at '%s' has no commit to record", path);
		goto out;
	}

	/* .gitmodules first: the entry that says what this path is */
	{
		char *file = sm_gitmodules(r);
		char *key = xstrfmt("submodule.%s.path", name);

		config_file_set(file, key, path);
		free(key);
		key = xstrfmt("submodule.%s.url", name);
		config_file_set(file, key, url);
		free(key);
		free(file);
	}

	/*
	 * Then the index, which records both halves: `.gitmodules` as the file
	 * it now is, and the path as the commit the submodule stands on.
	 */
	if (stage_worktree_path(r, ".gitmodules") < 0) {
		gp_error("cannot stage .gitmodules");
		goto out;
	}

	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));
	memset(&e, 0, sizeof e);
	e.mode = MODE_GITLINK;
	oid_parse(&e.oid, head);
	e.path = path;
	{
		char *dir = xstrfmt("%s/%s", r->root, path);

		index_fill_stat(&e, dir);
		free(dir);
	}
	index_add(&ist, &e);
	index_write(&ist, repo_index_path(r));
	index_release(&ist);

	rc = 0;
out:
	if (rc && made_store && name) {
		/* nothing was recorded, so nothing should be left behind */
		char *store = sm_store(r, name);

		remove_dir_recursive(store);
		free(store);
	}
	free(head);
	free(existing);
	free(name);
	free(path);
	return rc;
}

/* ------------------------------------------------------------------ */
/* status                                                              */

static int sm_status(struct repo *r, struct opts *o)
{
	struct slist *names = sm_names(r);
	size_t i;

	(void)o;
	for (i = 0; i < names->nr; i++) {
		char *path = sm_entry(r, names->v[i], "path");
		const char *shown = path ? path : names->v[i];
		char *recorded = path ? sm_recorded(r, path) : NULL;
		char *gitfile = path ? xstrfmt("%s/%s/.git", r->root, path) : NULL;
		int present = gitfile && is_file(gitfile);

		/*
		 * The prefix is what the submodule's state is: `-` for one that
		 * has never been checked out, `+` for one checked out somewhere
		 * other than where the index says, and a bare space otherwise.
		 * The commit named is the recorded one either way, which is what
		 * git prints too.
		 */
		if (!present) {
			printf("-%s %s\n", recorded ? recorded : "0000000000000000000000000000000000000000",
			       shown);
		} else {
			struct repo sub;
			char *here = NULL;
			char *dir = xstrfmt("%s/%s", r->root, path);

			if (sm_open(&sub, dir) == 0) {
				here = sm_head_of(&sub);
				repo_release(&sub);
			}
			free(dir);

			if (here && recorded && strcmp(here, recorded))
				printf("+%s %s\n", recorded, shown);
			else
				printf(" %s %s\n",
				       recorded ? recorded
						: "0000000000000000000000000000000000000000",
				       shown);
			free(here);
		}
		free(gitfile);
		free(recorded);
		free(path);
	}
	slist_release(names);
	return 0;
}

/* ------------------------------------------------------------------ */
/* init                                                                */

/*
 * Copy what `.gitmodules` says about each submodule -- its url -- into this
 * repository's own configuration.  That is all `init` is: the file that is
 * committed says where a submodule comes from, and the local configuration is
 * what says that this checkout wants it.  `update` reads the local one back,
 * so a clone that has not run this yet has nothing pointing anywhere.
 */
static int sm_init(struct repo *r, struct opts *o)
{
	struct slist *names = sm_names(r);
	size_t i;

	for (i = 0; i < names->nr; i++) {
		const char *name = names->v[i];
		char *path = sm_entry(r, name, "path");
		char *url = sm_entry(r, name, "url");
		char *key;

		if (!sm_wanted(o, path ? path : name)) {
			free(path);
			free(url);
			continue;
		}
		if (!url) {
			gp_error("submodule '%s' has no url in .gitmodules", name);
			free(path);
			continue;
		}
		key = xstrfmt("submodule.%s.url", name);
		repo_config_set(r, key, url, 0);
		free(key);
		key = xstrfmt("submodule.%s.active", name);
		repo_config_set(r, key, "true", 0);
		free(key);
		printf("Submodule '%s' (%s) registered for path '%s'\n",
		       name, url, path ? path : name);
		free(path);
		free(url);
	}
	slist_release(names);
	return 0;
}

/* ------------------------------------------------------------------ */
/* update                                                              */

/*
 * The commit a submodule should stand on with `--remote`: the tip of the branch
 * it tracks, rather than the one the parent recorded.  Which branch that is
 * comes from `submodule.<name>.branch`, or from the branch the submodule is
 * itself on, and the usual two names stand in when neither says.
 */
static int sm_remote_target(struct repo *sub, struct repo *r, const char *name,
			    oid_t *out)
{
	char *branches[4];
	size_t i, n = 0;

	branches[n++] = sm_entry(r, name, "branch");
	{
		char *head = refs_head_target(&sub->refs);

		if (head) {
			if (!strncmp(head, "refs/heads/", 11))
				branches[n++] = xstrdup(head + 11);
			free(head);
		}
	}
	branches[n++] = xstrdup("main");
	branches[n++] = xstrdup("master");

	for (i = 0; i < n; i++) {
		char *ref;
		int ok;

		if (!branches[i])
			continue;
		ref = xstrfmt("refs/remotes/origin/%s", branches[i]);
		ok = refs_read(&sub->refs, ref, out) == 0;
		free(ref);
		if (ok) {
			while (n)
				free(branches[--n]);
			return 0;
		}
	}
	while (n)
		free(branches[--n]);
	return -1;
}

/*
 * Bring each submodule to the commit it should be on, and check that commit
 * out inside it -- in detached HEAD, which is what a submodule is, since the
 * parent is what decides where it stands rather than the submodule's own
 * branch.  A submodule that is not there yet is made and filled first.
 *
 * `--remote` takes the commit from the submodule's remote instead of from the
 * index, which is the newer one: the parent records a commit, and this asks
 * for wherever the branch has got to since.  A submodule that `init` has not
 * registered is skipped, unless `--init` says to register it here, which is the
 * same division of labour git has.
 */
static int sm_update(struct repo *r, struct opts *o)
{
	struct slist *names = sm_names(r);
	int want_init = opts_flag(o, "--init");
	int want_remote = opts_flag(o, "--remote");
	int force = opts_flag(o, "-f") || opts_flag(o, "--force");
	size_t i;
	int rc = 0;

	for (i = 0; i < names->nr; i++) {
		const char *name = names->v[i];
		char *path = sm_entry(r, name, "path");
		char *url = sm_entry(r, name, "url");
		char *dir = NULL, *gitfile = NULL, *recorded = NULL;
		struct repo sub;
		oid_t target;
		struct commit c = COMMIT_INIT;
		int made = 0, fresh = 0;

		if (!sm_wanted(o, path ? path : name))
			goto next;
		if (!path || !url) {
			gp_error("submodule '%s' is incomplete in .gitmodules",
				 name);
			rc = 1;
			goto next;
		}

		/*
		 * `init` is the step that says this checkout wants a submodule;
		 * without it, one that was never registered is left alone, which
		 * is the line git draws here too.
		 */
		{
			char *key = xstrfmt("submodule.%s.url", name);
			int registered = repo_config_get(r, key, NULL) == 0;

			if (want_init)
				repo_config_set(r, key, url, 0);
			free(key);
			if (!registered && !want_init)
				goto next;
		}

		dir = xstrfmt("%s/%s", r->root, path);
		gitfile = xstrfmt("%s/.git", dir);

		if (!is_file(gitfile)) {
			if (sm_prepare(r, name, path, &sub, &made) < 0) {
				rc = 1;
				goto next;
			}
			/* there was no work tree here to keep, so there is
			 * nothing in one to check for changes */
			fresh = 1;
		} else if (sm_open(&sub, dir) < 0) {
			rc = 1;
			goto next;
		}

		remote_add(&sub, "origin", url);
		if (remote_fetch_all(&sub, "origin", url) < 0) {
			rc = 1;
			repo_release(&sub);
			goto undo;
		}

		if (want_remote) {
			if (sm_remote_target(&sub, r, name, &target) < 0) {
				gp_error("submodule '%s': no remote branch to follow",
					 name);
				rc = 1;
				repo_release(&sub);
				goto undo;
			}
		} else {
			recorded = sm_recorded(r, path);
			if (!recorded) {
				gp_error("submodule '%s': nothing recorded for '%s' in the index",
					 name, path);
				rc = 1;
				repo_release(&sub);
				goto undo;
			}
			oid_parse(&target, recorded);
		}

		/*
		 * The submodule's own work tree is its own business, but a
		 * checkout that throws away what is in it is not something to do
		 * quietly, which is the same line git draws -- and one that was
		 * made here just now has nothing in it to throw away.
		 */
		if (!fresh && !force) {
			char **dirty = NULL;
			int ndirty = worktree_dirty_paths(&sub, &dirty);

			path_list_free(dirty);
			if (ndirty > 0) {
				gp_error("submodule '%s' has local changes; not checking out",
					 name);
				rc = 1;
				repo_release(&sub);
				goto undo;
			}
		}

		{
			struct buf raw = BUF_INIT;

			if (odb_read(&sub.odb, &target, NULL, &raw) < 0) {
				gp_error("submodule '%s' does not have %s", name,
					 recorded ? recorded : "the commit it should be on");
				buf_release(&raw);
				rc = 1;
				repo_release(&sub);
				goto undo;
			}
			commit_parse(&c, raw.b, raw.len);
			buf_release(&raw);
		}

		refs_set_head_detached(&sub.refs, &target);
		checkout_tree(&sub, &c.tree, 1, 1);
		{
			char hex[GP_SHA1_HEXSZ + 1];

			oid_hex(&target, hex);
			printf("Submodule path '%s': checked out '%s'\n", path,
			       hex);
		}
		commit_release(&c);
		repo_release(&sub);
		goto next;

undo:
		/* a store this call made, and then failed to fill, is not
		 * something to leave behind */
		if (made) {
			char *store = sm_store(r, name);

			remove_dir_recursive(store);
			free(store);
		}
next:
		free(recorded);
		free(gitfile);
		free(dir);
		free(url);
		free(path);
	}
	slist_release(names);
	return rc;
}

/* ------------------------------------------------------------------ */
/* sync / deinit / foreach                                             */

/*
 * Put the url `.gitmodules` has into the submodule's own remote, and into this
 * repository's configuration of it.  This is the command that fixes a checkout
 * whose submodule is still pointing at wherever it was cloned from.
 */
static int sm_sync(struct repo *r, struct opts *o)
{
	struct slist *names = sm_names(r);
	size_t i;

	for (i = 0; i < names->nr; i++) {
		const char *name = names->v[i];
		char *path = sm_entry(r, name, "path");
		char *url = sm_entry(r, name, "url");
		char *dir = NULL;

		if (!sm_wanted(o, path ? path : name))
			goto next;
		if (!path || !url) {
			gp_error("submodule '%s' is incomplete in .gitmodules",
				 name);
			goto next;
		}
		dir = xstrfmt("%s/%s", r->root, path);
		{
			char *gitfile = xstrfmt("%s/.git", dir);
			int present = is_file(gitfile);
			free(gitfile);

			if (present) {
				struct repo sub;

				if (sm_open(&sub, dir) == 0) {
					remote_set_url(&sub, "origin", url);
					repo_release(&sub);
				}
			}
		}
		{
			char *key = xstrfmt("submodule.%s.url", name);

			/* only when it was set: sync does not register a
			 * submodule, it corrects one */
			if (repo_config_get(r, key, NULL) == 0)
				repo_config_set(r, key, url, 0);
			free(key);
		}
		printf("Synchronizing submodule url for '%s'\n", path);
next:
		free(dir);
		free(url);
		free(path);
	}
	slist_release(names);
	return 0;
}

/*
 * Take a submodule's work tree away and forget that this checkout wanted it --
 * the store is left, since it holds whatever the submodule's own history is and
 * that is not this repository's to throw away.  What is in the work tree is,
 * though: a submodule with changes of its own is refused rather than cleared,
 * unless `-f` says otherwise.
 */
static int sm_deinit(struct repo *r, struct opts *o)
{
	struct slist *names = sm_names(r);
	int force = opts_flag(o, "-f") || opts_flag(o, "--force");
	size_t i;
	int rc = 0;

	if (!opts_arg(o, 1)) {
		gp_error("submodule deinit: expected a path\n"
			 "usage: gitprompt submodule deinit [-f] <path>...");
		slist_release(names);
		return 1;
	}

	for (i = 0; i < names->nr; i++) {
		const char *name = names->v[i];
		char *path = sm_entry(r, name, "path");
		char *url = sm_entry(r, name, "url");
		char *dir = NULL, *gitfile = NULL;
		char *key;

		if (!sm_wanted(o, path ? path : name))
			goto next;
		if (!path) {
			gp_error("submodule '%s' has no path in .gitmodules", name);
			rc = 1;
			goto next;
		}

		dir = xstrfmt("%s/%s", r->root, path);
		gitfile = xstrfmt("%s/.git", dir);

		if (is_file(gitfile)) {
			if (!force) {
				struct repo sub;
				int some = 0;

				if (sm_open(&sub, dir) == 0) {
					some = !worktree_is_clean(&sub);
					repo_release(&sub);
				}
				if (some) {
					gp_error("submodule '%s' has local changes; use -f to clear it",
						 path);
					rc = 1;
					goto next;
				}
			}
			if (remove_dir_recursive(dir) < 0) {
				gp_error("cannot clear '%s'", path);
				rc = 1;
				goto next;
			}
			printf("Cleared directory '%s'\n", path);
		}

		key = xstrfmt("submodule.%s.url", name);
		repo_config_unset(r, key);
		free(key);
		key = xstrfmt("submodule.%s.active", name);
		repo_config_unset(r, key);
		free(key);
		printf("Submodule '%s' (%s) unregistered for path '%s'\n", name,
		       url ? url : "", path);
next:
		free(gitfile);
		free(dir);
		free(url);
		free(path);
	}
	slist_release(names);
	return rc;
}

/*
 * Run one line with `dir` as the working directory, and the environment the
 * caller has already set.  The line goes to a file that a shell is pointed at
 * rather than to a `-c` argument: a command a user typed is full of quotes,
 * which would have to be quoted once for the shell and then again for the
 * command interpreter that starts the shell on Windows, and a file needs none
 * of that.
 *
 * `sh` is the shell git uses, and the one such commands are written for.  A
 * machine without it -- Windows without Git for Windows -- gets the line
 * through the default interpreter, which is what system() is there; that is
 * the same fallback, for the same reason, as `bisect run`.
 */
static int sm_shell(const char *dir, const char *script, const char *line)
{
	char *cwd = sm_getcwd(NULL, 0);
	char *quoted = NULL;
	int rc;

	if (write_file(script, line, strlen(line)) < 0) {
		gp_error("cannot write %s", script);
		free(cwd);
		return -1;
	}
	if (sm_chdir(dir) != 0) {
		gp_error("cannot enter %s", dir);
		remove(script);
		free(cwd);
		return -1;
	}

	quoted = xstrfmt("sh \"%s\"", script);
	rc = sm_wait_status(system(quoted));
	if (rc == 127 || rc == 9009) {
		/* no shell: the line goes to whatever system() uses here */
		rc = sm_wait_status(system(line));
	}
	free(quoted);

	if (cwd && sm_chdir(cwd) != 0)
		gp_warn("cannot return to %s", cwd);
	free(cwd);
	remove(script);
	return rc;
}

/*
 * Run one command in each submodule, the way a loop over them would.
 *
 * The command runs with the submodule's directory as the working directory,
 * which is what makes it useful, and with the variables git sets -- `$name`,
 * `$sm_path`, `$displaypath`, `$sha1` and `$toplevel` -- in its environment, so
 * a command written for git finds them.  A command that fails stops the walk
 * and is the status of this one, since an error partway through a loop is not
 * something to keep going past.
 */
static int sm_foreach(struct repo *r, struct opts *o)
{
	struct slist *names = sm_names(r);
	struct buf cmd = BUF_INIT;
	char *script = NULL;
	int i;
	size_t j;
	int rc = 0;

	for (i = 1; opts_arg(o, i); i++)
		buf_addf(&cmd, "%s%s", i > 1 ? " " : "", opts_arg(o, i));

	if (!cmd.len) {
		gp_error("submodule foreach: expected a command\n"
			 "usage: gitprompt submodule foreach <command>");
		slist_release(names);
		buf_release(&cmd);
		return 1;
	}

	{
		char *clean = gp_clean_path(r->gpdir);

		/* named for this process: a foreach whose command is itself a
		 * foreach would otherwise write over this file mid-run */
		script = xstrfmt("%s/foreach.%ld.sh", clean, gp_getpid());
		free(clean);
	}

	for (j = 0; j < names->nr; j++) {
		const char *name = names->v[j];
		char *path = sm_entry(r, name, "path");
		char *dir = NULL, *gitfile = NULL;

		if (!path)
			continue;
		dir = xstrfmt("%s/%s", r->root, path);
		gitfile = xstrfmt("%s/.git", dir);

		if (is_file(gitfile)) {
			char *sha = sm_recorded(r, path);

			/*
			 * The directory and the values go to the shell, so a
			 * quote in one of them would end a value early and let
			 * the rest be read as syntax.  They are refused rather
			 * than escaped, which is the same choice the editor and
			 * the transports make.
			 */
			if (strpbrk(dir, "\"\r\n") || strpbrk(path, "'\r\n") ||
			    (sha && strpbrk(sha, "'\r\n"))) {
				gp_error("submodule foreach: refusing to hand '%s' to the shell",
					 dir);
				rc = 1;
			} else {
				/*
				 * These go into the environment rather than into
				 * the line, so a command made of more than one
				 * word still finds them.  There is no `path`
				 * among them: an environment name is not
				 * case-sensitive on Windows, so setting one
				 * would take the PATH away and leave the shell
				 * unable to find anything at all -- git dropped
				 * it from its own list for the same reason.
				 */
				char *env;

				env = xstrfmt("name=%s", name);
				putenv(env);
				env = xstrfmt("sm_path=%s", path);
				putenv(env);
				env = xstrfmt("displaypath=%s", path);
				putenv(env);
				env = xstrfmt("sha1=%s", sha ? sha : "");
				putenv(env);
				env = xstrfmt("toplevel=%s", r->root);
				putenv(env);

				printf("Entering '%s'\n", path);
				fflush(stdout);
				rc = sm_shell(dir, script, buf_cstr(&cmd));
			}
			free(sha);
			if (rc) {
				free(gitfile);
				free(dir);
				free(path);
				break;
			}
		}
		free(gitfile);
		free(dir);
		free(path);
	}

	free(script);
	slist_release(names);
	buf_release(&cmd);
	/* the status of the command that stopped the walk, as git hands back */
	return rc > 0 ? rc : rc < 0 ? 1 : 0;
}

/* ------------------------------------------------------------------ */

static void sm_usage(void)
{
	gp_error("submodule: expected a subcommand\n"
		 "usage: gitprompt submodule add <url> [<path>]\n"
		 "   submodule status\n"
		 "   submodule init [<path>...]\n"
		 "   submodule update [--init] [--remote] [-f] [<path>...]\n"
		 "   submodule sync [<path>...]\n"
		 "   submodule deinit [-f] <path>...\n"
		 "   submodule foreach <command>");
}

int cmd_submodule(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *sub;

	opts_init(&o, argc, argv, (const char *const[]){
		"--init", "--remote", "-f", "--force", NULL });

	if (!r->root) {
		gp_error("submodule: this needs a work tree to work in");
		return 1;
	}

	sub = opts_arg(&o, 0);
	if (!sub) {
		sm_usage();
		return 1;
	}

	if (!strcmp(sub, "add"))
		return sm_add(r, &o);
	if (!strcmp(sub, "status"))
		return sm_status(r, &o);
	if (!strcmp(sub, "init"))
		return sm_init(r, &o);
	if (!strcmp(sub, "update"))
		return sm_update(r, &o);
	if (!strcmp(sub, "sync"))
		return sm_sync(r, &o);
	if (!strcmp(sub, "deinit"))
		return sm_deinit(r, &o);
	if (!strcmp(sub, "foreach"))
		return sm_foreach(r, &o);

	sm_usage();
	return 1;
}
