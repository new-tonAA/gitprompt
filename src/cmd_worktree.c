/*
 * cmd_worktree.c - one repository, more than one working directory.
 *
 * The objects and the refs belong to the repository and are one set; HEAD and
 * the index belong to a working directory and are one per directory.  So a
 * second worktree gets a directory of its own under <gpdir>/worktrees/<name>/
 * to hold those two, a `commondir` naming the store it shares, and a `.git`
 * *file* in the working directory naming that directory -- which is how the
 * repository is found again from inside it.  Nothing is copied: every worktree
 * reads the one object store and the one set of refs.
 *
 * The name is the last component of the working directory's path, as it is in
 * git.  A worktree deleted without being removed leaves its registration
 * behind, and `prune` is what clears those.
 */
#include "gp.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#define WT_SUBDIR "worktrees"

/* ------------------------------------------------------------------ */
/* the paths a registration is made of                                 */

#ifdef _WIN32
#include <direct.h>
#define wt_getcwd(b, n) _getcwd((b), (int)(n))
#else
#include <unistd.h>
#define wt_getcwd(b, n) getcwd((b), (n))
#endif

#ifdef _WIN32
static int wt_is_absolute(const char *p)
{
	if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
	    p[1] == ':')
		return 1;
	return p[0] == '/';
}
#else
static int wt_is_absolute(const char *p)
{
	return p[0] == '/';
}
#endif

/*
 * The path the user named, as an absolute one free of "." and "..".  Every
 * path stored in a registration and printed by `list` comes through here, so
 * the same worktree is the same string however it was named.
 */
static char *wt_abspath(const char *p)
{
	char cwd[4096];
	struct buf b = BUF_INIT;
	char *out;

	if (wt_is_absolute(p)) {
		buf_addstr(&b, p);
	} else {
		buf_addstr(&b, wt_getcwd(cwd, sizeof cwd) ? cwd : ".");
		buf_addch(&b, '/');
		buf_addstr(&b, p);
	}
	out = xstrndup((const char *)b.b, b.len);
	buf_release(&b);
	{
		char *clean = gp_clean_path(out);

		free(out);
		out = clean;
	}
	if (!out[0]) {
		free(out);
		out = xstrdup("/");
	}
	return out;
}

static char *wt_basename(const char *p)
{
	const char *slash = strrchr(p, '/');

	return xstrdup(slash ? slash + 1 : p);
}

/*
 * Where a worktree's own HEAD and index live.  The store's own path may have
 * come out of the system with "\\" and a trailing "/.", so the answer is put
 * through the same cleaning every other path here goes through: a registration
 * written by one run has to be found again by the next.
 */
static char *wt_admin_dir(struct repo *r, const char *name)
{
	char *raw = xstrfmt("%s/%s/%s", r->gpdir, WT_SUBDIR, name);
	char *clean = gp_clean_path(raw);

	free(raw);
	return clean;
}

/*
 * Where the main working tree lives: the directory the store hangs off.  A
 * bare repository has no working tree, and its store is the root itself.
 */
static char *wt_main_path(struct repo *r)
{
	size_t n = strlen(r->gpdir);
	const char *base = ".gitprompt";
	size_t bl = strlen(base);
	char *raw, *clean;

	if (n > bl && !strcmp(r->gpdir + n - bl, base) &&
	    (n - bl == 1 || r->gpdir[n - bl - 1] == '/'))
		raw = xstrndup(r->gpdir, n - bl - 1);
	else
		raw = xstrdup(r->gpdir);
	clean = gp_clean_path(raw);
	free(raw);
	return clean;
}

/*
 * The working directory a registration names.  `gitdir` holds the path of the
 * worktree's own .git file, so the directory is that with the last component
 * off; a registration with no such file names nothing.
 */
