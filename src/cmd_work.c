/*
 * cmd_work.c - the commands that move things between the three places a
 * file can live: the work tree, the index, and the commits.
 *
 * A prompt is an ordinary file, so add/commit/status/diff treat it as an
 * ordinary file.  Nothing here knows what a prompt is.
 */
#include "gp.h"

#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* a small string list                                                 */

struct slist {
	char **v;
	size_t nr, alloc;
};

static void slist_push(struct slist *l, const char *s)
{
	if (l->nr == l->alloc) {
		l->alloc = l->alloc ? l->alloc * 2 : 16;
		l->v = xrealloc(l->v, l->alloc * sizeof(*l->v));
	}
	l->v[l->nr++] = xstrdup(s);
}

static void slist_release(struct slist *l)
{
	size_t i;
	for (i = 0; i < l->nr; i++)
		free(l->v[i]);
	free(l->v);
	l->v = NULL;
	l->nr = l->alloc = 0;
}

static int slist_has(const struct slist *l, const char *s)
{
	size_t i;
	for (i = 0; i < l->nr; i++)
		if (!strcmp(l->v[i], s))
			return 1;
	return 0;
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void slist_sort_unique(struct slist *l)
{
	size_t i, w;
	if (l->nr > 1)
		qsort(l->v, l->nr, sizeof(*l->v), cmp_str);
	w = 0;
	for (i = 0; i < l->nr; i++) {
		if (w > 0 && !strcmp(l->v[w - 1], l->v[i])) {
			free(l->v[i]);
			continue;
		}
		l->v[w++] = l->v[i];
	}
	l->nr = w;
}

/* ------------------------------------------------------------------ */
/* shared helpers                                                      */

/* HEAD's tree, if HEAD exists and points at a commit */
static int head_tree(struct repo *r, oid_t *tree)
{
	oid_t head;
	struct commit c = COMMIT_INIT;

	if (refs_head(&r->refs, &head) < 0)
		return -1;
	read_commit(r, &head, &c);
	*tree = c.tree;
	commit_release(&c);
	return 0;
}

/* "main", or "HEAD detached at 1a2b3c4" */
static char *branch_description(struct repo *r)
{
	char *t = refs_head_target(&r->refs);
	oid_t head;
	char hex[GP_SHA1_HEXSZ + 1];

	if (!t || refs_head(&r->refs, &head) < 0) {
		free(t);
		return xstrdup("(unborn)");
	}
	if (!strncmp(t, "refs/heads/", 11)) {
		char *name = xstrdup(t + 11);
		free(t);
		return name;
	}
	free(t);
	oid_hex(&head, hex);
	hex[7] = '\0';
	return xstrfmt("HEAD detached at %s", hex);
}

/* hash a work-tree file into the object store; returns 0 on success */
static int hash_worktree_file(struct repo *r, const char *relpath, oid_t *oid,
			      u32 *mode)
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
#ifndef _WIN32
	{
		struct stat st;
		if (stat(full, &st) == 0 && (st.st_mode & 0111))
			*mode = MODE_EXEC;
		else
			*mode = MODE_BLOB;
	}
#else
	*mode = MODE_BLOB;
#endif
	free(full);
	rc = odb_write(&r->odb, OBJ_BLOB, b.b, b.len, oid);
	buf_release(&b);
	return rc;
}

/* stage one work-tree path into an index that is already open */
static int stage_into(struct repo *r, struct index_state *ist,
		      const char *relpath)
{
	struct index_entry e;
	oid_t oid;
	u32 mode = MODE_BLOB;
	char *full;

	if (hash_worktree_file(r, relpath, &oid, &mode) < 0)
		return -1;

	memset(&e, 0, sizeof e);
	full = r->root ? xstrfmt("%s/%s", r->root, relpath) : xstrdup(relpath);
	index_fill_stat(&e, full);
	free(full);
	e.mode = mode;
	e.oid = oid;
	e.path = (char *)relpath;
	e.flags = 0;
	index_add(ist, &e);
	return 0;
}

/*
 * Stage one path, reading and writing the index around it.  Commands that
 * create a file and want it recorded straight away -- `prompt`, `capture`,
 * `outcome` -- use this rather than opening the index themselves.
 */
int stage_worktree_path(struct repo *r, const char *relpath)
{
	struct index_state ist;
	int rc;

	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));
	rc = stage_into(r, &ist, relpath);
	if (rc == 0)
		index_write(&ist, repo_index_path(r));
	index_release(&ist);
	return rc;
}

/* does `path` match any of the pathspecs?  No pathspecs means everything. */
static int matches(const struct slist *specs, const char *path)
{
	size_t i, n;
	if (!specs->nr)
		return 1;
	for (i = 0; i < specs->nr; i++) {
		n = strlen(specs->v[i]);
		if (!strncmp(path, specs->v[i], n) &&
		    (path[n] == '\0' || path[n] == '/'))
			return 1;
	}
	return 0;
}

struct wt_ctx {
	struct slist *out;
};

static void wt_collect(const char *relpath, void *ud)
{
	struct wt_ctx *c = ud;
	slist_push(c->out, relpath);
}

/* every file in the work tree, sorted */
static void worktree_paths(struct repo *r, struct slist *out)
{
	struct wt_ctx c;
	c.out = out;
	walk_worktree(r, wt_collect, &c);
	slist_sort_unique(out);
}

static void add_pathspecs(int argc, char **argv, const char *const *takes,
			  int ntakes, struct opts *o, struct slist *specs)
{
	int i;
	opts_init(o, argc, argv, takes, ntakes);
	for (i = 0; i < o->nargs; i++) {
		struct buf n;
		buf_init(&n);
		path_normalize(o->args[i], &n);
		if (n.len)
			slist_push(specs, buf_cstr(&n));
		buf_release(&n);
	}
}

/* ------------------------------------------------------------------ */
/* add                                                                 */

