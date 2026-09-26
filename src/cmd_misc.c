/*
 * cmd_misc.c - integrity, housekeeping and help.
 *
 * fsck walks every object reachable from every ref, checks that each one
 * parses and that everything it points at exists, then reports the loose
 * objects nothing points at.  Since gitprompt's whole point is that the
 * prompt files carry their own history, a broken object graph is a broken
 * record of how a project was built, which is worth being able to detect.
 *
 * gc exists for the case fsck flags: objects written during aborted
 * experimentation, or left behind when a branch was reset away.
 *
 * Both read objects with odb_read and parse them by hand rather than going
 * through read_commit/read_tree_obj, because those report a bad object by
 * dying -- the one thing fsck must not do.
 */
#include "gp.h"

#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

static char *oid_hex_dup(const oid_t *oid)
{
	char hex[GP_SHA1_HEXSZ + 1];
	oid_hex(oid, hex);
	return xstrdup(hex);
}

/* ------------------------------------------------------------------ */
/* a set of object ids, kept sorted so membership is a binary search   */

struct oid_set {
	oid_t *e;
	size_t nr, alloc;
};

static void oid_set_add(struct oid_set *s, const oid_t *oid)
{
	if (s->nr == s->alloc) {
		s->alloc = s->alloc ? s->alloc * 2 : 64;
		s->e = xrealloc(s->e, s->alloc * sizeof(*s->e));
	}
	s->e[s->nr++] = *oid;
}

static int oid_cmp(const void *a, const void *b)
{
	return memcmp(a, b, sizeof(oid_t));
}

static void oid_set_sort(struct oid_set *s)
{
	if (s->nr > 1)
		qsort(s->e, s->nr, sizeof(*s->e), oid_cmp);
}

static int oid_set_has(const struct oid_set *s, const oid_t *oid)
{
	return bsearch(oid, s->e, s->nr, sizeof(*s->e), oid_cmp) != NULL;
}

static void oid_set_release(struct oid_set *s)
{
	free(s->e);
	s->e = NULL;
	s->nr = s->alloc = 0;
}

/* ------------------------------------------------------------------ */
/* fsck                                                                */

struct fsck {
	struct repo *r;
	struct oid_set seen;      /* everything reachable from a ref */
	int errors;
	int commits;
	int trees;
	int refs;
};