static char *wt_path_of(struct repo *r, const char *name)
{
	char *admin = wt_admin_dir(r, name);
	char *file = xstrfmt("%s/gitdir", admin);
	struct buf b = BUF_INIT;
	char *result = NULL;

	if (read_file(file, &b) >= 0) {
		char *s = (char *)b.b;
		size_t n = b.len;

		while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
			s[--n] = '\0';
		if (n) {
			char *clean = gp_clean_path(s);
			size_t m = strlen(clean);

			if (m > 5 && !strcmp(clean + m - 5, "/.git"))
				clean[m - 5] = '\0';
			result = clean;
		}
	}
	buf_release(&b);
	free(file);
	free(admin);
	return result;
}

static int wt_locked(struct repo *r, const char *name)
{
	char *admin = wt_admin_dir(r, name);
	char *file = xstrfmt("%s/locked", admin);
	int rc = is_file(file);

	free(file);
	free(admin);
	return rc;
}

/* ------------------------------------------------------------------ */
/* reading a worktree's HEAD, wherever its directory is                */

struct wt_head {
	char *branch;    /* "refs/heads/main", or NULL when detached */
	oid_t oid;
	int have_oid;
};

/*
 * `admin` is the directory holding the HEAD to read: a registration's own, or
 * the repository's for the main worktree.  Returns 0 when HEAD was read, in
 * which case `branch` is set for a symbolic HEAD and `have_oid` says whether
 * the commit it stands for could be looked up -- a branch with no commits yet
 * names one that is not there.  On -1 nothing is set.
 */
static int wt_head_of(struct repo *r, const char *admin, struct wt_head *out)
{
	char *file = xstrfmt("%s/HEAD", admin);
	struct buf b = BUF_INIT;
	char *s;
	size_t n;
	int rc = -1;

	memset(out, 0, sizeof *out);
	out->oid = null_oid;
	if (read_file(file, &b) < 0)
		goto out;
	s = (char *)b.b;
	n = b.len;
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
		s[--n] = '\0';
	if (n > 5 && !memcmp(s, "ref: ", 5)) {
		if (refs_read(&r->refs, s + 5, &out->oid) == 0) {
			out->have_oid = 1;
			out->branch = xstrdup(s + 5);
			rc = 0;
		} else {
			/* the branch is there, the commit is not yet */
			out->branch = xstrdup(s + 5);
			rc = 0;
		}
	} else if (n == GP_SHA1_HEXSZ && oid_parse(&out->oid, s) == 0) {
		out->have_oid = 1;
		rc = 0;
	}
out:
	buf_release(&b);
	free(file);
	return rc;
}

/* ------------------------------------------------------------------ */
/* the registrations                                                   */

static int wt_name_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* every registered name, sorted, NULL-terminated */
static char **wt_names(struct repo *r, size_t *nr_out)
{
	char *dir = xstrfmt("%s/%s", r->gpdir, WT_SUBDIR);
	DIR *d = opendir(dir);
	char **v = NULL;
	size_t nr = 0;
	struct dirent *de;

	*nr_out = 0;
	free(dir);
	if (!d)
		return NULL;
	while ((de = readdir(d))) {
		char *admin;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		admin = wt_admin_dir(r, de->d_name);
		if (is_directory(admin)) {
			v = xrealloc(v, (nr + 2) * sizeof(*v));
			v[nr++] = xstrdup(de->d_name);
		}
		free(admin);
	}
	closedir(d);
	if (v) {
		qsort(v, nr, sizeof(*v), wt_name_cmp);
		v[nr] = NULL;
	}
	*nr_out = nr;
	return v;
}

static void wt_names_free(char **v)
{
	size_t i;

	if (!v)
		return;
	for (i = 0; v[i]; i++)
		free(v[i]);
	free(v);
}

/* the registration whose directory is `abs`, or NULL */
static char *wt_name_for(struct repo *r, const char *abs)
{
	char **names;
	size_t nr, i;
	char *found = NULL;

	names = wt_names(r, &nr);
	for (i = 0; i < nr && !found; i++) {
		char *p = wt_path_of(r, names[i]);

		if (p && !strcmp(p, abs))
			found = xstrdup(names[i]);
		free(p);
	}
	wt_names_free(names);
	return found;
}