int cmd_add(struct repo *r, int argc, char **argv)
{
	static const char *const takes[] = { "--pathspec-from-file" };
	struct opts o;
	struct slist specs = { NULL, 0, 0 };
	struct index_state ist;
	struct slist paths = { NULL, 0, 0 };
	size_t i;
	int dry_run, update_only, all;

	add_pathspecs(argc, argv, takes, 1, &o, &specs);
	dry_run = opts_flag(&o, "-n") || opts_flag(&o, "--dry-run");
	update_only = opts_flag(&o, "-u") || opts_flag(&o, "--update");
	all = opts_flag(&o, "-A") || opts_flag(&o, "--all");

	if (!o.nargs && !all && !update_only) {
		gp_error("Nothing specified, nothing added.\nhint: Maybe you "
			 "wanted to say 'gitprompt add .'?");
		slist_release(&specs);
		return 1;
	}

	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));
	worktree_paths(r, &paths);

	if (update_only) {
		/* only paths already tracked are of interest */
		for (i = 0; i < ist.nr; i++) {
			if (!matches(&specs, ist.e[i].path))
				continue;
			if (slist_has(&paths, ist.e[i].path)) {
				if (dry_run)
					printf("add '%s'\n", ist.e[i].path);
				else
					stage_into(r, &ist, ist.e[i].path);
			} else if (!dry_run) {
				index_remove(&ist, ist.e[i].path);
				i--;
			}
		}
	} else {
		for (i = 0; i < paths.nr; i++) {
			if (path_is_ignored(r, paths.v[i]))
				continue;
			if (!matches(&specs, paths.v[i]))
				continue;
			if (dry_run)
				printf("add '%s'\n", paths.v[i]);
			else
				stage_into(r, &ist, paths.v[i]);
		}
		/* -A and an explicit list also record deletions */
		if (all || specs.nr) {
			for (i = 0; i < ist.nr; i++) {
				if (!matches(&specs, ist.e[i].path))
					continue;
				if (!slist_has(&paths, ist.e[i].path)) {
					if (dry_run)
						printf("remove '%s'\n", ist.e[i].path);
					else {
						index_remove(&ist, ist.e[i].path);
						i--;
					}
				}
			}
		}
	}

	/*
	 * Staging a path is how a merge conflict is declared resolved, exactly
	 * as in git: writing the path's stage-0 entry drops the stages it had.
	 * A path this command did not touch keeps its conflict, so
	 * `gitprompt add one.txt` leaves the other one still unmerged.
	 */
	if (!dry_run)
		index_write(&ist, repo_index_path(r));

	index_release(&ist);
	slist_release(&paths);
	slist_release(&specs);
	return 0;
}

/* ------------------------------------------------------------------ */
/* rm                                                                  */

int cmd_rm(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct index_state ist;
	struct slist specs = { NULL, 0, 0 };
	int cached, i, rc = 0;

	add_pathspecs(argc, argv, NULL, 0, &o, &specs);
	cached = opts_flag(&o, "--cached");

	if (!specs.nr) {
		gp_error("rm: expected at least one path");
		slist_release(&specs);
		return 1;
	}

	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));

	for (i = 0; i < o.nargs; i++) {
		struct buf n;
		const char *rel;
		buf_init(&n);
		path_normalize(o.args[i], &n);
		rel = buf_cstr(&n);
		if (!index_get(&ist, rel)) {
			gp_error("rm: '%s' is not tracked", o.args[i]);
			rc = 1;
			buf_release(&n);
			continue;
		}
		index_remove(&ist, rel);
		if (!cached) {
			char *full = r->root ? xstrfmt("%s/%s", r->root, rel)
					     : xstrdup(rel);
			remove_file(full);
			printf("rm '%s'\n", rel);
			free(full);
		} else {
			printf("rm '%s'\n", rel);
		}
		specs.nr = specs.nr;
		buf_release(&n);
	}

	index_write(&ist, repo_index_path(r));
	index_release(&ist);
	slist_release(&specs);
	return rc;
}

/* ------------------------------------------------------------------ */
/* mv                                                                  */

int cmd_mv(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct index_state ist;
	struct buf from, to;
	char *ffull, *tfull, *tdir;

	opts_init(&o, argc, argv, NULL, 0);
	if (!opts_arg(&o, 0) || !opts_arg(&o, 1)) {
		gp_error("mv: expected <source> <destination>");
		return 1;
	}

	buf_init(&from);
	buf_init(&to);
	path_normalize(opts_arg(&o, 0), &from);
	path_normalize(opts_arg(&o, 1), &to);

	ffull = r->root ? xstrfmt("%s/%s", r->root, buf_cstr(&from))
			: xstrdup(buf_cstr(&from));

	/* a trailing slash, or an existing directory, means "move it inside" */
	tdir = r->root ? xstrfmt("%s/%s", r->root, buf_cstr(&to))
		       : xstrdup(buf_cstr(&to));
	if (is_directory(tdir)) {
		const char *base = strrchr(buf_cstr(&from), '/');
		char *joined = xstrfmt("%s/%s", buf_cstr(&to), base ? base + 1
								    : buf_cstr(&from));
		buf_reset(&to);
		buf_addstr(&to, joined);
		free(joined);
	}
	free(tdir);

	tfull = r->root ? xstrfmt("%s/%s", r->root, buf_cstr(&to))
			: xstrdup(buf_cstr(&to));

	if (is_file(tfull)) {
		gp_error("mv: destination '%s' already exists", buf_cstr(&to));
		free(ffull);
		free(tfull);
		buf_release(&from);
		buf_release(&to);
		return 1;
	}

	{
		char *dir = xstrdup(tfull);
		char *slash = strrchr(dir, '/');
		if (slash) {
			*slash = '\0';
			mkdir_p(dir);
		}
		free(dir);
	}

	if (rename(ffull, tfull) != 0) {
		gp_error("mv: cannot move '%s' to '%s'", buf_cstr(&from),
			 buf_cstr(&to));
		free(ffull);
		free(tfull);
		buf_release(&from);
		buf_release(&to);
		return 1;
	}

	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));
	{
		struct index_entry *e = index_get(&ist, buf_cstr(&from));
		if (e) {
			struct index_entry copy = *e;
			copy.path = (char *)buf_cstr(&to);
			/* the moved file is staged content, whatever stage it was
			 * read from: a stage on its own would leave the new path
			 * unmerged with no other side to merge with */
			copy.stage = 0;
			index_fill_stat(&copy, tfull);
			index_remove(&ist, buf_cstr(&from));
			index_add(&ist, &copy);
		}
	}
	index_write(&ist, repo_index_path(r));
	index_release(&ist);

	printf("%s -> %s\n", buf_cstr(&from), buf_cstr(&to));
	free(ffull);
	free(tfull);
	buf_release(&from);
	buf_release(&to);
	return 0;
}

/* ------------------------------------------------------------------ */
/* status                                                              */

struct status_row {
	const char *path;
	char x, y;
	const char *what;
};

static int row_cmp(const void *a, const void *b)
{
	return strcmp(((const struct status_row *)a)->path,
		      ((const struct status_row *)b)->path);
}

/*
 * git's two letters and the words the long form uses for an unmerged path,
 * from which of the three stages the index holds: it is a switch over the
 * stages present, and each combination has a name of its own.
 */
static void unmerged_desc(const struct index_state *ist, const char *path,
			  char *x, char *y, const char **what)
{
	int mask = (index_get_stage(ist, path, 1) ? 1 : 0) |
		   (index_get_stage(ist, path, 2) ? 2 : 0) |
		   (index_get_stage(ist, path, 3) ? 4 : 0);

	switch (mask) {
	case 1:         /* base only: both sides deleted it */
		*x = 'D'; *y = 'D'; *what = "both deleted";
		break;
	case 2:         /* ours only: we added it, they never had it */
		*x = 'A'; *y = 'U'; *what = "added by us";
		break;
	case 3:         /* base and ours: they deleted what we changed */
		*x = 'U'; *y = 'D'; *what = "deleted by them";
		break;
	case 4:         /* theirs only: they added it */
		*x = 'U'; *y = 'A'; *what = "added by them";
		break;
	case 5:         /* base and theirs: we deleted what they changed */
		*x = 'D'; *y = 'U'; *what = "deleted by us";
		break;
	case 6:         /* ours and theirs, no base: both added it */
		*x = 'A'; *y = 'A'; *what = "both added";
		break;
	case 7:         /* all three: both changed it */
		*x = 'U'; *y = 'U'; *what = "both modified";
		break;
	default:
		*x = 'U'; *y = 'U'; *what = "unmerged";
		break;
	}
}

