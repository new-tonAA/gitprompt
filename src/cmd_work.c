/*
 * cmd_work.c - the commands that move things between the three places a
 * file can live: the work tree, the index, and the commits.
 *
 * A prompt is an ordinary file, so add/commit/status/diff treat it as an
 * ordinary file.  Nothing here knows what a prompt is.
 */
#include "gp.h"

#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* a small string list                                                 */

void slist_push(struct slist *l, const char *s)
{
	if (l->nr == l->alloc) {
		l->alloc = l->alloc ? l->alloc * 2 : 16;
		l->v = xrealloc(l->v, l->alloc * sizeof(*l->v));
	}
	l->v[l->nr++] = xstrdup(s);
}

void slist_release(struct slist *l)
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
int slist_matches(const struct slist *specs, const char *path)
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

static void add_pathspecs(int argc, char **argv, const char *const *allows,
			  struct opts *o, struct slist *specs)
{
	int i;
	opts_init(o, argc, argv, allows);
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

/*
 * A named path the ignore rules match is not staged in silence: git names it
 * and stops, because a build output file that got committed once is otherwise
 * the kind of mistake nobody notices for a year.  The wording is git's, save
 * its last line -- there is no advice.addIgnoredFile here to turn this off, so
 * offering the setting would be a lie.
 */
static int refuse_ignored(const struct slist *ign)
{
	size_t i;

	fputs("The following paths are ignored by one of your "
	      ".gitpromptignore files:\n", stderr);
	for (i = 0; i < ign->nr; i++)
		fprintf(stderr, "%s\n", ign->v[i]);
	fputs("hint: Use -f if you really want to add them.\n", stderr);
	return 1;
}

/*
 * -f on a named ignored path.  The walk that filled `paths` left it out, so
 * the spec is followed here instead: a file is staged, a directory is walked
 * and everything under it is staged, its own ignore rules included.
 */
static void stage_forced(struct repo *r, struct index_state *ist,
			 const char *rel)
{
	char *abs = xstrfmt("%s/%s", r->root, rel);

	if (is_directory(abs)) {
		DIR *d = opendir(abs);
		struct dirent *de;

		if (d) {
			while ((de = readdir(d))) {
				char *sr;

				if (!strcmp(de->d_name, ".") ||
				    !strcmp(de->d_name, ".."))
					continue;
				sr = xstrfmt("%s/%s", rel, de->d_name);
				stage_forced(r, ist, sr);
				free(sr);
			}
			closedir(d);
		}
	} else if (is_file(abs)) {
		stage_into(r, ist, rel);
	}
	free(abs);
}

int cmd_add(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct slist specs = { NULL, 0, 0 };
	struct index_state ist;
	struct slist paths = { NULL, 0, 0 };
	struct slist ign = { NULL, 0, 0 };
	size_t i;
	int dry_run, update_only, all, force;

	add_pathspecs(argc, argv, (const char *const[]){
		"-A", "--all", "-u", "--update", "-n", "--dry-run",
		"-f", "--force", "--pathspec-from-file=", NULL }, &o, &specs);
	dry_run = opts_flag(&o, "-n") || opts_flag(&o, "--dry-run");
	update_only = opts_flag(&o, "-u") || opts_flag(&o, "--update");
	all = opts_flag(&o, "-A") || opts_flag(&o, "--all");
	force = opts_flag(&o, "-f") || opts_flag(&o, "--force");

	if (!o.nargs && !all && !update_only) {
		gp_error("Nothing specified, nothing added.\nhint: Maybe you "
			 "wanted to say 'gitprompt add .'?");
		slist_release(&specs);
		return 1;
	}

	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));
	worktree_paths(r, &paths);

	/*
	 * Only a path named outright is refused.  `add sub` walks a directory
	 * that is not itself ignored and quietly leaves its ignored files be,
	 * and `add .` and `add -A` say nothing at all -- which is git's
	 * behaviour, and the reason a build directory does not turn every
	 * `add .` into a refusal.
	 */
	if (specs.nr && !all && !update_only) {
		for (i = 0; i < specs.nr; i++) {
			char *abs = xstrfmt("%s/%s", r->root, specs.v[i]);
			int is_dir = is_directory(abs);
			int exists = is_dir || is_file(abs);

			free(abs);
			if (!exists)
				continue;
			if (is_dir ? path_is_ignored_dir(r, specs.v[i])
				   : path_is_ignored(r, specs.v[i]))
				slist_push(&ign, specs.v[i]);
		}
		if (ign.nr && !force) {
			int rc;

			slist_sort_unique(&ign);
			rc = refuse_ignored(&ign);
			index_release(&ist);
			slist_release(&paths);
			slist_release(&specs);
			slist_release(&ign);
			return rc;
		}
	}

	if (update_only) {
		/* only paths already tracked are of interest */
		for (i = 0; i < ist.nr; i++) {
			if (!slist_matches(&specs, ist.e[i].path))
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
			if (!slist_matches(&specs, paths.v[i]))
				continue;
			if (dry_run)
				printf("add '%s'\n", paths.v[i]);
			else
				stage_into(r, &ist, paths.v[i]);
		}
		/* -A and an explicit list also record deletions */
		if (all || specs.nr) {
			for (i = 0; i < ist.nr; i++) {
				if (!slist_matches(&specs, ist.e[i].path))
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
	 * -f is the only way a path the rules match is ever staged, and the walk
	 * that filled `paths` left it out, so it is followed from the spec.
	 */
	if (force && !dry_run)
		for (i = 0; i < ign.nr; i++)
			stage_forced(r, &ist, ign.v[i]);

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
	slist_release(&ign);
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

	add_pathspecs(argc, argv,
		      (const char *const[]){ "--cached", NULL }, &o, &specs);
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

	opts_init(&o, argc, argv, NULL);
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
	const char *renamed_from;   /* set on 'R' rows: where the file was */
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
 * stages present, and each combination has a name of its own.  The word
 * carries its colon because the long form pads it to a column of its own.
 */
static void unmerged_desc(const struct index_state *ist, const char *path,
			  char *x, char *y, const char **what)
{
	int mask = (index_get_stage(ist, path, 1) ? 1 : 0) |
		   (index_get_stage(ist, path, 2) ? 2 : 0) |
		   (index_get_stage(ist, path, 3) ? 4 : 0);

	switch (mask) {
	case 1:         /* base only: both sides deleted it */
		*x = 'D'; *y = 'D'; *what = "both deleted:";
		break;
	case 2:         /* ours only: we added it, they never had it */
		*x = 'A'; *y = 'U'; *what = "added by us:";
		break;
	case 3:         /* base and ours: they deleted what we changed */
		*x = 'U'; *y = 'D'; *what = "deleted by them:";
		break;
	case 4:         /* theirs only: they added it */
		*x = 'U'; *y = 'A'; *what = "added by them:";
		break;
	case 5:         /* base and theirs: we deleted what they changed */
		*x = 'D'; *y = 'U'; *what = "deleted by us:";
		break;
	case 6:         /* ours and theirs, no base: both added it */
		*x = 'A'; *y = 'A'; *what = "both added:";
		break;
	case 7:         /* all three: both changed it */
		*x = 'U'; *y = 'U'; *what = "both modified:";
		break;
	default:
		*x = 'U'; *y = 'U'; *what = "unmerged:";
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

	opts_init(&o, argc, argv, (const char *const[]){ "-s", "--short", NULL });
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

	/*
	 * A path the index has lost and a path it has gained holding the same
	 * file is one file that moved, which git reports as a rename rather than
	 * as a deletion beside an addition.  Only a move that was staged is one:
	 * a file moved in the work tree but never added is not in the index under
	 * either name, and git reads that as a deletion and an untracked file.
	 */
	{
		struct rename_list rl;
		size_t k;

		renames_between(&r->odb, &head_ist, &ist, &rl);
		for (k = 0; k < rl.nr; k++) {
			size_t d = nrows, a = nrows, j;

			for (j = 0; j < nrows; j++) {
				if (rows[j].x == 'D' &&
				    !strcmp(rows[j].path, rl.e[k].from))
					d = j;
				else if (rows[j].x == 'A' &&
					 !strcmp(rows[j].path, rl.e[k].to))
					a = j;
			}
			if (d == nrows || a == nrows)
				continue;
			/* both names are the index's own, and outlive the rows */
			rows[d].x = 'R';
			rows[d].renamed_from = rows[d].path;
			rows[d].path = rows[a].path;
			memmove(&rows[a], &rows[a + 1],
				(nrows - a - 1) * sizeof(*rows));
			nrows--;
		}
		rename_list_release(&rl);
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
		for (i = 0; i < nrows; i++) {
			if (rows[i].x == 'R')
				printf("R%c %s -> %s\n", rows[i].y,
				       rows[i].renamed_from, rows[i].path);
			else
				printf("%c%c %s\n", rows[i].x, rows[i].y,
				       rows[i].path);
		}
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
		/*
		 * git prints the staged changes and then the unstaged ones, each
		 * in path order, rather than following the one sort order: a path
		 * with both is named in both sections, and the sections do not
		 * interleave.  The word is padded to git's column either way.
		 */
		int staged, printed;

		for (staged = 1; staged >= 0; staged--) {
			printed = 0;
			for (i = 0; i < nrows; i++) {
				/* `what` is set on unmerged rows and nowhere else;
				 * their letters can be any of UU/AA/DU/UD */
				if (rows[i].what || rows[i].x == '?')
					continue;
				if ((rows[i].x != ' ') != (staged == 1))
					continue;
				if (!printed) {
					printf("\n%s:\n", staged
						? "Changes to be committed"
						: "Changes not staged for commit");
					printed = 1;
				}
				if (staged && rows[i].x == 'R') {
					printf("\t%-11s %s -> %s\n", "renamed:",
					       rows[i].renamed_from, rows[i].path);
					continue;
				}
				printf("\t%-11s %s\n",
				       rows[i].x == 'A' ? "new file:"
							: rows[i].x == 'D'
								  ? "deleted:"
						: rows[i].x == ' ' && rows[i].y == 'D'
								  ? "deleted:"
								  : "modified:",
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
			printf("\t%-16s %s\n", rows[i].what, rows[i].path);
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

/* ------------------------------------------------------------------ */
/* the commit message editor                                           */

/*
 * The command to run as the editor.  First one set wins, as in git: GIT_EDITOR,
 * then the repository's core.editor, then VISUAL, then EDITOR.  The string is a
 * shell command line, not a program name, so that "code --wait" works.
 */
static char *editor_command(struct repo *r)
{
	char *v = NULL;

	v = getenv("GIT_EDITOR");
	if (v && *v)
		return xstrdup(v);
	if (repo_config_get(r, "core.editor", &v) == 0 && v && *v)
		return v;
	free(v);
	v = getenv("VISUAL");
	if (v && *v)
		return xstrdup(v);
	v = getenv("EDITOR");
	if (v && *v)
		return xstrdup(v);
	return NULL;
}

/*
 * The editor is run through the shell, so the path goes in quoted.  Anything
 * that could break out of those quotes is refused rather than escaped, as the
 * transports do with the same kind of argument.
 */
static int run_editor(const char *editor, const char *path)
{
	struct buf cmd = BUF_INIT;
	char *p;
	int rc;

	if (strpbrk(path, "\"\r\n%")) {
		gp_error("commit: refusing to hand '%s' to the shell", path);
		return -1;
	}
	p = xstrdup(path);
#ifdef _WIN32
	{
		char *q;
		for (q = p; *q; q++)
			if (*q == '\\')
				*q = '/';
	}
#endif
	buf_addf(&cmd, "%s \"%s\"", editor, p);
	free(p);
	rc = system(buf_cstr(&cmd));
	buf_release(&cmd);
	return rc == 0 ? 0 : -1;
}

/*
 * git's rule for a message that came back from an editor: a line whose first
 * character is '#' is a comment and is dropped whole, trailing whitespace goes,
 * a run of blank lines becomes one, and the ends are trimmed.  The carriage
 * return matters: an editor on Windows saves the file with CRLF, and a message
 * with a '\r' at the end of every line would otherwise be committed as written.
 */
static void strip_message(struct buf *b)
{
	struct buf out = BUF_INIT;
	size_t i = 0;
	int pending_blank = 0;

	while (i < b->len) {
		size_t start = i, end;
		int blank;

		while (i < b->len && b->b[i] != '\n')
			i++;
		end = i;
		if (i < b->len)
			i++;
		while (end > start &&
		       (b->b[end - 1] == '\r' || b->b[end - 1] == ' ' ||
			b->b[end - 1] == '\t'))
			end--;
		blank = end == start;
		if (blank) {
			/* a blank only means something between two real lines */
			if (out.len)
				pending_blank = 1;
			continue;
		}
		if (b->b[start] == '#')
			continue;
		if (pending_blank) {
			buf_addch(&out, '\n');
			pending_blank = 0;
		}
		buf_add(&out, b->b + start, end - start);
		buf_addch(&out, '\n');
	}
	buf_reset(b);
	buf_add(b, out.b, out.len);
	buf_release(&out);
}

/*
 * Put the message through an editor.  `msg` is what the buffer starts with --
 * a merge's own message, or the one --amend is replacing -- and comes back as
 * what the editor left behind, stripped.  Returns -1 when the commit must not
 * go ahead, having said why.
 */
static int edit_message(struct repo *r, struct buf *msg)
{
	struct buf file = BUF_INIT;
	char *path = repo_git_path(r, "COMMIT_EDITMSG");
	char *editor = editor_command(r);
	char *branch;
	int rc = -1;

	if (!editor) {
		gp_error("commit: no editor is configured\n"
			 "hint: set GIT_EDITOR or EDITOR, or pass -m \"...\" to "
			 "write the message on the command line");
		goto out;
	}

	buf_add(&file, msg->b, msg->len);
	if (file.len && file.b[file.len - 1] != '\n')
		buf_addch(&file, '\n');
	buf_addstr(&file, "# Please enter the commit message for your changes. "
		   "Lines starting\n# with '#' will be ignored, and an empty "
		   "message aborts the commit.\n#\n");
	branch = refs_head_target(&r->refs);
	if (branch) {
		const char *name = branch;
		if (!strncmp(name, "refs/heads/", 11))
			name += 11;
		buf_addf(&file, "# On branch %s\n", name);
	} else {
		buf_addstr(&file, "# Not currently on any branch.\n");
	}
	free(branch);

	if (write_file(path, file.b, file.len) < 0) {
		gp_error("commit: cannot write %s", path);
		goto out;
	}
	if (run_editor(editor, path) < 0) {
		gp_error("commit: the editor exited with an error; nothing was "
			 "committed");
		goto out;
	}
	if (read_file(path, msg) < 0) {
		gp_error("commit: cannot read back %s", path);
		goto out;
	}
	strip_message(msg);
	if (!msg->len) {
		gp_error("Aborting commit due to empty commit message.");
		goto out;
	}
	rc = 0;
out:
	free(editor);
	free(path);
	buf_release(&file);
	return rc;
}

/*
 * The message a commit already has: a merge's, so that a bare `commit` can
 * conclude one, or the message --amend is about to replace.
 */
static int existing_message(struct repo *r, int amend, struct buf *out)
{
	oid_t head;
	struct commit c = COMMIT_INIT;

	if (merge_message(r, out))
		return 0;
	if (!amend)
		return -1;
	if (refs_head(&r->refs, &head) < 0)
		return -1;
	read_commit(r, &head, &c);
	if (c.message)
		buf_addstr(out, c.message);
	commit_release(&c);
	return out->len ? 0 : -1;
}

int cmd_commit(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct buf msg;
	const char *m;
	int amend, edit = 0;

	opts_init(&o, argc, argv, (const char *const[]){
		"-m=", "-F=", "-e", "--edit", "--no-edit", "-a", "--all",
		"--amend", "--allow-empty", "-q", NULL });
	amend = opts_flag(&o, "--amend");

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
	} else if (opts_flag(&o, "--no-edit")) {
		/* say the same thing again: a merge's message, or the old one */
		if (existing_message(r, amend, &msg) < 0) {
			buf_release(&msg);
			gp_error("commit: no message given\nhint: pass -m \"...\" "
				 "or -F <file>, or leave --no-edit off to write "
				 "the message in an editor");
			return 1;
		}
	} else {
		/* start from whatever the commit already has, if anything */
		existing_message(r, amend, &msg);
		edit = 1;
	}
	/* -e asks for the editor even when the command line carried a message */
	if (opts_flag(&o, "-e") || opts_flag(&o, "--edit"))
		edit = 1;
	if (edit && edit_message(r, &msg) < 0) {
		buf_release(&msg);
		return 1;
	}
	if (msg.len && msg.b[msg.len - 1] != '\n')
		buf_addch(&msg, '\n');

	{
		int rc = do_commit(r, buf_cstr(&msg), amend,
				   opts_flag(&o, "--allow-empty"),
				   opts_flag(&o, "-q"));
		buf_release(&msg);
		return rc;
	}
}

/* ------------------------------------------------------------------ */
/* the prompts a commit carries                                        */

struct prev_scan {
	struct slist *seen;
};

static void prev_prompt_cb(const char *path, u32 mode, const oid_t *oid, void *ud)
{
	struct prev_scan *s = ud;
	char hex[GP_SHA1_HEXSZ + 1];
	struct buf b;

	if (mode == MODE_TREE)
		return;
	oid_hex(oid, hex);
	buf_init(&b);
	buf_addf(&b, "%s %s", hex, path);
	slist_push(s->seen, buf_cstr(&b));
	buf_release(&b);
}

/*
 * The subtree a repository-relative directory names, following one component at
 * a time.  The prompt directory is a configuration value and may be nested; a
 * single lookup of the whole name would ask a tree for an entry called
 * "docs/prompts", which no tree has, and the previous tree would look empty --
 * so every prompt in the commit would look new and a prompt that had not
 * changed would be carried a second time.
 */
static int lookup_dir_tree(struct repo *r, const oid_t *root, const char *dir,
			   oid_t *out)
{
	oid_t cur = *root;
	const char *p = dir;

	while (*p == '/')
		p++;
	while (*p) {
		const char *slash = strchr(p, '/');
		size_t n = slash ? (size_t)(slash - p) : strlen(p);
		struct tree t = TREE_INIT;
		char *name;
		u32 mode;
		int found;

		if (!n) {
			p++;
			continue;
		}
		name = xstrndup(p, n);
		read_tree_obj(r, &cur, &t);
		found = tree_lookup(&t, name, &mode, &cur, NULL) &&
			(mode & 0170000) == 0040000;
		tree_release(&t);
		free(name);
		if (!found)
			return -1;
		p = slash ? slash + 1 : p + n;
	}
	*out = cur;
	return 0;
}

int is_prompt_path(const char *dir, size_t dl, const char *path,
		   const char **rest)
{
	size_t n, rn;

	if (strncmp(path, dir, dl) || path[dl] != '/')
		return 0;
	*rest = path + dl + 1;
	rn = strlen(*rest);
	if (rn <= 3 || strchr(*rest, '/'))
		return 0;
	n = rn - 3;
	return !strcmp(*rest + n, ".md");
}

/*
 * A commit carries the prompts that produced the code in it: the prompt
 * files it adds or changes, as the previous tree tells them apart.  The
 * ids are read out of the index's blobs rather than the work tree, so a
 * commit carries exactly what it committed, and a prompt whose outcome or
 * body was edited after it was recorded is carried by the commit that
 * carried the edit.
 */
void collect_commit_prompts(struct repo *r, const struct index_state *ist,
			    const oid_t *prev_tree, int have_prev,
			    struct commit *c)
{
	struct slist seen = { NULL, 0, 0 };
	char *dir = xstrdup(repo_prompt_dir(r));
	size_t dl = strlen(dir), i;

	if (have_prev) {
		oid_t sub;

		if (lookup_dir_tree(r, prev_tree, dir, &sub) == 0) {
			char *prefix = xstrfmt("%s/", dir);
			struct prev_scan ps;

			ps.seen = &seen;
			load_tree_flat(r, &sub, prefix, prev_prompt_cb, &ps);
			free(prefix);
		}
	}

	for (i = 0; i < ist->nr; i++) {
		const struct index_entry *e = &ist->e[i];
		const char *rest;
		char hex[GP_SHA1_HEXSZ + 1];
		struct buf key, blob;
		struct prompt p = PROMPT_INIT;

		if (e->stage != 0 || !is_prompt_path(dir, dl, e->path, &rest))
			continue;

		oid_hex(&e->oid, hex);
		buf_init(&key);
		buf_addf(&key, "%s %s", hex, e->path);
		if (slist_has(&seen, buf_cstr(&key))) {
			buf_release(&key);
			continue;
		}
		buf_release(&key);

		buf_init(&blob);
		if (odb_read(&r->odb, &e->oid, NULL, &blob) < 0) {
			buf_release(&blob);
			continue;
		}
		if (prompt_from_file(&p, blob.b, blob.len) &&
		    p.id && !strncmp(p.id, "p_", 2)) {
			size_t k;
			int have = 0;

			/* an amend carries its prompts already, and a prompt
			 * edited since then is the same prompt, not a second one */
			for (k = 0; k < c->nr_prompts; k++)
				if (!strcmp(c->prompts[k], p.id)) {
					have = 1;
					break;
				}
			if (!have) {
				c->prompts = xrealloc(c->prompts,
					(c->nr_prompts + 1) * sizeof(*c->prompts));
				c->prompts[c->nr_prompts++] = xstrdup(p.id);
			}
		}
		prompt_release(&p);
		buf_release(&blob);
	}

	slist_release(&seen);
	free(dir);
}

static int do_commit(struct repo *r, const char *message, int amend,
		     int allow_empty, int quiet)
{
	struct index_state ist;
	struct commit c = COMMIT_INIT;
	struct buf body, ident;
	oid_t tree, parent, commit_oid, prev_tree;
	char hex[GP_SHA1_HEXSZ + 1];
	char *br = NULL, *branch_ref = NULL;
	int had_head, have_prev;

	/*
	 * A replayed commit is recorded by the replay, which also knows what is
	 * still to come; recording it here instead would make it by hand and
	 * leave the state naming a commit that has already been made.
	 */
	{
		char *kind = NULL;

		if (replay_in_progress(r, &kind)) {
			gp_error("a %s is in progress", kind);
			fprintf(stderr, "hint: record the commit and carry on with "
					"'gitprompt %s --continue'\n", kind);
			free(kind);
			return 1;
		}
	}

	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));

	had_head = head_tree(r, &tree) == 0;   /* tree = old HEAD tree */
	prev_tree = tree;
	have_prev = had_head;
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
		/* and the prompts it carried, which the new tree still holds */
		{
			size_t i;
			for (i = 0; i < old.nr_prompts; i++) {
				c.prompts = xrealloc(c.prompts,
					(c.nr_prompts + 1) * sizeof(*c.prompts));
				c.prompts[c.nr_prompts++] = xstrdup(old.prompts[i]);
			}
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

	collect_commit_prompts(r, &ist, &prev_tree, have_prev, &c);

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
		refs_reflog_head(&r->refs, branch_ref, had ? &old : &null_oid,
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

void format_author_line(const char *raw, struct buf *out)
{
	/* raw is "Name <email> 1700000000 +0800"; drop the timestamp */
	char *name, *email;

	buf_reset(out);
	parse_ident(raw, &name, &email);
	if (!name)
		return;
	if (email)
		buf_addf(out, "%s <%s>", name, email);
	else
		buf_addstr(out, name);
	free(name);
	free(email);
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
	if (c->nr_prompts) {
		size_t i;

		printf("Prompts:");
		for (i = 0; i < c->nr_prompts; i++)
			printf(" %s", c->prompts[i]);
		printf("\n");
	}
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
	struct opts o;
	struct oid_array tips = OID_ARRAY_INIT;
	struct log_ctx ctx;
	oid_t start;
	const char *n;

	opts_init(&o, argc, argv, (const char *const[]){
		"--oneline", "-n=", "--max-count=", NULL });
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

static void print_prompt_carriers(struct repo *r, const char *id);

static void print_prompt_detail(struct repo *r, const struct prompt_ref *ref)
{
	const struct prompt *p = ref->prompt;

	printf("prompt %s\n", p->id ? p->id : "?");
	if (p->session)
		printf("  session:   %s\n", p->session);
	printf("  seq:       %d\n", p->seq);
	if (p->timestamp)
		printf("  timestamp: %s\n", p->timestamp);
	if (p->author)
		printf("  author:    %s\n", p->author);
	if (p->model)
		printf("  model:     %s\n", p->model);
	if (p->outcome)
		printf("  outcome:   %s\n", p->outcome);
	if (p->parent_prompt)
		printf("  answers:   %s\n", p->parent_prompt);
	if (p->nr_tags) {
		size_t k;

		printf("  tags:      ");
		for (k = 0; k < p->nr_tags; k++)
			printf("%s%s", k ? ", " : "", p->tags[k]);
		printf("\n");
	}
	if (ref->path)
		printf("  file:      %s\n", ref->path);
	if (p->id)
		print_prompt_carriers(r, p->id);
	printf("\n");
	body_print_indented(p->body, "  ");

	if (p->response) {
		printf("\nresponse %s", p->response->id);
		if (p->response->model)
			printf("  %s", p->response->model);
		printf("\n\n");
		body_print_indented(p->response->body, "  ");
	}
}

/*
 * The commits that carry a prompt, oldest first.
 *
 * A prompt is carried by the commits whose header names it, and it is the one
 * thing about a prompt that history can answer on its own -- no snapshot is
 * needed, because "which commits named this prompt" is written in the commit
 * headers.  A prompt replayed onto another branch is carried by the copy there
 * as well as by the original, so this is a list and not a single commit, and
 * it is listed in the order the work happened rather than the order the walk
 * found it, which is what a reader asking "where did this prompt land" wants.
 */
struct carrier {
	oid_t oid;
	i64 when;
	char *subject;
};

struct carries {
	struct carrier *e;
	size_t nr, alloc;
};

struct carries_ctx {
	const char *id;
	struct carries *c;
};

static void carries_cb(const oid_t *oid, const struct commit *cm, void *ud)
{
	struct carries_ctx *c = ud;
	struct carrier *k;
	size_t i;

	for (i = 0; i < cm->nr_prompts; i++) {
		if (!cm->prompts[i] || strcmp(cm->prompts[i], c->id))
			continue;
		if (c->c->nr == c->c->alloc) {
			c->c->alloc = c->c->alloc ? c->c->alloc * 2 : 8;
			c->c->e = xrealloc(c->c->e,
					   c->c->alloc * sizeof(*c->c->e));
		}
		k = &c->c->e[c->c->nr++];
		k->oid = *oid;
		k->when = commit_time(cm);
		k->subject = commit_message_line(cm);
		return;
	}
}

static int carrier_cmp(const void *a, const void *b)
{
	const struct carrier *x = a, *y = b;

	if (x->when != y->when)
		return x->when < y->when ? -1 : 1;
	return 0;
}

static void tips_cb(const char *name, const oid_t *oid, void *ud)
{
	struct oid_array *out = ud;

	(void)name;
	if (!oid_array_contains(out, oid))
		oid_array_append(out, oid);
}

static void print_prompt_carriers(struct repo *r, const char *id)
{
	struct oid_array tips = OID_ARRAY_INIT;
	struct carries cs;
	struct carries_ctx cc;
	size_t i;

	memset(&cs, 0, sizeof cs);
	refs_list(&r->refs, "refs/heads/", tips_cb, &tips);
	refs_list(&r->refs, "refs/tags/", tips_cb, &tips);
	refs_list(&r->refs, "refs/remotes/", tips_cb, &tips);
	refs_list_packed(&r->refs, "refs/", tips_cb, &tips);

	cc.id = id;
	cc.c = &cs;
	walk_commits(r, &tips, carries_cb, &cc);
	oid_array_clear(&tips);

	if (!cs.nr)
		return;

	qsort(cs.e, cs.nr, sizeof(*cs.e), carrier_cmp);
	printf("  carried by:\n");
	for (i = 0; i < cs.nr; i++) {
		struct buf when;
		char *ab = abbrev_oid(&cs.e[i].oid);

		buf_init(&when);
		epoch_to_iso8601(cs.e[i].when, &when);
		printf("    %s %s %s\n", ab, buf_cstr(&when), cs.e[i].subject);
		buf_release(&when);
		free(ab);
		free(cs.e[i].subject);
	}
	free(cs.e);
}

/*
 * `show` advertises a prompt id next to a revision, but a prompt is not an
 * object -- it is a file in the history, which is why resolve_rev cannot see
 * one and `show p_...` used to die with "unknown revision".  Reading the id out
 * of the collected history is what makes the advertised form work, and it
 * answers a session id and a response id too, since all three are ids a reader
 * gets off one of the listings and then wants to look at.
 *
 * Returns 1 when the id named something here and it was shown, 0 when it has
 * the shape of an id and names nothing, and -1 when it does not have that shape
 * at all -- the caller reports the last of those as a revision that did not
 * resolve, which is what it is, rather than as a missing prompt.
 */
static int show_gp_id(struct repo *r, const char *want)
{
	struct prompt_list pl;
	struct session_groups sg;
	size_t i, j;
	int shown = 0;

	/* only the three id shapes are ours; anything else is a revision that
	 * genuinely did not resolve, and "no prompt with id mainn" would be a
	 * worse answer than the one it gets */
	if (!want || !want[0] || want[1] != '_' ||
	    (want[0] != 'p' && want[0] != 's' && want[0] != 'r'))
		return -1;

	load_groups(r, &pl, &sg);
	for (i = 0; i < sg.nr && !shown; i++) {
		const struct session_group *g = &sg.g[i];

		/* this loop is where the session was found, so it exists and
		 * session_show cannot answer "no such session" */
		if (g->session && !strcmp(g->session->id, want)) {
			session_groups_release(&sg);
			prompt_list_release(&pl);
			session_show(r, want);
			return 1;
		}
		for (j = 0; j < g->prompts.nr; j++) {
			const struct prompt_ref *ref = &g->prompts.e[j];
			const struct prompt *p = ref->prompt;

			if (p->id && !strcmp(p->id, want)) {
				print_prompt_detail(r, ref);
				shown = 1;
				break;
			}
			if (p->response && p->response->id &&
			    !strcmp(p->response->id, want)) {
				printf("response %s\n", p->response->id);
				if (p->response->model)
					printf("  model:     %s\n",
					       p->response->model);
				printf("  answers:   %s\n", p->id);
				if (p->response->path)
					printf("  file:      %s\n",
					       p->response->path);
				printf("\n");
				body_print_indented(p->response->body, "  ");
				shown = 1;
				break;
			}
		}
	}
	session_groups_release(&sg);
	prompt_list_release(&pl);
	return shown;
}

/*
 * A commit's change, told the way it was asked for: one step per prompt, each
 * under the prompt that asked for it.  A commit with no chain -- one recorded
 * before snapshots were kept, or one whose prompts came from somewhere else --
 * gets the ordinary diff and a line saying why, because a diff with no prompt
 * names on it is still the whole truth about what changed.
 */
static void trace_or_diff(struct repo *r, const struct commit *c,
			  const oid_t *old_tree, int have_old, struct buf *out)
{
	struct trace tr;

	trace_of_commit(r, c, &tr);
	if (tr.nr) {
		trace_render(r, &tr, out);
	} else {
		if (c->nr_prompts && tr.why)
			buf_addf(out, "(no breakdown: %s)\n", tr.why);
		diff_trees(r, have_old ? old_tree : NULL, &c->tree, out, 0);
	}
	trace_release(&tr);
}

int cmd_show(struct repo *r, int argc, char **argv)
{
	struct opts o;
	oid_t oid;
	enum obj_type t;

	opts_init(&o, argc, argv,
		   (const char *const[]){ "--stat", "--prompt-hunks", NULL });
	if (!opts_arg(&o, 0)) {
		gp_error("show: expected a revision");
		return 1;
	}
	if (resolve_rev(r, opts_arg(&o, 0), &oid) < 0) {
		int found = show_gp_id(r, opts_arg(&o, 0));

		if (found > 0)
			return 0;
		if (found == 0)
			gp_die("show: no prompt, session or response has the "
			       "id %s", opts_arg(&o, 0));
		gp_die("show: unknown revision: %s", opts_arg(&o, 0));
	}

	if (odb_type_of(&r->odb, &oid, &t) < 0)
		gp_die("show: cannot read %s", opts_arg(&o, 0));

	if (t == OBJ_COMMIT) {
		char hex[GP_SHA1_HEXSZ + 1];
		struct commit c = COMMIT_INIT;
		struct buf diff;
		oid_t old_tree, new_tree;
		int have_old = 0;
		int stat_only = opts_flag(&o, "--stat");

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
			if (c.nr_prompts) {
				size_t i;

				printf("Prompts:");
				for (i = 0; i < c.nr_prompts; i++)
					printf(" %s", c.prompts[i]);
				printf("\n");
			}
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

		if (opts_flag(&o, "--prompt-hunks") && stat_only) {
			gp_error("show: --stat and --prompt-hunks cannot both be "
				 "asked for");
			commit_release(&c);
			return 1;
		}

		buf_init(&diff);
		if (opts_flag(&o, "--prompt-hunks"))
			trace_or_diff(r, &c, &old_tree, have_old, &diff);
		else
			diff_trees(r, have_old ? &old_tree : NULL, &new_tree,
				   &diff, stat_only);
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

void diff_split_lines(const void *data, size_t len, struct dline **out, size_t *nr)
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

/*
 * A plain longest-common-subsequence diff.  The table is quadratic, so
 * a pathological pair of files falls back to "replace everything" rather
 * than allocating gigabytes.
 */
int lcs_diff(const struct dline *a, size_t na, const struct dline *b,
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

/*
 * The name git puts after a hunk's range: the last line above the hunk that
 * begins with a letter, an underscore or a dollar sign, which is the guess git
 * makes at the enclosing function when it has no language-aware pattern to go
 * on -- the line above a hunk in ordinary prose is usually the one that says
 * what the hunk is about.  Trailing whitespace goes and the line is cut at 79
 * columns, both as git trims them, and a hunk at the top of a file has no line
 * above it to name.
 */
static size_t funcname_before(const struct dline *a, size_t line,
			      const char **out)
{
	while (line) {
		const struct dline *l = &a[--line];
		size_t n = l->len;

		if (!n || !(isalpha((unsigned char)l->p[0]) ||
			    l->p[0] == '_' || l->p[0] == '$'))
			continue;
		while (n && isspace((unsigned char)l->p[n - 1]))
			n--;
		if (n > 79)
			n = 79;
		*out = l->p;
		return n;
	}
	return 0;
}

static int append_hunks(const struct oline *o, size_t n,
			const struct dline *la, struct buf *out)
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
			/*
			 * A side's count is written only when it is not one,
			 * and when it is none the line before the range is
			 * named instead, so that "-0,0" reads as "above the
			 * first line" -- the two spellings a patch reader
			 * takes, and the ones git writes.
			 */
			buf_addf(out, "@@ -%lu",
				 (unsigned long)(a_len ? a_start : a_start - 1));
			if (a_len != 1)
				buf_addf(out, ",%lu", (unsigned long)a_len);
			buf_addf(out, " +%lu",
				 (unsigned long)(b_len ? b_start : b_start - 1));
			if (b_len != 1)
				buf_addf(out, ",%lu", (unsigned long)b_len);
			buf_addstr(out, " @@");
			{
				const char *fn;
				size_t fnlen = funcname_before(la, o[first].a,
							       &fn);

				if (fnlen) {
					buf_addch(out, ' ');
					buf_add(out, fn, fnlen);
				}
			}
			buf_addch(out, '\n');
			for (k = first; k < last; k++) {
				buf_addch(out, o[k].op);
				buf_add(out, o[k].p, o[k].len);
				if (o[k].len && o[k].p[o[k].len - 1] == '\n')
					continue;
				/*
				 * A last line with no newline ends the file it
				 * came from, and a diff has to say so: without
				 * this the line reads as one that has a newline,
				 * which is a different line -- and, since a line
				 * compares as much by its newline as by its text,
				 * it is the only way to tell the two apart.
				 */
				buf_addch(out, '\n');
				buf_addstr(out,
					   "\\ No newline at end of file\n");
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

	diff_split_lines(a ? a : "", a ? alen : 0, &la, &na);
	diff_split_lines(b ? b : "", b ? blen : 0, &lb, &nb);
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
		append_hunks(o, n, la, out);
	}

	free(la);
	free(lb);
	free(o);
	return 1;
}

/* ------------------------------------------------------------------ */

/* walk two trees in parallel and emit a diff per differing path */
/*
 * One line of --stat, gathered rather than written.  The lines cannot go out as
 * they are found, because git pads every path to the longest one in the same
 * output and sizes each bar against the largest change in it -- figures that
 * are only known once the last file has been seen.  So the rows are kept and
 * laid out together at the end.
 */
struct stat_row {
	char *label;            /* owned; a move reads "old => new" */
	long add, del;          /* changed lines, or the two byte counts of a binary */
	int binary;
};

struct stat_totals {
	size_t files, adds, dels;
	long max_change;
	long bin_width;         /* widest "Bin XXX -> YYY bytes" */
	struct stat_row *row;
	size_t nr, alloc;
};

struct tdiff_ctx {
	struct repo *r;
	struct buf *out;
	int stat_only;
	const struct rename_list *rl;   /* the moves to report as renames */
	const struct index_state *old_ist;  /* where a move's old name is */
	struct stat_totals totals;
};

static struct stat_row *stat_row_new(struct stat_totals *t, const char *label)
{
	struct stat_row *r;

	if (t->nr == t->alloc) {
		t->alloc = t->alloc ? t->alloc * 2 : 16;
		t->row = xrealloc(t->row, t->alloc * sizeof *t->row);
	}
	r = &t->row[t->nr++];
	memset(r, 0, sizeof *r);
	r->label = xstrdup(label);
	return r;
}

/*
 * The number of changed lines between two texts, as a --stat row.  A move names
 * both paths, which is what tells a reader where the file went.
 */
static void stat_line(struct stat_totals *t, const char *label, size_t add,
		      size_t del)
{
	struct stat_row *r = stat_row_new(t, label);

	r->add = (long)add;
	r->del = (long)del;
	if ((long)(add + del) > t->max_change)
		t->max_change = (long)(add + del);
	t->files++;
	t->adds += add;
	t->dels += del;
}

static long decimal_width(long v)
{
	long n = 1;

	while (v >= 10) {
		v /= 10;
		n++;
	}
	return n;
}

/* git's row for a file whose contents are not lines */
static void stat_binary_line(struct stat_totals *t, const char *label,
			     size_t old_len, size_t new_len)
{
	struct stat_row *r = stat_row_new(t, label);
	long w;

	r->binary = 1;
	r->add = (long)new_len;
	r->del = (long)old_len;
	/* the row reads "Bin OLD -> NEW bytes", so those sizes set the width too */
	w = 14 + decimal_width(r->del) + decimal_width(r->add);
	if (w > t->bin_width)
		t->bin_width = w;
	t->files++;
}

/*
 * The width --stat has to lay itself out in: what the terminal reports through
 * COLUMNS, or the 80 git falls back on when nothing says otherwise.
 */
static long stat_width(void)
{
	const char *c = getenv("COLUMNS");
	long n;

	if (c && *c) {
		n = strtol(c, NULL, 10);
		if (n > 0)
			return n;
	}
	return 80;
}

/*
 * How much of a bar a change of `it` out of `max_change` lines earns.  git
 * spends the columns linearly, and gives every change that is not zero at
 * least one column, so that a file which changed cannot be drawn as one that
 * did not -- that rounding up is the whole point of the sum below.
 */
static long stat_bar(long it, long graph_width, long max_change)
{
	if (!it)
		return 0;
	if (max_change <= 0)
		return it;
	return (it * (graph_width - 1)) / max_change + 1;
}

/* a padding count that printf will not read as a left-justify flag */
static int pad(long width, long len)
{
	return len < width ? (int)(width - len) : 0;
}

/*
 * git's closing line for --stat.  Each of the two terms is printed when it is
 * not zero, or when the other one is and something did change -- so a move
 * that changed no lines reads "1 file changed, 0 insertions(+), 0
 * deletions(-)" and a pure deletion reads "1 file changed, 1 deletion(-)".
 */
static void stat_summary(const struct stat_totals *t, struct buf *out)
{
	buf_addf(out, " %lu file%s changed", (unsigned long)t->files,
		 t->files == 1 ? "" : "s");
	if (t->adds || (!t->dels && t->files))
		buf_addf(out, ", %lu insertion%s(+)", (unsigned long)t->adds,
			 t->adds == 1 ? "" : "s");
	if (t->dels || (!t->adds && t->files))
		buf_addf(out, ", %lu deletion%s(-)", (unsigned long)t->dels,
			 t->dels == 1 ? "" : "s");
	buf_addch(out, '\n');
}

/*
 * Lay the gathered rows out and write them, followed by the summary.
 *
 * Three things share the width: the paths, the changed-line count, and the bar.
 * The bar gives up its columns first, but only down to a floor -- an eighth of
 * the width below three eighths of it, so 6 columns at the standard 80 -- since
 * a bar with no height in it says nothing at all.  Whatever those two leave
 * goes to the paths, and only when they still do not fit is a path cut, because
 * a reader can spare the front of a path more easily than the change beside it.
 */
static void stat_render(struct stat_totals *t, struct buf *out)
{
	long width = stat_width();
	long name_width = 0, number_width, graph_width;
	long i;

	for (i = 0; i < (long)t->nr; i++) {
		long l = (long)strlen(t->row[i].label);

		if (l > name_width)
			name_width = l;
	}
	/* a "Bin" row is three columns wide and can be wider than any count */
	number_width = t->bin_width ? 3 : 0;
	if (decimal_width(t->max_change) > number_width)
		number_width = decimal_width(t->max_change);

	/* below this there is not even room for the columns themselves */
	if (width < 16 + 6 + number_width)
		width = 16 + 6 + number_width;

	graph_width = t->max_change + 4 > t->bin_width
		? t->max_change : t->bin_width - 4;

	if (name_width + number_width + 6 + graph_width > width) {
		if (graph_width > width * 3 / 8 - number_width - 6) {
			graph_width = width * 3 / 8 - number_width - 6;
			if (graph_width < 6)
				graph_width = 6;
		}
		if (name_width > width - number_width - 6 - graph_width)
			name_width = width - number_width - 6 - graph_width;
		else
			graph_width = width - number_width - 6 - name_width;
	}

	for (i = 0; i < (long)t->nr; i++) {
		struct stat_row *r = &t->row[i];
		const char *name = r->label;
		const char *prefix = "";
		long name_len = (long)strlen(name);
		long len = name_width, padding;

		/*
		 * A path too long for its column keeps its tail -- the end of a
		 * path is the part that says which file it is -- and the
		 * directories dropped in front of it become "...".  The tail is
		 * cut again at the first slash in it, so what is left reads as a
		 * path rather than starting in the middle of a name.
		 */
		if (name_width < name_len) {
			const char *slash;

			prefix = "...";
			len -= 3;
			if (len < 0)
				len = 0;
			while (name_len > len && *name) {
				name++;
				name_len--;
			}
			slash = strchr(name, '/');
			if (slash)
				name = slash;
		}
		padding = len - (long)strlen(name);
		if (padding < 0)
			padding = 0;

		if (r->binary) {
			buf_addf(out, " %s%s%*s | %*s", prefix, name,
				 pad(len, (long)strlen(name)), "",
				 (int)number_width, "Bin");
			if (!r->add && !r->del) {
				buf_addch(out, '\n');
				continue;
			}
			buf_addf(out, " %ld -> %ld bytes\n", r->del, r->add);
			continue;
		}

		/*
		 * The bar is drawn only while it can stand for the largest
		 * change in the output; once it is wider than that, the counts
		 * beside it are the whole story and are printed unscaled.
		 */
		{
			long add = r->add, del = r->del;

			if (graph_width <= t->max_change) {
				long total = stat_bar(add + del, graph_width,
						      t->max_change);

				if (total < 2 && add && del)
					total = 2;
				if (add < del) {
					add = stat_bar(add, graph_width,
						       t->max_change);
					del = total - add;
				} else {
					del = stat_bar(del, graph_width,
						       t->max_change);
					add = total - del;
				}
			}
			buf_addf(out, " %s%s%*s | %*ld", prefix, name,
				 (int)padding, "", (int)number_width,
				 r->add + r->del);
			if (r->add + r->del) {
				long k;

				buf_addch(out, ' ');
				for (k = 0; k < add; k++)
					buf_addch(out, '+');
				for (k = 0; k < del; k++)
					buf_addch(out, '-');
			}
			buf_addch(out, '\n');
		}
	}
	stat_summary(t, out);
}

static void stat_totals_release(struct stat_totals *t)
{
	size_t i;

	for (i = 0; i < t->nr; i++)
		free(t->row[i].label);
	free(t->row);
	memset(t, 0, sizeof *t);
}

/* how many lines two texts have changed between them, for a stat line */
static void diff_counts(const void *a, size_t alen, const void *b, size_t blen,
			struct stat_counts *counts)
{
	struct dline *la = NULL, *lb = NULL;
	struct oline *o = NULL;
	size_t na = 0, nb = 0, n = 0;

	diff_split_lines(a ? a : "", a ? alen : 0, &la, &na);
	diff_split_lines(b ? b : "", b ? blen : 0, &lb, &nb);
	lcs_diff(la, na, lb, nb, &o, &n, counts);
	free(la);
	free(lb);
	free(o);
}

static int tdiff_binary(const struct buf *a, const struct buf *b)
{
	return looks_binary(a->b, a->len) || looks_binary(b->b, b->len);
}

static void tdiff_stat(struct tdiff_ctx *c, const char *label,
		       const struct buf *a, const struct buf *b)
{
	struct stat_counts counts;

	if (tdiff_binary(a, b)) {
		stat_binary_line(&c->totals, label, a->len, b->len);
		return;
	}
	diff_counts(a->b, a->len, b->b, b->len, &counts);
	stat_line(&c->totals, label, counts.add, counts.del);
}

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

	if (c->stat_only) {
		tdiff_stat(c, path, &a, &b);
		buf_release(&a);
		buf_release(&b);
		return;
	}

	buf_addf(c->out, "diff --git a/%s b/%s\n", path, path);

	/*
	 * A file that arrived or went is named as such, and a mode that
	 * changed is spelled out on its own pair of lines; either way the mode
	 * is on a line of its own, which is why the index line below carries a
	 * mode only in the case left over -- both sides present, both the same
	 * mode -- and why it carries none when one of them is absent.
	 */
	if (!old_oid)
		buf_addf(c->out, "new file mode %06o\n", new_mode);
	else if (!new_oid)
		buf_addf(c->out, "deleted file mode %06o\n", old_mode);
	else if (old_mode != new_mode)
		buf_addf(c->out, "old mode %06o\nnew mode %06o\n", old_mode,
			 new_mode);

	/*
	 * The index line names the two blobs the hunks below are the difference
	 * between, so it appears exactly when there are two different blobs to
	 * name: a file that arrived or went has the null id on the side that is
	 * missing, and a mode that changed on its own, or a file that moved
	 * whole, has the same id on both and so gets no line at all.
	 */
	{
		const oid_t *o = old_oid ? old_oid : &null_oid;
		const oid_t *nw = new_oid ? new_oid : &null_oid;

		if (!oid_equal(o, nw)) {
			char *o1 = abbrev_oid(o);
			char *o2 = abbrev_oid(nw);

			buf_addf(c->out, "index %s..%s", o1, o2);
			if (old_oid && new_oid && old_mode == new_mode)
				buf_addf(c->out, " %06o", new_mode);
			buf_addch(c->out, '\n');
			free(o1);
			free(o2);
		}
	}

	alab = old_oid ? xstrfmt("a/%s", path) : xstrdup("/dev/null");
	blab = new_oid ? xstrfmt("b/%s", path) : xstrdup("/dev/null");
	diff_buffers(alab, a.b, a.len, blab, b.b, b.len, c->out, 0);
	free(alab);
	free(blab);
	buf_release(&a);
	buf_release(&b);
}

/*
 * A move, as git prints it: the header names both paths, how much of the file
 * survived, and the two names in full, followed by the change to the contents
 * when the move carried one.
 *
 * The share is the share of lines the two files still have in common, which is
 * the score the merge uses to recognise the move in the first place.  git's
 * number is an estimate over bytes rather than lines, so the two agree on a
 * file that moved whole, and can differ by a few points on one that was edited
 * on the way -- which is why a file that did not change at all is the one case
 * that prints git's own word for it, 100%.
 */
static void tdiff_emit_rename(struct tdiff_ctx *c, const char *from,
			      const char *to, const oid_t *old_oid,
			      u32 old_mode, const oid_t *new_oid, u32 new_mode)
{
	struct buf a, b;
	char *label;

	buf_init(&a);
	buf_init(&b);
	odb_read(&c->r->odb, old_oid, NULL, &a);
	odb_read(&c->r->odb, new_oid, NULL, &b);

	if (c->stat_only) {
		label = xstrfmt("%s => %s", from, to);
		tdiff_stat(c, label, &a, &b);
		free(label);
		buf_release(&a);
		buf_release(&b);
		return;
	}

	buf_addf(c->out, "diff --git a/%s b/%s\n", from, to);
	buf_addf(c->out, "similarity index %d%%\n",
		 oid_equal(old_oid, new_oid) ? 100
					     : merge3_similarity(&a, &b));
	buf_addf(c->out, "rename from %s\n", from);
	buf_addf(c->out, "rename to %s\n", to);
	if (!oid_equal(old_oid, new_oid)) {
		char *alab = xstrfmt("a/%s", from);
		char *blab = xstrfmt("b/%s", to);
		char *o1 = abbrev_oid(old_oid);
		char *o2 = abbrev_oid(new_oid);

		/* the same rule the pair of names on the line above does not
		 * change: the ids differ, so the two blobs are named */
		if (old_mode != new_mode)
			buf_addf(c->out, "old mode %06o\nnew mode %06o\n",
				 old_mode, new_mode);
		buf_addf(c->out, "index %s..%s", o1, o2);
		if (old_mode == new_mode)
			buf_addf(c->out, " %06o", new_mode);
		buf_addch(c->out, '\n');

		diff_buffers(alab, a.b, a.len, blab, b.b, b.len, c->out, 0);
		free(o1);
		free(o2);
		free(alab);
		free(blab);
	}
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
			else if (!rename_by_from(c->rl, p))
				/* a path that moved is reported where it went */
				tdiff_emit(c, p, &ot.e[i].oid, ot.e[i].mode, NULL, 0);
			free(p);
			free(full);
			i++;
		} else if (cmp > 0) {
			char *p = xstrfmt("%s%s", prefix, nt.e[j].name);
			char *full = xstrfmt("%s/", p);
			const struct rename_pair *rp = rename_by_to(c->rl, p);
			if ((nt.e[j].mode & 0170000) == 0040000)
				tdiff_recurse(c, full, NULL, &nt.e[j].oid);
			else if (rp) {
				const struct index_entry *oe =
					index_get(c->old_ist, rp->from);
				if (oe)
					tdiff_emit_rename(c, oe->path, p, &oe->oid,
							  oe->mode, &nt.e[j].oid,
							  nt.e[j].mode);
			} else
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
			} else if (!oid_equal(&ot.e[i].oid, &nt.e[j].oid) ||
				   ot.e[i].mode != nt.e[j].mode) {
				/* a mode that changed on its own still changed */
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
	struct index_state old_ist, new_ist;
	struct rename_list rl;

	memset(&old_ist, 0, sizeof old_ist);
	memset(&new_ist, 0, sizeof new_ist);
	memset(&rl, 0, sizeof rl);

	/*
	 * A move is a path one tree has and the other has not, holding the same
	 * file: the two have to be lined up by path to see that, which is not
	 * something the walk below can do while it is walking.  So the paths are
	 * collected first, and the walk asks this what moved.
	 */
	if (old_tree && new_tree) {
		read_tree_into_index(r, &old_ist, old_tree, "");
		read_tree_into_index(r, &new_ist, new_tree, "");
		renames_between(&r->odb, &old_ist, &new_ist, &rl);
	}

	c.r = r;
	c.out = out;
	c.stat_only = stat_only;
	c.rl = &rl;
	c.old_ist = &old_ist;
	memset(&c.totals, 0, sizeof c.totals);

	tdiff_recurse(&c, "", old_tree, new_tree);

	if (stat_only && c.totals.files)
		stat_render(&c.totals, out);
	stat_totals_release(&c.totals);

	rename_list_release(&rl);
	index_release(&old_ist);
	index_release(&new_ist);
}

/* ------------------------------------------------------------------ */

int cmd_diff(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct buf out;
	int cached, stat_only;
	oid_t old_tree, new_tree;
	int have_old = 0, have_new = 0;
	struct stat_totals totals;

	opts_init(&o, argc, argv, (const char *const[]){
		"--cached", "--staged", "--stat", "--prompt-hunks", NULL });
	cached = opts_flag(&o, "--cached") || opts_flag(&o, "--staged");
	stat_only = opts_flag(&o, "--stat");
	memset(&totals, 0, sizeof totals);

	buf_init(&out);

	/*
	 * With this, the commit's own change is what is asked for, broken down
	 * by the prompt that asked for each part of it.  A range has no chain:
	 * the prompts that made the far end of it are not the ones that made the
	 * near end, so there is nothing single to break down, and saying so is
	 * better than picking one end and pretending.
	 */
	if (opts_flag(&o, "--prompt-hunks")) {
		oid_t coid, parent_tree;
		struct commit c = COMMIT_INIT;
		enum obj_type t;
		int have_parent = 0;

		if (stat_only)
			gp_die("diff: --stat and --prompt-hunks cannot both be "
			       "asked for");
		if (o.nargs != 1)
			gp_die("diff: --prompt-hunks takes one commit\n"
			       "hint: a range has no single chain of prompts "
			       "to break it down");
		if (resolve_rev(r, o.args[0], &coid) < 0 ||
		    odb_type_of(&r->odb, &coid, &t) < 0 || t != OBJ_COMMIT)
			gp_die("diff: not a commit: %s", o.args[0]);
		read_commit(r, &coid, &c);
		if (c.parents.nr) {
			struct commit parent = COMMIT_INIT;

			read_commit(r, &c.parents.oid[0], &parent);
			parent_tree = parent.tree;
			have_parent = 1;
			commit_release(&parent);
		}
		trace_or_diff(r, &c, &parent_tree, have_parent, &out);
		fwrite(out.b, 1, out.len, stdout);
		buf_release(&out);
		commit_release(&c);
		return 0;
	}

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
			struct buf body;
			oid_t woid;
			int changed;
			buf_init(&w);
			buf_init(&piece);
			buf_init(&body);
			if (read_file(full, &w) < 0)
				buf_reset(&w);
			{
				struct buf old;
				buf_init(&old);
				odb_read(&r->odb, &ist.e[i].oid, NULL, &old);
				if (stat_only) {
					struct stat_counts counts;

					if (looks_binary(old.b, old.len) ||
					    looks_binary(w.b, w.len)) {
						/* a file the work tree did not
						 * touch is not a row */
						if (old.len != w.len ||
						    memcmp(old.b, w.b, old.len))
							stat_binary_line(
								&totals,
								ist.e[i].path,
								old.len, w.len);
					} else {
						diff_counts(old.b, old.len, w.b,
							    w.len, &counts);
						if (counts.add || counts.del)
							stat_line(&totals,
								  ist.e[i].path,
								  counts.add,
								  counts.del);
					}
					buf_release(&old);
					free(full);
					buf_release(&w);
					buf_release(&piece);
					continue;
				}
				{
					char *alab = xstrfmt("a/%s", ist.e[i].path);
					char *blab = xstrfmt("b/%s", ist.e[i].path);
					changed = diff_buffers(alab, old.b, old.len,
							       blab, w.b, w.len,
							       &body, 0);
					free(alab);
					free(blab);
				}
				/*
				 * The work tree's side of the index line names a
				 * blob that was never written, so it is hashed
				 * here and not stored -- which is what git does
				 * with the file it is diffing, and the only way
				 * the line can name both ends.
				 */
				if (changed) {
					const oid_t *w2 = &null_oid;
					char *o1, *o2;

					if (odb_hash(&r->odb, OBJ_BLOB, w.b, w.len,
						     &woid, 0) == 0)
						w2 = &woid;
					o1 = abbrev_oid(&ist.e[i].oid);
					o2 = abbrev_oid(w2);
					buf_addf(&piece,
						 "diff --git a/%s b/%s\n",
						 ist.e[i].path, ist.e[i].path);
					buf_addf(&piece,
						 "index %s..%s %06o\n", o1, o2,
						 ist.e[i].mode);
					buf_add(&piece, body.b, body.len);
					free(o1);
					free(o2);
				}
				buf_release(&body);
				buf_release(&old);
			}
			if (changed)
				buf_add(&out, piece.b, piece.len);
			free(full);
			buf_release(&w);
			buf_release(&piece);
		}
		index_release(&ist);
		if (stat_only && totals.files)
			stat_render(&totals, &out);
	}
	stat_totals_release(&totals);

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

	opts_init(&o, argc, argv, (const char *const[]){
		"--soft", "--mixed", "--hard", NULL });
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

	{
		oid_t old = null_oid;
		int had = refs_head(&r->refs, &old) == 0;

		if (branch_ref) {
			refs_write(&r->refs, branch_ref, &target);
			refs_reflog(&r->refs, branch_ref, had ? &old : &null_oid,
				    &target, "reset: moving to HEAD");
			refs_reflog_head(&r->refs, branch_ref,
					 had ? &old : &null_oid, &target,
					 "reset: moving to HEAD");
		} else {
			refs_set_head_detached(&r->refs, &target);
			refs_reflog_head(&r->refs, NULL, had ? &old : &null_oid,
					 &target, "reset: moving to HEAD");
		}
	}

	if (mode == 1) {
		struct index_state ist;
		memset(&ist, 0, sizeof ist);
		read_tree_into_index(r, &ist, &c.tree, "");
		index_write(&ist, repo_index_path(r));
		index_release(&ist);
	}
	if (mode == 2) {
		/* The checkout writes the index itself, and it has to be the one
		 * that does, because it tells which files to remove by comparing
		 * the index it finds against the tree it is checking out.  Writing
		 * the new index first would have both sides agree and leave the
		 * previous tree's files sitting in the work tree. */
		checkout_tree(r, &c.tree, 1, 1);
		printf("HEAD is now at %s\n", abbrev_oid(&target));
	}

	free(branch_ref);
	commit_release(&c);
	return 0;
}

/* ------------------------------------------------------------------ */
/* reflog                                                              */

/* one reflog file, printed line by line; 0 when it was there */
static int reflog_dump(struct repo *r, const char *full)
{
	char *path = repo_git_path(r, "logs/%s", full);
	struct buf b;
	const char *p, *end;

	buf_init(&b);
	if (read_file(path, &b) < 0) {
		free(path);
		buf_release(&b);
		return -1;
	}
	p = (const char *)b.b;
	end = p + b.len;
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);

		if (len)
			printf("%.*s\n", (int)len, p);
		p = nl ? nl + 1 : end;
	}
	free(path);
	buf_release(&b);
	return 0;
}

/*
 * `reflog` with no argument is HEAD's reflog, which is what git means by it:
 * the record of where the work tree has been, and the thing that survives a
 * switch.  A repository whose HEAD has never moved has no such file -- one
 * written by an older gitprompt, or by a plain `git clone` of a history that
 * only ever had one branch -- so the branch's log is shown instead of saying
 * there is no reflog at all.
 */
int cmd_reflog(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *name;
	char *full;

	opts_init(&o, argc, argv, NULL);
	name = opts_arg(&o, 0);
	if (!name)
		name = "HEAD";

	if (strchr(name, '/'))
		full = xstrdup(name);
	else if (!strcmp(name, "HEAD"))
		full = xstrdup("HEAD");
	else
		full = xstrfmt("refs/heads/%s", name);

	if (reflog_dump(r, full) == 0) {
		free(full);
		return 0;
	}
	if (!strcmp(name, "HEAD")) {
		char *t = refs_head_target(&r->refs);
		int rc = t ? reflog_dump(r, t) : -1;

		free(t);
		if (rc == 0) {
			free(full);
			return 0;
		}
	}
	gp_error("reflog: no reflog for %s", name);
	free(full);
	return 1;
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
	struct opts o;
	oid_t target;
	struct oid_array tips = OID_ARRAY_INIT;
	struct tag_scan ts;
	struct describe_ctx d;

	opts_init(&o, argc, argv, (const char *const[]){ "--tags", NULL });

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