/*
 * The worktree that already has `branch` checked out, or NULL.  Each worktree
 * keeps its HEAD in a file of its own, so this asks each in turn, the main one
 * first -- its HEAD is the repository's own.
 */
static char *wt_holder(struct repo *r, const char *branch)
{
	char *want = xstrfmt("refs/heads/%s", branch);
	char *result = NULL;
	struct wt_head h;
	char **names;
	size_t nr, i;

	if (wt_head_of(r, r->gpdir, &h) == 0) {
		if (h.branch && !strcmp(h.branch, want))
			result = wt_main_path(r);
		free(h.branch);
	}
	if (result) {
		free(want);
		return result;
	}

	names = wt_names(r, &nr);
	for (i = 0; i < nr && !result; i++) {
		char *admin = wt_admin_dir(r, names[i]);

		if (wt_head_of(r, admin, &h) == 0) {
			if (h.branch && !strcmp(h.branch, want))
				result = wt_path_of(r, names[i]);
			free(h.branch);
		}
		free(admin);
	}
	wt_names_free(names);
	free(want);
	return result;
}

/*
 * The *other* working directory that has `branch` checked out, or NULL.  The
 * one the caller stands in is not counted: a branch's own worktree is not a
 * second checkout of it, and `switch_to` asks this to refuse the branch that
 * belongs to a different one.  A branch has a single HEAD, so two working
 * directories sharing it would put two sets of edits on one ref -- which is
 * what `worktree add` refuses for the same reason.
 */
char *worktree_other_holder(struct repo *r, const char *branch)
{
	char *holder = wt_holder(r, branch);
	char *here;

	if (!holder)
		return NULL;
	here = gp_clean_path(r->root);
	if (!strcmp(here, holder)) {
		free(holder);
		holder = NULL;
	}
	free(here);
	return holder;
}

static int wt_dir_empty(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *de;
	int empty = 1;

	if (!d)
		return 1;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		empty = 0;
		break;
	}
	closedir(d);
	return empty;
}

/* ------------------------------------------------------------------ */
/* add                                                                 */

static void wt_write(const char *path, const char *line)
{
	write_file(path, line, strlen(line));
}

/*
 * Lay down the two directories and the three small files that make a
 * registration, and the .git file that points back at it.  The caller has
 * already decided what HEAD should say.
 */
static int wt_register(struct repo *r, const char *name, const char *abs,
		       const char *branch, const oid_t *oid)
{
	char *admin = wt_admin_dir(r, name);
	char *f, *line;
	int rc = -1;

	if (mkdir_p(abs) < 0 || mkdir_p(admin) < 0) {
		gp_error("cannot create %s", abs);
		goto out;
	}

	f = xstrfmt("%s/HEAD", admin);
	if (branch) {
		line = xstrfmt("ref: refs/heads/%s\n", branch);
		wt_write(f, line);
		free(line);
	} else {
		char hex[GP_SHA1_HEXSZ + 2];

		oid_hex(oid, hex);
		hex[GP_SHA1_HEXSZ] = '\n';
		hex[GP_SHA1_HEXSZ + 1] = '\0';
		write_file(f, hex, GP_SHA1_HEXSZ + 1);
	}
	free(f);

	/* <gpdir>/worktrees/<name> is two levels below the store */
	f = xstrfmt("%s/commondir", admin);
	wt_write(f, "../..\n");
	free(f);

	f = xstrfmt("%s/gitdir", admin);
	line = xstrfmt("%s/.git\n", abs);
	wt_write(f, line);
	free(line);
	free(f);

	f = xstrfmt("%s/.git", abs);
	line = xstrfmt("gitdir: %s\n", admin);
	wt_write(f, line);
	free(line);
	free(f);

	rc = 0;
out:
	free(admin);
	return rc;
}