int cmd_status(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct index_state ist, head_ist;
	struct slist wt = { NULL, 0, 0 };
	struct status_row *rows = NULL;
	size_t nrows = 0, i;
	int short_fmt;
	oid_t tree;
	size_t nconf = 0;
	char **conf;

	opts_init(&o, argc, argv, NULL, 0);
	short_fmt = opts_flag(&o, "-s") || opts_flag(&o, "--short");

	memset(&ist, 0, sizeof ist);
	memset(&head_ist, 0, sizeof head_ist);
	index_read(&ist, repo_index_path(r));
	if (head_tree(r, &tree) == 0)
		read_tree_into_index(r, &head_ist, &tree, "");
	worktree_paths(r, &wt);
	conf = index_unmerged_paths(&ist, &nconf);

	rows = xcalloc(ist.nr + head_ist.nr + wt.nr + nconf + 1, sizeof(*rows));

	/* index against HEAD: what a commit would record */
	for (i = 0; i < ist.nr; i++) {
		struct index_entry *h;

		/* an unmerged path is described from its stages below, not from
		 * whichever of them this loop would have compared */
		if (ist.e[i].stage)
			continue;
		h = index_get(&head_ist, ist.e[i].path);
		if (!h)
			rows[nrows].x = 'A';
		else if (!oid_equal(&h->oid, &ist.e[i].oid))
			rows[nrows].x = 'M';
		else
			continue;
		rows[nrows].y = ' ';
		rows[nrows].path = ist.e[i].path;
		nrows++;
	}
	for (i = 0; i < head_ist.nr; i++) {
		if (!index_get(&ist, head_ist.e[i].path)) {
			rows[nrows].x = 'D';
			rows[nrows].y = ' ';
			rows[nrows].path = head_ist.e[i].path;
			nrows++;
		}
	}

	/* work tree against index: what is unstaged */
	for (i = 0; i < ist.nr; i++) {
		struct index_entry *e = &ist.e[i];
		char *full;
		struct buf b;
		oid_t oid;
		int found;
		size_t k;

		if (e->stage)
			continue;
		if (!slist_has(&wt, e->path)) {
			found = 0;              /* deleted from the work tree */
		} else {
			full = xstrfmt("%s/%s", r->root, e->path);
			buf_init(&b);
			found = read_file(full, &b) == 0 ? 1 : 0;
			free(full);
			if (found == 1) {
				odb_hash(&r->odb, OBJ_BLOB, b.b, b.len, &oid, 0);
				found = oid_equal(&oid, &e->oid) ? 2 : 1;
			}
			buf_release(&b);
		}
		if (found == 2) {
			/* unchanged; but merge in with any staged letter */
			for (k = 0; k < nrows; k++)
				if (!strcmp(rows[k].path, e->path))
					rows[k].y = ' ';
			continue;
		}
		{
			int merged = 0;
			for (k = 0; k < nrows; k++) {
				if (!strcmp(rows[k].path, e->path)) {
					rows[k].y = found ? 'M' : 'D';
					merged = 1;
					break;
				}
			}
			if (!merged) {
				rows[nrows].x = ' ';
				rows[nrows].y = found ? 'M' : 'D';
				rows[nrows].path = e->path;
				nrows++;
			}
		}
	}

	/* anything in the work tree and in neither the index nor HEAD */
	for (i = 0; i < wt.nr; i++) {
		if (index_get(&ist, wt.v[i]) || path_is_ignored(r, wt.v[i]))
			continue;
		rows[nrows].x = '?';
		rows[nrows].y = '?';
		rows[nrows].path = wt.v[i];
		nrows++;
	}

	if (nrows > 1)
		qsort(rows, nrows, sizeof(*rows), row_cmp);

	/* an unmerged path outranks whatever the three loops called it */
	for (i = 0; i < nconf; i++) {
		size_t k, at = nrows;
		for (k = 0; k < nrows; k++)
			if (!strcmp(rows[k].path, conf[i])) {
				at = k;
				break;
			}
		unmerged_desc(&ist, conf[i], &rows[at].x, &rows[at].y,
			      &rows[at].what);
		rows[at].path = conf[i];
		if (at == nrows)
			nrows++;
	}
	if (nconf && nrows > 1)
		qsort(rows, nrows, sizeof(*rows), row_cmp);

	if (short_fmt) {
		for (i = 0; i < nrows; i++)
			printf("%c%c %s\n", rows[i].x, rows[i].y, rows[i].path);
		free(rows);
		index_release(&ist);
		index_release(&head_ist);
		slist_release(&wt);
		index_paths_free(conf);
		return 0;
	}

	{
		char *br = branch_description(r);
		if (!strcmp(br, "(unborn)")) {
			char *t = refs_head_target(&r->refs);
			const char *shown = (t && !strncmp(t, "refs/heads/", 11))
						    ? t + 11 : "main";
			printf("On branch %s\n\nNo commits yet\n", shown);
			free(t);
		} else {
			printf("On branch %s\n", br);
		}
		free(br);
	}

	if (!nrows)
		printf("nothing to commit, working tree clean\n");

	if (nconf) {
		printf("\nYou have unmerged paths.\n");
		printf("  (fix conflicts, then run \"gitprompt add\" "
		       "and \"gitprompt commit\")\n");
	}

	{
		int section = 0;
		for (i = 0; i < nrows; i++) {
			/* `what` is set on unmerged rows and nowhere else; their
			 * letters can be any of UU/AA/DU/UD */
			if (rows[i].what || rows[i].x == '?')
				continue;
			if (rows[i].x == ' ') {
				if (section != 2) {
					printf("\nChanges not staged for commit:\n");
					section = 2;
				}
				printf("\t%s:   %s\n",
				       rows[i].y == 'D' ? "deleted" : "modified",
				       rows[i].path);
			} else {
				if (section != 1) {
					printf("\nChanges to be committed:\n");
					section = 1;
				}
				printf("\t%s:   %s\n",
				       rows[i].x == 'A' ? "new file"
							: rows[i].x == 'D'
								  ? "deleted"
								  : "modified",
				       rows[i].path);
			}
		}
	}

	{
		int printed = 0;
		for (i = 0; i < nrows; i++) {
			if (rows[i].x != '?')
				continue;
			if (!printed) {
				printf("\nUntracked files:\n");
				printed = 1;
			}
			printf("\t%s\n", rows[i].path);
		}
	}

	if (nconf) {
		int printed = 0;
		for (i = 0; i < nrows; i++) {
			if (!rows[i].what)
				continue;
			if (!printed) {
				printf("\nUnmerged paths:\n");
				printed = 1;
			}
			printf("\t%s:   %s\n", rows[i].what, rows[i].path);
		}
	}

	free(rows);
	index_release(&ist);
	index_release(&head_ist);
	slist_release(&wt);
	index_paths_free(conf);
	return 0;
}