static void fsck_err(struct fsck *f, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("error: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	f->errors++;
}

/* read an object and hand back its type, without dying on a bad one */
static int fsck_load(struct fsck *f, const oid_t *oid, enum obj_type want,
		     struct buf *out, const char *what)
{
	enum obj_type t;

	if (odb_read(&f->r->odb, oid, &t, out) < 0) {
		fsck_err(f, "%s: unreadable %s", abbrev_oid(oid), what);
		return -1;
	}
	if (t != want) {
		fsck_err(f, "%s: expected a %s but found a different type",
			 abbrev_oid(oid), what);
		buf_release(out);
		return -1;
	}
	return 0;
}

static void fsck_tree(struct fsck *f, const oid_t *oid)
{
	struct buf b;
	struct tree t;
	size_t i;

	if (oid_set_has(&f->seen, oid))
		return;
	oid_set_add(&f->seen, oid);

	buf_init(&b);
	if (fsck_load(f, oid, OBJ_TREE, &b, "tree") < 0)
		return;

	memset(&t, 0, sizeof t);
	tree_parse(&t, b.b, b.len);
	f->trees++;

	for (i = 0; i < t.nr; i++) {
		const struct tree_entry *e = &t.e[i];

		if (!e->name || !e->name[0]) {
			fsck_err(f, "%s: tree entry with an empty name",
				 abbrev_oid(oid));
			continue;
		}
		if (strchr(e->name, '/')) {
			fsck_err(f, "%s: tree entry '%s' contains a slash",
				 abbrev_oid(oid), e->name);
			continue;
		}
		if (e->mode == MODE_TREE) {
			fsck_tree(f, &e->oid);
		} else if (e->mode == MODE_BLOB || e->mode == MODE_EXEC ||
			   e->mode == MODE_LINK) {
			if (!odb_exists(&f->r->odb, &e->oid)) {
				fsck_err(f, "%s: missing blob %s (for '%s')",
					 abbrev_oid(oid), abbrev_oid(&e->oid),
					 e->name);
				continue;
			}
			oid_set_add(&f->seen, &e->oid);
		} else {
			fsck_err(f, "%s: entry '%s' has bad mode %06o",
				 abbrev_oid(oid), e->name, e->mode);
		}
	}
	tree_release(&t);
	buf_release(&b);
}

/* walk the commit chain iteratively; the queue lives in the caller */
static void fsck_commit(struct fsck *f, const oid_t *oid, struct oid_array *todo)
{
	struct buf b;
	struct commit c = COMMIT_INIT;
	size_t i;

	if (oid_set_has(&f->seen, oid))
		return;
	oid_set_add(&f->seen, oid);

	buf_init(&b);
	if (fsck_load(f, oid, OBJ_COMMIT, &b, "commit") < 0)
		return;

	commit_parse(&c, b.b, b.len);
	f->commits++;

	if (!c.author || !c.author[0])
		fsck_err(f, "%s: commit has no author", abbrev_oid(oid));
	else if (!strchr(c.author, '<'))
		fsck_err(f, "%s: author '%s' has no address", abbrev_oid(oid),
			 c.author);

	if (!c.committer || !c.committer[0])
		fsck_err(f, "%s: commit has no committer", abbrev_oid(oid));

	if (!c.message || !c.message[0])
		fsck_err(f, "%s: commit has an empty message", abbrev_oid(oid));

	if (!odb_exists(&f->r->odb, &c.tree))
		fsck_err(f, "%s: commit points at missing tree %s",
			 abbrev_oid(oid), abbrev_oid(&c.tree));
	else
		fsck_tree(f, &c.tree);

	for (i = 0; i < c.parents.nr; i++) {
		if (!odb_exists(&f->r->odb, &c.parents.oid[i]))
			fsck_err(f, "%s: commit points at missing parent %s",
				 abbrev_oid(oid), abbrev_oid(&c.parents.oid[i]));
		else
			oid_array_append(todo, &c.parents.oid[i]);
	}

	commit_release(&c);
	buf_release(&b);
}

static void fsck_tag(struct fsck *f, const oid_t *oid)
{
	struct buf b;
	struct buf tb;
	enum obj_type t;
	const char *nl, *hex;
	oid_t target;

	if (oid_set_has(&f->seen, oid))
		return;
	oid_set_add(&f->seen, oid);

	buf_init(&b);
	if (odb_read(&f->r->odb, oid, &t, &b) < 0) {
		fsck_err(f, "%s: unreadable tag", abbrev_oid(oid));
		return;
	}
	if (t != OBJ_TAG) {
		fsck_err(f, "%s: a ref under refs/tags points at a non-tag",
			 abbrev_oid(oid));
		buf_release(&b);
		return;
	}
	if (b.len < 7 || memcmp(b.b, "object ", 7)) {
		fsck_err(f, "%s: annotated tag has no object header",
			 abbrev_oid(oid));
		buf_release(&b);
		return;
	}

	hex = (const char *)b.b + 7;
	nl = memchr(hex, '\n', (size_t)((const char *)b.b + b.len - hex));
	if (!nl || (size_t)(nl - hex) != GP_SHA1_HEXSZ) {
		fsck_err(f, "%s: annotated tag names no object", abbrev_oid(oid));
		buf_release(&b);
		return;
	}
	{
		char *h = xstrndup(hex, GP_SHA1_HEXSZ);
		int bad = oid_parse(&target, h) < 0;
		free(h);
		if (bad) {
			fsck_err(f, "%s: annotated tag names no object",
				 abbrev_oid(oid));
			buf_release(&b);
			return;
		}
	}
	if (!odb_exists(&f->r->odb, &target)) {
		fsck_err(f, "%s: annotated tag points at missing object %s",
			 abbrev_oid(oid), abbrev_oid(&target));
		buf_release(&b);
		return;
	}

	/* follow it, so the target's own subtree gets checked */
	buf_init(&tb);
	if (odb_read(&f->r->odb, &target, &t, &tb) < 0) {
		fsck_err(f, "%s: unreadable target %s", abbrev_oid(oid),
			 abbrev_oid(&target));
	} else {
		buf_release(&tb);
		if (t == OBJ_COMMIT) {
			struct oid_array todo = OID_ARRAY_INIT;
			fsck_commit(f, &target, &todo);
			while (todo.nr) {
				oid_t next = todo.oid[--todo.nr];
				fsck_commit(f, &next, &todo);
			}
			oid_array_clear(&todo);
		} else if (t == OBJ_TREE) {
			fsck_tree(f, &target);
		} else if (t == OBJ_TAG) {
			fsck_tag(f, &target);
		}
	}
	buf_release(&b);
}

static void fsck_ref_cb(const char *refname, const oid_t *oid, void *ud)
{
	struct fsck *f = ud;
	enum obj_type t;
	struct buf b;

	f->refs++;

	if (!odb_exists(&f->r->odb, oid)) {
		fsck_err(f, "%s: points at missing object %s", refname,
			 abbrev_oid(oid));
		return;
	}
	buf_init(&b);
	if (odb_read(&f->r->odb, oid, &t, &b) < 0) {
		fsck_err(f, "%s: unreadable object %s", refname, abbrev_oid(oid));
		return;
	}
	buf_release(&b);

	switch (t) {
	case OBJ_COMMIT: {
		struct oid_array todo = OID_ARRAY_INIT;
		fsck_commit(f, oid, &todo);
		while (todo.nr) {
			oid_t next = todo.oid[--todo.nr];
			fsck_commit(f, &next, &todo);
		}
		oid_array_clear(&todo);
		break;
	}
	case OBJ_TREE:
		fsck_tree(f, oid);
		break;
	case OBJ_TAG:
		fsck_tag(f, oid);
		break;
	case OBJ_BLOB:
		oid_set_add(&f->seen, oid);
		break;
	default:
		break;
	}
}

struct dangling_ctx {
	const struct oid_set *seen;
	int nr;
	int show;
};

static int dangling_one(const oid_t *oid, void *ud)
{
	struct dangling_ctx *c = ud;
	if (!oid_set_has(c->seen, oid)) {
		c->nr++;
		if (c->show) {
			char *h = oid_hex_dup(oid);
			printf("dangling %s\n", h);
			free(h);
		}
	}
	return 0;
}

/* mark everything reachable from the refs, for gc to work from */
static void mark_reachable(struct repo *r, struct fsck *f)
{
	refs_list(&r->refs, "refs/", fsck_ref_cb, f);
	refs_list_packed(&r->refs, "refs/", fsck_ref_cb, f);
	oid_set_sort(&f->seen);
}

int cmd_fsck(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct fsck f;
	struct dangling_ctx d;
	int verbose;

	opts_init(&o, argc, argv, NULL, 0);
	verbose = opts_flag(&o, "-v") || opts_flag(&o, "--verbose");

	memset(&f, 0, sizeof f);
	f.r = r;
	mark_reachable(r, &f);

	d.seen = &f.seen;
	d.nr = 0;
	d.show = verbose;

	odb_foreach_loose(&r->odb, dangling_one, &d);

	if (verbose || f.errors)
		printf("%d ref(s), %d commit(s), %d tree(s) checked\n", f.refs,
		       f.commits, f.trees);
	if (f.errors)
		printf("%d problem(s) found\n", f.errors);
	else if (verbose)
		printf("no problems found\n");
	if (d.nr)
		printf("%d dangling object(s)\n", d.nr);

	oid_set_release(&f.seen);
	return f.errors ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* gc                                                                  */

/* an object survives if it is reachable, or if it was written in the
 * last two weeks -- the same grace period git gives, so that work in a
 * detached HEAD or a torn-down branch is not thrown away at once */
#define GC_GRACE_SECONDS (14 * 24 * 60 * 60)

struct gc_ctx {
	struct repo *r;
	const struct oid_set *seen;
	size_t pruned;
	size_t kept;
	size_t recent;
	int dry_run;
};

static int gc_one(const oid_t *oid, void *ud)
{
	struct gc_ctx *c = ud;
	char hex[GP_SHA1_HEXSZ + 1];
	char *path, *dir, *slash;

	if (oid_set_has(c->seen, oid)) {
		c->kept++;
		return 0;
	}

	oid_hex(oid, hex);
	path = xstrfmt("%s/%c%c/%s", c->r->odb.dir, hex[0], hex[1], hex + 2);

	{
		struct stat st;
		if (stat(path, &st) == 0 &&
		    (long long)(time(NULL) - (time_t)st.st_mtime) <
			    GC_GRACE_SECONDS) {
			c->recent++;
			free(path);
			return 0;
		}
	}

	if (c->dry_run) {
		char *h = oid_hex_dup(oid);
		printf("would prune %s\n", h);
		free(h);
		c->pruned++;
		free(path);
		return 0;
	}

	if (remove(path) == 0) {
		c->pruned++;
		dir = xstrdup(path);
		slash = strrchr(dir, '/');
		if (slash) {
			*slash = '\0';
			rmdir(dir);
		}
		free(dir);
	}
	free(path);
	return 0;
}

int cmd_gc(struct repo *r, int argc, char **argv)
{
	static const char *const takes[] = { "--prune" };
	struct opts o;
	struct fsck f;
	struct gc_ctx g;
	size_t total;

	opts_init(&o, argc, argv, takes, 1);

	memset(&f, 0, sizeof f);
	f.r = r;
	mark_reachable(r, &f);

	g.r = r;
	g.seen = &f.seen;
	g.pruned = g.kept = g.recent = 0;
	g.dry_run = opts_flag(&o, "-n") || opts_flag(&o, "--dry-run");

	total = odb_count(&r->odb);
	odb_foreach_loose(&r->odb, gc_one, &g);

	/* gc must not report fsck's findings; it only wanted the reach set */
	f.errors = 0;

	printf("%s %lu object(s) of %lu\n", g.dry_run ? "Would prune" : "Pruned",
	       (unsigned long)g.pruned, (unsigned long)total);
	if (g.recent)
		printf("Kept %lu unreachable object(s) younger than %d days\n",
		       (unsigned long)g.recent, GC_GRACE_SECONDS / (24 * 60 * 60));
	printf("Kept %lu reachable object(s)\n", (unsigned long)g.kept);

	oid_set_release(&f.seen);
	return 0;
}

/* ------------------------------------------------------------------ */
/* help and version                                                    */

static void print_command_list(void)
{
	size_t i;
	printf("usage: gitprompt <command> [<args>]\n\n");
	printf("These are the gitprompt commands:\n\n");
	for (i = 0; i < commands_nr; i++)
		printf("   %-18s %s\n", commands[i].name, commands[i].summary);
	printf("\nSee 'gitprompt help <command>' for the usage of one command.\n");
}

int cmd_help(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *what;
	size_t i;

	(void)r;
	opts_init(&o, argc, argv, NULL, 0);
	what = opts_arg(&o, 0);
	if (!what) {
		print_command_list();
		return 0;
	}
	for (i = 0; i < commands_nr; i++) {
		if (!strcmp(commands[i].name, what)) {
			printf("usage: gitprompt %s\n\n   %s\n",
			       commands[i].usage, commands[i].summary);
			return 0;
		}
	}
	gp_error("no such command: %s", what);
	return 1;
}

int cmd_version(struct repo *r, int argc, char **argv)
{
	(void)r;
	(void)argc;
	(void)argv;
	printf("gitprompt %s\n", GP_VERSION);
	return 0;
}