static int wt_add(struct repo *r, struct opts *o)
{
	const char *path = opts_arg(o, 1);
	const char *cs = opts_arg(o, 2);
	const char *newb = opts_value(o, "-b");
	int detach = opts_flag(o, "--detach");
	int force = opts_flag(o, "-f") || opts_flag(o, "--force");
	char *abs = NULL, *name = NULL, *admin = NULL, *create = NULL;
	char *branch = NULL, *holder = NULL;
	oid_t oid = null_oid;
	int made_abs = 0, rc = 1;

	if (!path) {
		gp_error("worktree add needs a path\n"
			 "usage: gitprompt worktree add [-b <branch>] [--detach] "
			 "[-f] <path> [<commit-ish>]");
		return 1;
	}
	if (opts_count(o) > 3) {
		gp_error("worktree add: unexpected argument '%s'", opts_arg(o, 3));
		return 1;
	}
	if (newb && detach) {
		gp_error("worktree add: -b and --detach cannot be combined");
		return 1;
	}

	abs = wt_abspath(path);
	name = wt_basename(abs);
	admin = wt_admin_dir(r, name);
	if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) {
		gp_error("'%s' is not a name a worktree can have", path);
		goto out;
	}

	if (is_directory(admin)) {
		char *old = wt_path_of(r, name);

		if (old && is_directory(old)) {
			gp_error("'%s' is already a worktree", abs);
			free(old);
			goto out;
		}
		if (!force) {
			gp_error("'%s' is registered as a worktree whose "
				 "directory is gone; use 'worktree prune' to "
				 "clear it", name);
			free(old);
			goto out;
		}
		remove_dir_recursive(admin);
		free(old);
	}

	/* what the new worktree starts at */
	if (cs) {
		if (resolve_rev(r, cs, &oid) < 0) {
			gp_error("invalid reference: %s", cs);
			goto out;
		}
	} else if (refs_head(&r->refs, &oid) < 0) {
		gp_error("HEAD does not point at a commit yet");
		goto out;
	}
	if (commit_peel(r, &oid, OBJ_COMMIT, &oid) < 0) {
		gp_error("not a commit: %s", cs ? cs : "HEAD");
		goto out;
	}

	/*
	 * A branch named after the directory is what a bare `worktree add`
	 * makes; a commit-ish that is a branch is checked out where it is,
	 * and one that is not detaches, which is git's arithmetic.
	 */
	if (newb) {
		create = xstrdup(newb);
	} else if (detach) {
		create = NULL;
	} else if (cs) {
		char *ref = xstrfmt("refs/heads/%s", cs);

		if (refs_exists(&r->refs, ref))
			branch = xstrdup(cs);
		free(ref);
	} else {
		create = xstrdup(name);
	}

	if (create) {
		char *ref;

		if (refs_check_name(create) < 0) {
			gp_error("'%s' is not a valid branch name; give one with -b",
				 create);
			goto out;
		}
		ref = xstrfmt("refs/heads/%s", create);
		if (refs_exists(&r->refs, ref)) {
			gp_error("a branch named '%s' already exists", create);
			free(ref);
			goto out;
		}
		free(ref);
	}

	holder = wt_holder(r, create ? create : branch);
	if (holder && !force) {
		gp_error("'%s' is already used by worktree at '%s'",
			 create ? create : branch, holder);
		goto out;
	}

	if (is_file(abs)) {
		gp_error("'%s' already exists", abs);
		goto out;
	}
	if (is_directory(abs) && !wt_dir_empty(abs)) {
		gp_error("'%s' already exists and is not empty", abs);
		goto out;
	}

	if (create) {
		printf("Preparing worktree (new branch '%s')\n", create);
	} else if (branch) {
		printf("Preparing worktree (checking out '%s')\n", branch);
	} else {
		char *ab = abbrev_oid(&oid);

		printf("Preparing worktree (detached HEAD %s)\n", ab);
		free(ab);
	}

	if (wt_register(r, name, abs, create ? create : branch,
			(create || branch) ? NULL : &oid) < 0)
		goto out;
	made_abs = 1;

	if (create) {
		char *ref = xstrfmt("refs/heads/%s", create);

		refs_write(&r->refs, ref, &oid);
		refs_reflog(&r->refs, ref, &null_oid, &oid,
			    "branch: Created from worktree add");
		free(ref);
	}

	{
		struct repo wt;
		struct commit c = COMMIT_INIT;

		if (repo_open(&wt, abs) < 0) {
			gp_error("cannot open the worktree at '%s'", abs);
			goto out;
		}
		read_commit(&wt, &oid, &c);
		if (checkout_tree(&wt, &c.tree, 1, 1) < 0) {
			commit_release(&c);
			repo_release(&wt);
			goto out;
		}
		commit_release(&c);
		repo_release(&wt);
	}

	{
		struct commit c = COMMIT_INIT;
		char *ab = abbrev_oid(&oid);
		char *line;

		read_commit(r, &oid, &c);
		line = commit_message_line(&c);
		printf("HEAD is now at %s %s\n", ab, line);
		free(line);
		free(ab);
		commit_release(&c);
	}
	rc = 0;