/* ------------------------------------------------------------------ */
/* commit                                                              */
/* ------------------------------------------------------------------ */

static int do_commit(struct repo *r, const char *message, int amend,
		     int allow_empty, int quiet);

int cmd_commit(struct repo *r, int argc, char **argv)
{
	static const char *const takes[] = { "-m", "-F", "--author" };
	struct opts o;
	struct buf msg;
	const char *m;

	opts_init(&o, argc, argv, takes, 3);

	if (opts_flag(&o, "-a") || opts_flag(&o, "--all")) {
		struct index_state ist;
		struct slist wt = { NULL, 0, 0 };
		size_t i;

		memset(&ist, 0, sizeof ist);
		index_read(&ist, repo_index_path(r));
		worktree_paths(r, &wt);
		for (i = 0; i < ist.nr; i++) {
			if (slist_has(&wt, ist.e[i].path)) {
				stage_into(r, &ist, ist.e[i].path);
			} else {
				index_remove(&ist, ist.e[i].path);
				i--;
			}
		}
		index_write(&ist, repo_index_path(r));
		index_release(&ist);
		slist_release(&wt);
	}

	buf_init(&msg);
	m = opts_value(&o, "-m");
	if (m) {
		buf_addstr(&msg, m);
	} else if (opts_value(&o, "-F")) {
		if (read_file(opts_value(&o, "-F"), &msg) < 0)
			gp_die("commit: cannot read %s", opts_value(&o, "-F"));
	} else if (!merge_message(r, &msg)) {
		/* no editor here; absent a merge's own message, one is required */
		buf_release(&msg);
		gp_error("commit: no message given\nhint: pass -m \"...\" "
			 "to write the message on the command line");
		return 1;
	}
	if (msg.len && msg.b[msg.len - 1] != '\n')
		buf_addch(&msg, '\n');

	{
		int rc = do_commit(r, buf_cstr(&msg), opts_flag(&o, "--amend"),
				   opts_flag(&o, "--allow-empty"),
				   opts_flag(&o, "-q"));
		buf_release(&msg);
		return rc;
	}
}

static int do_commit(struct repo *r, const char *message, int amend,
		     int allow_empty, int quiet)
{
	struct index_state ist;
	struct commit c = COMMIT_INIT;
	struct buf body, ident;
	oid_t tree, parent, commit_oid;
	char hex[GP_SHA1_HEXSZ + 1];
	char *br = NULL, *branch_ref = NULL;
	int had_head;

	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));

	had_head = head_tree(r, &tree) == 0;   /* tree = old HEAD tree */
	if (had_head && amend) {
		oid_t head;
		struct commit old = COMMIT_INIT;
		refs_head(&r->refs, &head);
		read_commit(r, &head, &old);
		/* amend replaces the commit: keep its parents */
		{
			size_t i;
			for (i = 0; i < old.parents.nr; i++)
				oid_array_append(&c.parents, &old.parents.oid[i]);
		}
		commit_release(&old);
	} else if (had_head) {
		refs_head(&r->refs, &parent);
		oid_array_append(&c.parents, &parent);
	}

	/*
	 * A commit that concludes a merge gets a second parent: the revision
	 * MERGE_HEAD names.  Without this the join disappears from the history
	 * and the merge becomes an ordinary commit.  Conflicts cannot be
	 * committed through, which is what keeps a half-resolved merge from
	 * being recorded as if it were finished.
	 */
	if (!amend) {
		/*
		 * The unmerged paths are the index's own stages, so the refusal
		 * does not depend on MERGE_HEAD being there -- which it may not
		 * be, an index read from elsewhere carrying stages just the same.
		 */
		if (index_has_unmerged(&ist)) {
			size_t nconf = 0, k;
			char **conf = index_unmerged_paths(&ist, &nconf);

			gp_error("You have unmerged paths.");
			for (k = 0; k < nconf; k++) {
				char x, y;
				const char *what;

				unmerged_desc(&ist, conf[k], &x, &y, &what);
				fprintf(stderr, "\t%c%c %s\n", x, y, conf[k]);
			}
			fprintf(stderr, "hint: resolve them, then "
					"'gitprompt add' and 'gitprompt commit'\n");
			index_paths_free(conf);
			commit_release(&c);
			index_release(&ist);
			return 1;
		}
		{
			oid_t other;
			if (merge_in_progress(r, &other) == 0)
				oid_array_append(&c.parents, &other);
		}
	}

	if (write_tree_from_index(r, &ist, &tree) < 0)
		gp_die("commit: cannot write the tree");

	if (!allow_empty && had_head && !amend) {
		struct commit previous = COMMIT_INIT;
		oid_t head;
		refs_head(&r->refs, &head);
		read_commit(r, &head, &previous);
		if (oid_equal(&previous.tree, &tree) && c.parents.nr == 1) {
			commit_release(&previous);
			commit_release(&c);
			index_release(&ist);
			printf("nothing to commit, working tree clean\n");
			return 1;
		}
		commit_release(&previous);
	}

	buf_init(&body);
	buf_init(&ident);
	repo_ident_with_time(r, &ident);

	c.tree = tree;
	c.author = xstrdup(buf_cstr(&ident));
	if (amend) {
		oid_t head;
		struct commit old = COMMIT_INIT;
		refs_head(&r->refs, &head);
		read_commit(r, &head, &old);
		free(c.author);
		c.author = xstrdup(old.author ? old.author : buf_cstr(&ident));
		commit_release(&old);
	}
	c.committer = xstrdup(buf_cstr(&ident));
	c.message = xstrdup(message);
	{
		char *sess = repo_current_session(r);
		if (sess)
			c.session = sess;
	}

	commit_format(&c, &body);
	if (odb_write(&r->odb, OBJ_COMMIT, body.b, body.len, &commit_oid) < 0)
		gp_die("commit: cannot write the commit");

	branch_ref = refs_head_target(&r->refs);
	if (!branch_ref) {
		/* detached: move HEAD itself */
		refs_set_head_detached(&r->refs, &commit_oid);
	} else {
		oid_t old = null_oid;
		int had = refs_read(&r->refs, branch_ref, &old) == 0;
		refs_write(&r->refs, branch_ref, &commit_oid);
		refs_reflog(&r->refs, branch_ref, had ? &old : &null_oid,
			    &commit_oid, amend ? "commit (amend)" : "commit");
	}
	/* the merge is concluded; nothing is left to abort */
	merge_state_clear(r);

	br = branch_description(r);
	oid_hex(&commit_oid, hex);
	hex[7] = '\0';
	if (!quiet) {
		char *line = commit_message_line(&c);
		printf("[%s%s %s] %s\n", br, amend ? " (amend)" : "", hex, line);
		free(line);
	}

	free(br);
	free(branch_ref);
	commit_release(&c);
	buf_release(&body);
	buf_release(&ident);
	index_release(&ist);
	return 0;
}