out:
	if (rc && made_abs) {
		if (create) {
			char *ref = xstrfmt("refs/heads/%s", create);

			refs_delete(&r->refs, ref);
			free(ref);
		}
		remove_dir_recursive(abs);
		remove_dir_recursive(admin);
	}
	free(holder);
	free(create);
	free(branch);
	free(admin);
	free(name);
	free(abs);
	return rc;
}

/* ------------------------------------------------------------------ */
/* list                                                                */

struct wt_row {
	char *path;
	char *abbrev;
	char *label;
	int locked;
};

static void wt_row_add(struct wt_row **v, size_t *nr, const char *path,
		       const struct wt_head *h, int locked)
{
	struct wt_row *r = *v;

	r = xrealloc(r, (*nr + 1) * sizeof(*r));
	r[*nr].path = xstrdup(path);
	r[*nr].abbrev = h->have_oid ? abbrev_oid(&h->oid) : xstrdup("");
	if (h->branch)
		r[*nr].label = xstrfmt("[%s]",
				       !strncmp(h->branch, "refs/heads/", 11)
				       ? h->branch + 11 : h->branch);
	else
		r[*nr].label = xstrdup("(detached HEAD)");
	r[*nr].locked = locked;
	(*nr)++;
	*v = r;
}

static int wt_list(struct repo *r, struct opts *o)
{
	struct wt_row *rows = NULL;
	size_t nr = 0, i, widest = 0;
	char **names;
	size_t nnames;

	if (opts_count(o) > 1) {
		gp_error("worktree list takes no arguments");
		return 1;
	}

	{
		struct wt_head h;

		if (wt_head_of(r, r->gpdir, &h) == 0) {
			char *p = wt_main_path(r);

			wt_row_add(&rows, &nr, p, &h, 0);
			free(h.branch);
			free(p);
		}
	}

	names = wt_names(r, &nnames);
	for (i = 0; i < nnames; i++) {
		char *admin = wt_admin_dir(r, names[i]);
		char *p = wt_path_of(r, names[i]);
		struct wt_head h;

		if (p && wt_head_of(r, admin, &h) == 0) {
			wt_row_add(&rows, &nr, p, &h, wt_locked(r, names[i]));
			free(h.branch);
		}
		free(p);
		free(admin);
	}
	wt_names_free(names);

	for (i = 0; i < nr; i++) {
		size_t n = strlen(rows[i].path);

		if (n > widest)
			widest = n;
	}
	for (i = 0; i < nr; i++) {
		size_t pad = widest - strlen(rows[i].path) + 2;

		printf("%s%*s%s %s%s\n", rows[i].path, (int)pad, "",
		       rows[i].abbrev, rows[i].label,
		       rows[i].locked ? " locked" : "");
		free(rows[i].path);
		free(rows[i].abbrev);
		free(rows[i].label);
	}
	free(rows);
	return 0;
}