/* ------------------------------------------------------------------ */
/* log                                                                 */

struct log_ctx {
	int oneline;
	int limit;
	int shown;
	int show_session;
};

static void format_author_line(const char *raw, struct buf *out)
{
	/* raw is "Name <email> 1700000000 +0800"; drop the timestamp */
	const char *gt = raw ? strrchr(raw, '>') : NULL;
	buf_reset(out);
	if (gt)
		buf_add(out, raw, (size_t)(gt - raw) + 1);
	else if (raw)
		buf_addstr(out, raw);
}

static void log_one(const oid_t *oid, const struct commit *c, void *ud)
{
	struct log_ctx *ctx = ud;
	char hex[GP_SHA1_HEXSZ + 1];
	struct buf who, when;

	if (ctx->limit && ctx->shown >= ctx->limit)
		return;
	ctx->shown++;

	oid_hex(oid, hex);

	if (ctx->oneline) {
		char *line = commit_message_line(c);
		printf("%s %s\n", hex, line);
		free(line);
		return;
	}

	printf("commit %s\n", hex);
	buf_init(&who);
	buf_init(&when);
	format_author_line(c->author, &who);
	epoch_to_iso8601(commit_time(c), &when);
	printf("Author: %s\n", buf_cstr(&who));
	printf("Date:   %s\n", buf_cstr(&when));
	if (ctx->show_session && c->session)
		printf("Session: %s\n", c->session);
	printf("\n");
	{
		const char *m = c->message ? c->message : "";
		const char *p = m;
		while (*p) {
			const char *nl = strchr(p, '\n');
			size_t len = nl ? (size_t)(nl - p) : strlen(p);
			printf("    %.*s\n", (int)len, p);
			if (!nl)
				break;
			p = nl + 1;
		}
	}
	printf("\n");
	buf_release(&who);
	buf_release(&when);
}

int cmd_log(struct repo *r, int argc, char **argv)
{
	static const char *const takes[] = { "-n", "--max-count" };
	struct opts o;
	struct oid_array tips = OID_ARRAY_INIT;
	struct log_ctx ctx;
	oid_t start;
	const char *n;

	opts_init(&o, argc, argv, takes, 2);
	memset(&ctx, 0, sizeof ctx);
	ctx.oneline = opts_flag(&o, "--oneline");
	ctx.show_session = 1;
	n = opts_value(&o, "-n") ? opts_value(&o, "-n")
				 : opts_value(&o, "--max-count");
	if (n)
		ctx.limit = atoi(n);

	if (opts_arg(&o, 0)) {
		if (resolve_rev(r, opts_arg(&o, 0), &start) < 0)
			gp_die("log: unknown revision: %s", opts_arg(&o, 0));
	} else if (refs_head(&r->refs, &start) < 0) {
		/* the branch is named rather than described: an unborn branch is
		 * still a branch, and "(unborn)" is not what a reader wants to
		 * see where the name goes */
		char *t = refs_head_target(&r->refs);
		const char *shown = (t && !strncmp(t, "refs/heads/", 11))
					    ? t + 11 : "main";
		gp_die("your current branch '%s' does not have any commits yet",
		       shown);
	}
	if (commit_peel(r, &start, OBJ_COMMIT, &start) < 0)
		gp_die("log: not a commit");

	oid_array_append(&tips, &start);
	walk_commits(r, &tips, log_one, &ctx);
	oid_array_clear(&tips);
	return 0;
}

/* ------------------------------------------------------------------ */
/* show                                                                */

int cmd_show(struct repo *r, int argc, char **argv)
{
	struct opts o;
	oid_t oid;
	enum obj_type t;

	opts_init(&o, argc, argv, NULL, 0);
	if (!opts_arg(&o, 0)) {
		gp_error("show: expected a revision");
		return 1;
	}
	if (resolve_rev(r, opts_arg(&o, 0), &oid) < 0)
		gp_die("show: unknown revision: %s", opts_arg(&o, 0));

	if (odb_type_of(&r->odb, &oid, &t) < 0)
		gp_die("show: cannot read %s", opts_arg(&o, 0));

	if (t == OBJ_COMMIT) {
		char hex[GP_SHA1_HEXSZ + 1];
		struct commit c = COMMIT_INIT;
		struct buf diff;
		oid_t old_tree, new_tree;
		int have_old = 0;

		read_commit(r, &oid, &c);
		oid_hex(&oid, hex);
		{
			struct buf who, when;
			buf_init(&who);
			buf_init(&when);
			format_author_line(c.author, &who);
			epoch_to_iso8601(commit_time(&c), &when);
			printf("commit %s\n", hex);
			printf("Author: %s\n", buf_cstr(&who));
			printf("Date:   %s\n", buf_cstr(&when));
			if (c.session)
				printf("Session: %s\n", c.session);
			printf("\n");
			{
				const char *m = c.message ? c.message : "";
				const char *p = m;
				while (*p) {
					const char *nl = strchr(p, '\n');
					size_t len = nl ? (size_t)(nl - p)
							: strlen(p);
					printf("    %.*s\n", (int)len, p);
					if (!nl)
						break;
					p = nl + 1;
				}
			}
			printf("\n");
			buf_release(&who);
			buf_release(&when);
		}

		if (c.parents.nr) {
			struct commit parent = COMMIT_INIT;
			read_commit(r, &c.parents.oid[0], &parent);
			old_tree = parent.tree;
			have_old = 1;
			commit_release(&parent);
		}
		new_tree = c.tree;

		buf_init(&diff);
		diff_trees(r, have_old ? &old_tree : NULL, &new_tree, &diff, 0);
		fwrite(diff.b, 1, diff.len, stdout);
		buf_release(&diff);
		commit_release(&c);
		return 0;
	}

	if (t == OBJ_TREE) {
		struct tree tr = TREE_INIT;
		size_t i;
		read_tree_obj(r, &oid, &tr);
		for (i = 0; i < tr.nr; i++) {
			char hex[GP_SHA1_HEXSZ + 1];
			oid_hex(&tr.e[i].oid, hex);
			printf("%06o %s %s\t%s\n", tr.e[i].mode,
			       (tr.e[i].mode & 0170000) == 0040000 ? "tree"
								   : "blob",
			       hex, tr.e[i].name);
		}
		tree_release(&tr);
		return 0;
	}

	{
		struct buf b;
		buf_init(&b);
		if (odb_read(&r->odb, &oid, NULL, &b) == 0)
			fwrite(b.b, 1, b.len, stdout);
		buf_release(&b);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* diff                                                                */

/* ------------------------------------------------------------------ */
/* the line diff engine                                                */

struct dline {
	const char *p;
	size_t len;
};

static void split_lines(const void *data, size_t len, struct dline **out,
			size_t *nr)
{
	struct dline *v = NULL;
	size_t n = 0, alloc = 0;
	const char *p = data, *end = p + len;

	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t l = nl ? (size_t)(nl - p) + 1 : (size_t)(end - p);
		if (n == alloc) {
			alloc = alloc ? alloc * 2 : 64;
			v = xrealloc(v, alloc * sizeof(*v));
		}
		v[n].p = p;
		v[n].len = l;
		n++;
		p += l;
	}
	*out = v;
	*nr = n;
}

struct oline {
	char op;                /* ' ', '-', '+' */
	const char *p;
	size_t len;
	size_t a, b;            /* 0-based indices into a[] and b[] */
};

struct stat_counts {
	long add, del;
};

/*
 * A plain longest-common-subsequence diff.  The table is quadratic, so
 * a pathological pair of files falls back to "replace everything" rather
 * than allocating gigabytes.
 */
static int lcs_diff(const struct dline *a, size_t na, const struct dline *b,
		    size_t nb, struct oline **out, size_t *nout,
		    struct stat_counts *counts)
{
	u32 *dp;
	size_t i, j, n = 0;
	struct oline *o;
	int fallback = 0;

	if (na && nb && na > 4000000 / (nb + 1))
		fallback = 1;

	o = xcalloc(na + nb + 1, sizeof(*o));
	memset(counts, 0, sizeof *counts);

	if (fallback) {
		for (i = 0; i < na; i++) {
			o[n].op = '-';
			o[n].p = a[i].p;
			o[n].len = a[i].len;
			o[n].a = i;
			o[n].b = 0;
			n++;
			counts->del++;
		}
		for (j = 0; j < nb; j++) {
			o[n].op = '+';
			o[n].p = b[j].p;
			o[n].len = b[j].len;
			o[n].a = na;
			o[n].b = j;
			n++;
			counts->add++;
		}
		*out = o;
		*nout = n;
		return 0;
	}

	dp = xcalloc((na + 1) * (nb + 1), sizeof(*dp));
	for (i = na; i-- > 0;) {
		for (j = nb; j-- > 0;) {
			if (a[i].len == b[j].len &&
			    !memcmp(a[i].p, b[j].p, a[i].len))
				dp[i * (nb + 1) + j] =
					dp[(i + 1) * (nb + 1) + j + 1] + 1;
			else {
				u32 x = dp[(i + 1) * (nb + 1) + j];
				u32 y = dp[i * (nb + 1) + j + 1];
				dp[i * (nb + 1) + j] = x > y ? x : y;
			}
		}
	}

	i = j = 0;
	while (i < na && j < nb) {
		if (a[i].len == b[j].len && !memcmp(a[i].p, b[j].p, a[i].len)) {
			o[n].op = ' ';
			o[n].p = a[i].p;
			o[n].len = a[i].len;
			o[n].a = i;
			o[n].b = j;
			n++;
			i++;
			j++;
		} else if (dp[(i + 1) * (nb + 1) + j] >=
			   dp[i * (nb + 1) + j + 1]) {
			o[n].op = '-';
			o[n].p = a[i].p;
			o[n].len = a[i].len;
			o[n].a = i;
			o[n].b = j;
			n++;
			counts->del++;
			i++;
		} else {
			o[n].op = '+';
			o[n].p = b[j].p;
			o[n].len = b[j].len;
			o[n].a = i;
			o[n].b = j;
			n++;
			counts->add++;
			j++;
		}
	}
	while (i < na) {
		o[n].op = '-';
		o[n].p = a[i].p;
		o[n].len = a[i].len;
		o[n].a = i;
		o[n].b = nb;
		n++;
		counts->del++;
		i++;
	}
	while (j < nb) {
		o[n].op = '+';
		o[n].p = b[j].p;
		o[n].len = b[j].len;
		o[n].a = na;
		o[n].b = j;
		n++;
		counts->add++;
		j++;
	}
	free(dp);
	*out = o;
	*nout = n;
	return 0;
}

static int looks_binary(const void *data, size_t len)
{
	const u8 *p = data;
	size_t i, n = len < 8000 ? len : 8000;
	for (i = 0; i < n; i++)
		if (p[i] == 0)
			return 1;
	return 0;
}

static int append_hunks(const struct oline *o, size_t n, struct buf *out)
{
	size_t i;
	int any_change = 0;

	for (i = 0; i < n; i++)
		if (o[i].op != ' ')
			any_change = 1;

	if (!any_change)
		return 0;

	/* one hunk covering everything, with three lines of context */
	{
		size_t first = 0, last = n;
		size_t k;

		for (k = 0; k < n; k++)
			if (o[k].op != ' ') {
				first = k > 3 ? k - 3 : 0;
				break;
			}
		for (k = n; k-- > 0;)
			if (o[k].op != ' ') {
				last = (k + 4 < n) ? k + 4 : n;
				break;
			}

		{
			size_t a_start = o[first].a + 1, b_start = o[first].b + 1;
			size_t a_len = 0, b_len = 0;
			for (k = first; k < last; k++) {
				if (o[k].op != '+')
					a_len++;
				if (o[k].op != '-')
					b_len++;
			}
			buf_addf(out, "@@ -%lu,%lu +%lu,%lu @@\n",
				 (unsigned long)a_start, (unsigned long)a_len,
				 (unsigned long)b_start, (unsigned long)b_len);
			for (k = first; k < last; k++) {
				buf_addch(out, o[k].op);
				buf_add(out, o[k].p, o[k].len);
				if (!o[k].len || o[k].p[o[k].len - 1] != '\n')
					buf_addch(out, '\n');
			}
		}
	}
	return 1;
}

int diff_buffers(const char *a_label, const void *a, size_t alen,
		 const char *b_label, const void *b, size_t blen,
		 struct buf *out, int stat_only)
{
	struct dline *la = NULL, *lb = NULL;
	size_t na = 0, nb = 0;
	struct oline *o = NULL;
	size_t n = 0;
	struct stat_counts counts;

	if (a && looks_binary(a, alen)) {
		buf_addf(out, "Binary files %s and %s differ\n", a_label, b_label);
		return 1;
	}
	if (b && looks_binary(b, blen)) {
		buf_addf(out, "Binary files %s and %s differ\n", a_label, b_label);
		return 1;
	}

	split_lines(a ? a : "", a ? alen : 0, &la, &na);
	split_lines(b ? b : "", b ? blen : 0, &lb, &nb);
	lcs_diff(la, na, lb, nb, &o, &n, &counts);

	if (!counts.add && !counts.del) {
		free(la);
		free(lb);
		free(o);
		return 0;
	}

	if (stat_only) {
		buf_addf(out, " %s | %ld ", b_label, counts.add + counts.del);
		{
			long k;
			for (k = 0; k < counts.add; k++)
				buf_addch(out, '+');
			for (k = 0; k < counts.del; k++)
				buf_addch(out, '-');
		}
		buf_addch(out, '\n');
	} else {
		buf_addf(out, "--- %s\n", a_label);
		buf_addf(out, "+++ %s\n", b_label);
		append_hunks(o, n, out);
	}

	free(la);
	free(lb);
	free(o);
	return 1;
}

/* ------------------------------------------------------------------ */

/* walk two trees in parallel and emit a diff per differing path */
struct tdiff_ctx {
	struct repo *r;
	struct buf *out;
	int stat_only;
};

static void tdiff_emit(struct tdiff_ctx *c, const char *path,
		       const oid_t *old_oid, u32 old_mode,
		       const oid_t *new_oid, u32 new_mode)
{
	struct buf a, b;
	char *alab, *blab;

	buf_init(&a);
	buf_init(&b);
	if (old_oid)
		odb_read(&c->r->odb, old_oid, NULL, &a);
	if (new_oid)
		odb_read(&c->r->odb, new_oid, NULL, &b);

	buf_addf(c->out, "diff --git a/%s b/%s\n", path, path);
	if (old_oid && new_oid && old_mode != new_mode)
		buf_addf(c->out, "old mode %06o\nnew mode %06o\n", old_mode,
			 new_mode);

	alab = old_oid ? xstrfmt("a/%s", path) : xstrdup("/dev/null");
	blab = new_oid ? xstrfmt("b/%s", path) : xstrdup("/dev/null");
	diff_buffers(alab, a.b, a.len, blab, b.b, b.len, c->out, c->stat_only);
	free(alab);
	free(blab);
	buf_release(&a);
	buf_release(&b);
}

static void tdiff_recurse(struct tdiff_ctx *c, const char *prefix,
			  const oid_t *old_tree, const oid_t *new_tree)
{
	struct tree ot = TREE_INIT, nt = TREE_INIT;
	size_t i = 0, j = 0;

	if (old_tree)
		read_tree_obj(c->r, old_tree, &ot);
	if (new_tree)
		read_tree_obj(c->r, new_tree, &nt);

	while (i < ot.nr || j < nt.nr) {
		int cmp;
		if (i >= ot.nr)
			cmp = 1;
		else if (j >= nt.nr)
			cmp = -1;
		else
			cmp = strcmp(ot.e[i].name, nt.e[j].name);

		if (cmp < 0) {
			char *p = xstrfmt("%s%s", prefix, ot.e[i].name);
			char *full = xstrfmt("%s/", p);
			if ((ot.e[i].mode & 0170000) == 0040000)
				tdiff_recurse(c, full, &ot.e[i].oid, NULL);
			else
				tdiff_emit(c, p, &ot.e[i].oid, ot.e[i].mode, NULL, 0);
			free(p);
			free(full);
			i++;
		} else if (cmp > 0) {
			char *p = xstrfmt("%s%s", prefix, nt.e[j].name);
			char *full = xstrfmt("%s/", p);
			if ((nt.e[j].mode & 0170000) == 0040000)
				tdiff_recurse(c, full, NULL, &nt.e[j].oid);
			else
				tdiff_emit(c, p, NULL, 0, &nt.e[j].oid, nt.e[j].mode);
			free(p);
			free(full);
			j++;
		} else {
			char *p = xstrfmt("%s%s", prefix, ot.e[i].name);
			int is_dir = (ot.e[i].mode & 0170000) == 0040000;
			if (is_dir) {
				char *full = xstrfmt("%s/", p);
				tdiff_recurse(c, full, &ot.e[i].oid, &nt.e[j].oid);
				free(full);
			} else if (!oid_equal(&ot.e[i].oid, &nt.e[j].oid)) {
				tdiff_emit(c, p, &ot.e[i].oid, ot.e[i].mode,
					   &nt.e[j].oid, nt.e[j].mode);
			}
			free(p);
			i++;
			j++;
		}
	}
	tree_release(&ot);
	tree_release(&nt);
}

void diff_trees(struct repo *r, const oid_t *old_tree, const oid_t *new_tree,
		struct buf *out, int stat_only)
{
	struct tdiff_ctx c;
	c.r = r;
	c.out = out;
	c.stat_only = stat_only;
	tdiff_recurse(&c, "", old_tree, new_tree);
}

/* ------------------------------------------------------------------ */

int cmd_diff(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct buf out;
	int cached, stat_only;
	oid_t old_tree, new_tree;
	int have_old = 0, have_new = 0;

	opts_init(&o, argc, argv, NULL, 0);
	cached = opts_flag(&o, "--cached") || opts_flag(&o, "--staged");
	stat_only = opts_flag(&o, "--stat");

	buf_init(&out);

	if (o.nargs >= 1) {
		if (resolve_rev_tree(r, o.args[0], &old_tree) < 0)
			gp_die("diff: not a tree: %s", o.args[0]);
		have_old = 1;
	}
	if (o.nargs >= 2) {
		if (resolve_rev_tree(r, o.args[1], &new_tree) < 0)
			gp_die("diff: not a tree: %s", o.args[1]);
		have_new = 1;
	}

	if (have_new) {
		diff_trees(r, have_old ? &old_tree : NULL, &new_tree, &out,
			   stat_only);
	} else if (cached || have_old) {
		struct index_state ist;
		struct tree index_tree = TREE_INIT;
		oid_t it;
		memset(&ist, 0, sizeof ist);
		index_read(&ist, repo_index_path(r));
		if (write_tree_from_index(r, &ist, &it) < 0) {
			/* an index with unmerged paths has no tree to diff */
			index_release(&ist);
			buf_release(&out);
			return 1;
		}
		read_tree_obj(r, &it, &index_tree);
		if (have_old) {
			diff_trees(r, &old_tree, &it, &out, stat_only);
		} else if (head_tree(r, &old_tree) == 0) {
			diff_trees(r, &old_tree, &it, &out, stat_only);
		} else {
			diff_trees(r, NULL, &it, &out, stat_only);
		}
		index_release(&ist);
		tree_release(&index_tree);
	} else {
		/* work tree against the index, path by path */
		struct index_state ist;
		size_t i;
		memset(&ist, 0, sizeof ist);
		index_read(&ist, repo_index_path(r));
		for (i = 0; i < ist.nr; i++) {
			char *full;

			/* three stages of one path are one file, not three */
			if (ist.e[i].stage)
				continue;
			full = xstrfmt("%s/%s", r->root, ist.e[i].path);
			struct buf w;
			struct buf piece;
			int changed;
			buf_init(&w);
			buf_init(&piece);
			if (read_file(full, &w) < 0)
				buf_reset(&w);
			{
				struct buf old;
				buf_init(&old);
				odb_read(&r->odb, &ist.e[i].oid, NULL, &old);
				buf_addf(&piece, "diff --git a/%s b/%s\n",
					 ist.e[i].path, ist.e[i].path);
				{
					char *alab = xstrfmt("a/%s", ist.e[i].path);
					char *blab = xstrfmt("b/%s", ist.e[i].path);
					changed = diff_buffers(alab, old.b, old.len,
							       blab, w.b, w.len,
							       &piece, stat_only);
					free(alab);
					free(blab);
				}
				buf_release(&old);
			}
			if (changed)
				buf_add(&out, piece.b, piece.len);
			free(full);
			buf_release(&w);
			buf_release(&piece);
		}
		index_release(&ist);
	}

	fwrite(out.b, 1, out.len, stdout);
	buf_release(&out);
	return 0;
}

/* ------------------------------------------------------------------ */
/* reset                                                               */

int cmd_reset(struct repo *r, int argc, char **argv)
{
	struct opts o;
	oid_t target;
	struct commit c = COMMIT_INIT;
	char *branch_ref;
	int mode;               /* 0 soft, 1 mixed, 2 hard */

	opts_init(&o, argc, argv, NULL, 0);
	mode = opts_flag(&o, "--soft") ? 0
	     : opts_flag(&o, "--hard") ? 2
				       : 1;

	if (opts_arg(&o, 0)) {
		if (resolve_rev(r, opts_arg(&o, 0), &target) < 0)
			gp_die("reset: unknown revision: %s", opts_arg(&o, 0));
		if (commit_peel(r, &target, OBJ_COMMIT, &target) < 0)
			gp_die("reset: not a commit: %s", opts_arg(&o, 0));
	} else if (refs_head(&r->refs, &target) < 0) {
		gp_die("reset: HEAD has no commits yet");
	}

	read_commit(r, &target, &c);
	branch_ref = refs_head_target(&r->refs);

	if (branch_ref) {
		oid_t old = null_oid;
		int had = refs_read(&r->refs, branch_ref, &old) == 0;
		refs_write(&r->refs, branch_ref, &target);
		refs_reflog(&r->refs, branch_ref, had ? &old : &null_oid, &target,
			    opts_flag(&o, "--hard") ? "reset: moving to HEAD"
						    : "reset: moving to HEAD");
	} else {
		refs_set_head_detached(&r->refs, &target);
	}

	if (mode >= 1) {
		struct index_state ist;
		memset(&ist, 0, sizeof ist);
		read_tree_into_index(r, &ist, &c.tree, "");
		index_write(&ist, repo_index_path(r));
		index_release(&ist);
	}
	if (mode >= 2) {
		checkout_tree(r, &c.tree, 1, 1);
		printf("HEAD is now at %s\n", abbrev_oid(&target));
	}

	free(branch_ref);
	commit_release(&c);
	return 0;
}

/* ------------------------------------------------------------------ */
/* reflog                                                              */

int cmd_reflog(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *name;
	char *path, *full;
	struct buf b;
	const char *p, *end;

	opts_init(&o, argc, argv, NULL, 0);
	name = opts_arg(&o, 0);
	if (!name)
		name = "HEAD";

	/* HEAD's reflog lives with the branch it names */
	if (!strcmp(name, "HEAD")) {
		char *t = refs_head_target(&r->refs);
		full = t ? xstrdup(t) : xstrdup("HEAD");
		free(t);
	} else if (strchr(name, '/')) {
		full = xstrdup(name);
	} else {
		char *t = refs_head_target(&r->refs);
		if (t && !strncmp(t, "refs/heads/", 11) &&
		    !strcmp(t + 11, name)) {
			full = xstrdup(t);
		} else {
			full = xstrfmt("refs/heads/%s", name);
		}
		free(t);
	}

	path = repo_git_path(r, "logs/%s", full);
	buf_init(&b);
	if (read_file(path, &b) < 0) {
		gp_error("reflog: no reflog for %s", name);
		free(path);
		free(full);
		buf_release(&b);
		return 1;
	}

	p = (const char *)b.b;
	end = p + b.len;
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
		if (len > 7 && !memcmp(p, "0000000", 7) && p[7] == '0') {
			/* the birth line is noisy; keep it, git does too */
		}
		if (len)
			printf("%.*s\n", (int)len, p);
		p = nl ? nl + 1 : end;
	}

	free(path);
	free(full);
	buf_release(&b);
	return 0;
}