/* ------------------------------------------------------------------ */
/* remove                                                              */

/*
 * The name the worktree directory of `path` is registered as, when it is one
 * this command may touch: not the main one, and not locked unless the caller
 * is the one undoing the lock.  Returns NULL after saying why not.
 */
static char *wt_takeable(struct repo *r, const char *path, struct opts *o,
			 int ignore_lock)
{
	char *abs = wt_abspath(path);
	char *name = wt_name_for(r, abs);
	char *main = wt_main_path(r);
	int force = opts_flag(o, "-f") || opts_flag(o, "--force");

	if (!name) {
		gp_error("'%s' is not a working tree", path);
		goto bad;
	}
	if (!strcmp(abs, main)) {
		gp_error("'%s' is a main working tree", path);
		goto bad;
	}
	if (!ignore_lock && wt_locked(r, name) && !force) {
		gp_error("'%s' is locked", path);
		goto bad;
	}
	free(main);
	free(abs);
	return name;
bad:
	free(main);
	free(abs);
	free(name);
	return NULL;
}

static int wt_remove(struct repo *r, struct opts *o)
{
	const char *path = opts_arg(o, 1);
	int force = opts_flag(o, "-f") || opts_flag(o, "--force");
	char *name, *abs, *admin;
	int rc = 1;

	if (!path) {
		gp_error("worktree remove needs a path\n"
			 "usage: gitprompt worktree remove [-f] <path>");
		return 1;
	}
	if (opts_count(o) > 2) {
		gp_error("worktree remove: unexpected argument '%s'", opts_arg(o, 2));
		return 1;
	}

	name = wt_takeable(r, path, o, 0);
	if (!name)
		return 1;
	abs = wt_abspath(path);
	admin = wt_admin_dir(r, name);

	if (!force && is_directory(abs)) {
		struct repo wt;
		int some = 0;

		if (repo_open(&wt, abs) == 0) {
			some = !worktree_is_clean(&wt);
			repo_release(&wt);
		}
		if (some) {
			gp_error("'%s' contains modified or untracked files, "
				 "use --force to delete it", path);
			goto out;
		}
	}

	remove_dir_recursive(abs);
	/*
	 * The registration goes only once the directory is really gone.  A
	 * removal that could not finish -- the caller standing in the worktree
	 * being removed, which is as far as this gets on Windows -- leaves the
	 * two together rather than a registration pointing at nothing.
	 */
	if (is_directory(abs)) {
		gp_error("cannot remove '%s'", path);
		goto out;
	}
	remove_dir_recursive(admin);
	rc = 0;
out:
	free(admin);
	free(abs);
	free(name);
	return rc;
}

/* ------------------------------------------------------------------ */
/* lock, unlock                                                        */

static int wt_lock(struct repo *r, struct opts *o, int lock)
{
	const char *path = opts_arg(o, 1);
	const char *reason = opts_value(o, "--reason");
	char *name, *admin;

	if (!path)
		path = r->root ? r->root : ".";
	if (opts_count(o) > 2) {
		gp_error("worktree %s: unexpected argument '%s'",
			 lock ? "lock" : "unlock", opts_arg(o, 2));
		return 1;
	}

	name = wt_takeable(r, path, o, !lock);
	if (!name)
		return 1;
	admin = wt_admin_dir(r, name);

	if (lock) {
		char *file = xstrfmt("%s/locked", admin);

		if (is_file(file)) {
			gp_error("'%s' is already locked", path);
			free(file);
			free(admin);
			free(name);
			return 1;
		}
		if (reason) {
			char *line = xstrfmt("%s\n", reason);

			wt_write(file, line);
			free(line);
		} else {
			wt_write(file, "\n");
		}
		free(file);
	} else {
		char *file = xstrfmt("%s/locked", admin);

		if (!is_file(file)) {
			gp_error("'%s' is not locked", path);
			free(file);
			free(admin);
			free(name);
			return 1;
		}
		remove_file(file);
		free(file);
	}

	free(admin);
	free(name);
	return 0;
}