/* ------------------------------------------------------------------ */
/* describe                                                            */

struct tag_scan {
	struct repo *r;
	struct oid_array oids;   /* peeled commit for each tag */
	struct slist names;      /* ref name without refs/tags/ */
};

static void tag_scan_cb(const char *name, const oid_t *oid, void *ud)
{
	struct tag_scan *t = ud;
	oid_t peeled;
	const char *shortname = name;
	if (!strncmp(shortname, "refs/tags/", 10))
		shortname += 10;
	if (commit_peel(t->r, oid, OBJ_COMMIT, &peeled) == 0) {
		oid_array_append(&t->oids, &peeled);
		slist_push(&t->names, shortname);
	}
}

struct describe_ctx {
	const struct tag_scan *tags;
	int found;
	int distance;
	const char *tagname;
};

static void describe_cb(const oid_t *oid, const struct commit *c, void *ud)
{
	struct describe_ctx *d = ud;
	size_t i;
	(void)c;
	if (d->found)
		return;
	for (i = 0; i < d->tags->oids.nr; i++) {
		if (oid_equal(&d->tags->oids.oid[i], oid)) {
			d->tagname = d->tags->names.v[i];
			d->found = 1;
			return;
		}
	}
	d->distance++;
}

int cmd_describe(struct repo *r, int argc, char **argv)
{
	static const char *const takes[] = { "--tags" };
	struct opts o;
	oid_t target;
	struct oid_array tips = OID_ARRAY_INIT;
	struct tag_scan ts;
	struct describe_ctx d;

	opts_init(&o, argc, argv, takes, 1);

	if (opts_arg(&o, 0)) {
		if (resolve_rev(r, opts_arg(&o, 0), &target) < 0)
			gp_die("describe: unknown revision: %s", opts_arg(&o, 0));
	} else if (refs_head(&r->refs, &target) < 0) {
		gp_die("describe: HEAD has no commits yet");
	}
	if (commit_peel(r, &target, OBJ_COMMIT, &target) < 0)
		gp_die("describe: not a commit");

	memset(&ts, 0, sizeof ts);
	ts.r = r;
	refs_list(&r->refs, "refs/tags/", tag_scan_cb, &ts);

	memset(&d, 0, sizeof d);
	d.tags = &ts;

	oid_array_append(&tips, &target);
	walk_commits(r, &tips, describe_cb, &d);
	oid_array_clear(&tips);

	if (d.tagname) {
		if (d.distance == 0)
			printf("%s\n", d.tagname);
		else
			printf("%s-%d-g%s\n", d.tagname, d.distance,
			       abbrev_oid(&target));
	} else {
		if (opts_flag(&o, "--tags"))
			printf("%s\n", abbrev_oid(&target));
		else
			gp_die("describe: no tags can describe %s",
			       abbrev_oid(&target));
	}
	oid_array_clear(&ts.oids);
	slist_release(&ts.names);
	return 0;
}