/* ------------------------------------------------------------------ */
/* prune                                                               */

static int wt_prune(struct repo *r, struct opts *o)
{
	int dry = opts_flag(o, "-n") || opts_flag(o, "--dry-run");
	int verbose = opts_flag(o, "-v") || opts_flag(o, "--verbose");
	char **names;
	size_t nr, i;

	if (opts_count(o) > 1) {
		gp_error("worktree prune takes no arguments");
		return 1;
	}

	names = wt_names(r, &nr);
	for (i = 0; i < nr; i++) {
		char *p = wt_path_of(r, names[i]);
		char *admin = wt_admin_dir(r, names[i]);

		/*
		 * A locked worktree is left even when it is gone: the lock is
		 * the user saying the directory is somewhere this machine
		 * cannot see, and dropping the registration would be the one
		 * thing they asked not to happen.
		 */
		if (wt_locked(r, names[i])) {
			free(p);
			free(admin);
			continue;
		}
		if (!p || !is_directory(p)) {
			if (dry || verbose)
				printf("Removing %s/%s: %s\n", WT_SUBDIR,
				       names[i],
				       p ? "directory is gone"
					 : "gitdir file points to a non-existent location");
			if (!dry)
				remove_dir_recursive(admin);
		}
		free(p);
		free(admin);
	}
	wt_names_free(names);
	return 0;
}

/* ------------------------------------------------------------------ */
/* move                                                                */

static int wt_move(struct repo *r, struct opts *o)
{
	const char *from = opts_arg(o, 1);
	const char *to = opts_arg(o, 2);
	char *name, *abs_from, *abs_to, *admin, *f, *line;
	int rc = 1;

	if (!from || !to || opts_count(o) > 3) {
		gp_error("usage: gitprompt worktree move <from> <to>");
		return 1;
	}

	name = wt_takeable(r, from, o, 0);
	if (!name)
		return 1;
	abs_from = wt_abspath(from);
	abs_to = wt_abspath(to);
	admin = wt_admin_dir(r, name);

	if (is_file(abs_to) || (is_directory(abs_to) && !wt_dir_empty(abs_to))) {
		gp_error("'%s' already exists", to);
		goto out;
	}
	if (rename(abs_from, abs_to) != 0) {
		/*
		 * The source is named in full because that is the one that was
		 * found and resolved; the destination is left as it was typed,
		 * which is what git reports too.
		 */
		gp_error("failed to move '%s' to '%s': %s", abs_from, to,
			 strerror(errno));
		goto out;
	}

	f = xstrfmt("%s/gitdir", admin);
	line = xstrfmt("%s/.git\n", abs_to);
	wt_write(f, line);
	free(line);
	free(f);
	rc = 0;
out:
	free(admin);
	free(abs_to);
	free(abs_from);
	free(name);
	return rc;
}

/* ------------------------------------------------------------------ */

int cmd_worktree(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *sub;

	opts_init(&o, argc, argv, (const char *const[]){
		"-b=", "--detach", "-f", "--force", "-n", "--dry-run",
		"-v", "--verbose", "--reason=", NULL });

	sub = opts_arg(&o, 0);
	if (!sub || !strcmp(sub, "list"))
		return wt_list(r, &o);
	if (!strcmp(sub, "add"))
		return wt_add(r, &o);
	if (!strcmp(sub, "remove"))
		return wt_remove(r, &o);
	if (!strcmp(sub, "lock"))
		return wt_lock(r, &o, 1);
	if (!strcmp(sub, "unlock"))
		return wt_lock(r, &o, 0);
	if (!strcmp(sub, "prune"))
		return wt_prune(r, &o);
	if (!strcmp(sub, "move"))
		return wt_move(r, &o);

	gp_error("'%s' is not a worktree subcommand", sub);
	return 1;
}
