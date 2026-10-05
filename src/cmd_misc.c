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

/*
 * Sort the set, and drop the duplicates.  Membership is a binary search, so
 * it only means anything once this has run: while the set is being built the
 * search can miss an entry that is there, and the same object can be added
 * twice.  Every producer calls this once the walk is over, and after that the
 * set really is one -- which the count of objects to pack depends on.
 */
static void oid_set_sort(struct oid_set *s)
{
	size_t i, w = 0;

	if (s->nr > 1)
		qsort(s->e, s->nr, sizeof(*s->e), oid_cmp);
	for (i = 0; i < s->nr; i++) {
		if (w > 0 && oid_cmp(&s->e[w - 1], &s->e[i]) == 0)
			continue;
		s->e[w++] = s->e[i];
	}
	s->nr = w;
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
	/*
	 * Prompt files name a snapshot tree in their frontmatter, which nothing
	 * else in the object graph points at -- the reference is a path written
	 * inside a blob, and a walk cannot see that.  It is read out here so
	 * that a snapshot is reachable, and gc does not reclaim the state a
	 * prompt's attribution is built from.
	 */
	const char *pdir;
	size_t pdir_len;
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

static void fsck_tree(struct fsck *f, const oid_t *oid, const char *prefix);

/*
 * A prompt file names the work tree it was recorded over, and nothing else in
 * the object graph points at that tree: the name is written inside a blob, and
 * a walk does not read blobs.  Reading it here is what makes a snapshot a root
 * -- gc leaves it alone, and fsck counts it as the tree it is.  A name that no
 * longer resolves is left alone rather than reported: the attribution is what
 * degrades, and trace.c is where that is said.
 */
static void fsck_prompt_snapshot(struct fsck *f, const char *path,
				 const oid_t *oid)
{
	const char *rest;
	struct buf b;
	struct prompt p = PROMPT_INIT;
	oid_t snap;

	if (!f->pdir || !is_prompt_path(f->pdir, f->pdir_len, path, &rest))
		return;

	buf_init(&b);
	if (odb_read(&f->r->odb, oid, NULL, &b) == 0 &&
	    prompt_from_file(&p, b.b, b.len) && p.snapshot &&
	    oid_parse(&snap, p.snapshot) == 0 &&
	    odb_exists(&f->r->odb, &snap))
		fsck_tree(f, &snap, "");

	prompt_release(&p);
	buf_release(&b);
}

static void fsck_tree(struct fsck *f, const oid_t *oid, const char *prefix)
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
		char *full;

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
		full = xstrfmt("%s%s", prefix, e->name);
		if (e->mode == MODE_TREE) {
			char *dir = xstrfmt("%s/", full);

			fsck_tree(f, &e->oid, dir);
			free(dir);
		} else if (e->mode == MODE_GITLINK) {
			/*
			 * The commit a gitlink names lives in the submodule's store, not
			 * in this one, so its absence here is the normal case rather than
			 * a hole -- and it is not something to follow into either.
			 */
			;
		} else if (e->mode == MODE_BLOB || e->mode == MODE_EXEC ||
			   e->mode == MODE_LINK) {
			if (!odb_exists(&f->r->odb, &e->oid)) {
				fsck_err(f, "%s: missing blob %s (for '%s')",
					 abbrev_oid(oid), abbrev_oid(&e->oid),
					 e->name);
				free(full);
				continue;
			}
			oid_set_add(&f->seen, &e->oid);
			fsck_prompt_snapshot(f, full, &e->oid);
		} else {
			fsck_err(f, "%s: entry '%s' has bad mode %06o",
				 abbrev_oid(oid), e->name, e->mode);
		}
		free(full);
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
		fsck_tree(f, &c.tree, "");

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
			fsck_tree(f, &target, "");
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
		fsck_tree(f, oid, "");
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

/*
 * The graph these four commands work on is the one the store literally holds,
 * so each of them turns replacement off before it starts.  A walk that followed
 * a replacement would mark the replacement's sub-objects and leave the replaced
 * object's own unmarked, and the pack that followed would write the
 * replacement's bytes under the replaced id; both are corruptions of a
 * repository whose history anyone else still sees as the real object.  git's
 * own fsck and prune do the same.  Reading a replacement, which is what
 * replacement is for, is untouched and happens in odb_read.
 */
static void no_replace(struct repo *r)
{
	odb_set_replace_dir(&r->odb, NULL);
}

/* mark everything reachable from the refs, for gc to work from */
static void mark_reachable(struct repo *r, struct fsck *f)
{
	/* repo_prompt_dir hands back a static buffer, so the walk gets a copy
	 * of its own to read for as long as it runs */
	char *dir = xstrdup(repo_prompt_dir(r));

	f->pdir = dir;
	f->pdir_len = strlen(dir);

	refs_list(&r->refs, "refs/", fsck_ref_cb, f);
	refs_list_packed(&r->refs, "refs/", fsck_ref_cb, f);
	oid_set_sort(&f->seen);

	f->pdir = NULL;
	free(dir);
}

int cmd_fsck(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct fsck f;
	struct dangling_ctx d;
	int verbose;

	opts_init(&o, argc, argv, (const char *const[]){
		"-v", "--verbose", NULL });
	verbose = opts_flag(&o, "-v") || opts_flag(&o, "--verbose");

	no_replace(r);
	memset(&f, 0, sizeof f);
	f.r = r;
	mark_reachable(r, &f);

	d.seen = &f.seen;
	d.nr = 0;
	d.show = verbose;

	/* an object nobody can reach is dangling whichever form it is in */
	odb_foreach(&r->odb, dangling_one, &d);

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
	long grace;      /* how young an unreachable object may be and stay */
	int dry_run;
	int verbose;     /* name each object, not only count them */
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
		    (long long)(time(NULL) - (time_t)st.st_mtime) < c->grace) {
			c->recent++;
			free(path);
			return 0;
		}
	}

	if (c->dry_run) {
		printf("would prune %s\n", hex);
		c->pruned++;
		free(path);
		return 0;
	}

	if (remove(path) == 0) {
		c->pruned++;
		if (c->verbose)
			printf("pruned %s\n", hex);
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

/*
 * Put `objs` into one pack and drop the loose copies that the pack duplicates;
 * with `drop`, also delete the packs it supersedes.  This is the half `gc` and
 * `repack` share -- gc packs everything reachable, and repack packs either
 * that set or only the loose part of it.
 *
 * Reachable, not everything: an unreachable object is one gc is supposed to
 * be able to leave alone for its grace period, and packing it would mean
 * keeping it forever.
 *
 * The whole set is written every time, not only the objects that are not in a
 * pack yet.  Writing only the new ones would leave a pack per run, and a store
 * packed daily would end up with as many packs as it had days.  One pack
 * holding everything is what git settles to, and it is what makes the second
 * run over an unchanged store rewrite the same file under the same name rather
 * than add to a pile.
 *
 * Returns how many objects went into the pack; *freed gets the loose files
 * that became duplicates, *dropped the packs that were superseded.
 */
static size_t pack_objects(struct repo *r, const oid_t *objs, size_t nr,
			   int drop, int dry_run, size_t *freed, size_t *dropped)
{
	char *packdir, *keep = NULL;
	u8 sha[GP_SHA1_RAWSZ];
	size_t i;

	*freed = *dropped = 0;
	if (!nr)
		return 0;
	if (dry_run) {
		if (drop)
			*dropped = pack_drop_redundant(&r->odb, objs, nr, NULL, 1);
		return nr;
	}

	packdir = xstrfmt("%s/pack", r->odb.dir);
	if (pack_write(&r->odb, objs, nr, packdir, sha) < 0) {
		/* the loose objects are all still there, so this is not fatal */
		gp_error("repack: nothing was packed; the objects are untouched");
		free(packdir);
		return 0;
	}
	{
		oid_t id;
		char hex[GP_SHA1_HEXSZ + 1];

		memcpy(id.raw, sha, GP_SHA1_RAWSZ);
		oid_hex(&id, hex);
		keep = xstrfmt("%s/pack-%s.pack", packdir, hex);
	}
	free(packdir);

	/* the pack just written is not in the cached list yet, so nothing that
	 * is looked at here is the one that must survive */
	if (drop)
		*dropped = pack_drop_redundant(&r->odb, objs, nr, keep, 0);
	free(keep);

	/*
	 * Reload: the cache still lists the packs that were just deleted, and
	 * does not yet list the one that was just written.
	 */
	pack_release_all(&r->odb);
	pack_load_all(&r->odb);
	for (i = 0; i < nr; i++)
		if (odb_forget_loose(&r->odb, &objs[i]) == 0)
			(*freed)++;
	return nr;
}

int cmd_gc(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct fsck f;
	struct gc_ctx g;
	size_t total, nr_pack, freed, dropped;

	opts_init(&o, argc, argv, (const char *const[]){
		"-n", "--dry-run", NULL });

	no_replace(r);
	memset(&f, 0, sizeof f);
	f.r = r;
	mark_reachable(r, &f);

	g.r = r;
	g.seen = &f.seen;
	g.pruned = g.kept = g.recent = 0;
	g.grace = GC_GRACE_SECONDS;
	g.verbose = 0;
	g.dry_run = opts_flag(&o, "-n") || opts_flag(&o, "--dry-run");

	total = odb_count(&r->odb);
	odb_foreach_loose(&r->odb, gc_one, &g);

	nr_pack = pack_objects(r, f.seen.e, f.seen.nr, 1, g.dry_run, &freed,
			       &dropped);

	/* gc must not report fsck's findings; it only wanted the reach set */
	f.errors = 0;

	printf("%s %lu object(s) of %lu\n", g.dry_run ? "Would prune" : "Pruned",
	       (unsigned long)g.pruned, (unsigned long)total);
	if (g.recent)
		printf("Kept %lu unreachable object(s) younger than %d days\n",
		       (unsigned long)g.recent, GC_GRACE_SECONDS / (24 * 60 * 60));
	printf("Kept %lu reachable object(s)\n", (unsigned long)g.kept);
	if (nr_pack)
		printf("%s %lu object(s) into a pack\n",
		       g.dry_run ? "Would pack" : "Packed",
		       (unsigned long)nr_pack);
	if (freed)
		printf("Removed %lu loose duplicate(s)\n", (unsigned long)freed);
	if (dropped)
		printf("%s %lu superseded pack(s)\n",
		       g.dry_run ? "Would remove" : "Removed",
		       (unsigned long)dropped);

	oid_set_release(&f.seen);
	return 0;
}

/* ------------------------------------------------------------------ */
/* repack and prune                                                    */

/* the loose objects that are reachable, which is what a plain repack adds */
struct loose_ctx {
	const struct oid_set *seen;
	struct oid_set *out;
};

static int loose_reachable_one(const oid_t *oid, void *ud)
{
	struct loose_ctx *c = ud;

	if (oid_set_has(c->seen, oid))
		oid_set_add(c->out, oid);
	return 0;
}

int cmd_repack(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct fsck f;
	struct oid_set loose;
	const oid_t *objs;
	size_t nr, nr_pack, freed, dropped;
	int all, drop, dry_run, quiet;

	opts_init(&o, argc, argv, (const char *const[]){
		"-a", "-A", "-d", "-n", "--dry-run", "-q", "--quiet", NULL });

	all = opts_flag(&o, "-a") || opts_flag(&o, "-A");
	drop = opts_flag(&o, "-d");
	dry_run = opts_flag(&o, "-n") || opts_flag(&o, "--dry-run");
	quiet = opts_flag(&o, "-q") || opts_flag(&o, "--quiet");

	no_replace(r);
	memset(&f, 0, sizeof f);
	f.r = r;
	mark_reachable(r, &f);

	memset(&loose, 0, sizeof loose);
	if (all) {
		objs = f.seen.e;
		nr = f.seen.nr;
	} else {
		struct loose_ctx lc;

		lc.seen = &f.seen;
		lc.out = &loose;
		odb_foreach_loose(&r->odb, loose_reachable_one, &lc);
		oid_set_sort(&loose);
		objs = loose.e;
		nr = loose.nr;
	}

	if (!nr) {
		if (!quiet)
			printf("Nothing new to pack.\n");
		oid_set_release(&loose);
		oid_set_release(&f.seen);
		return 0;
	}

	nr_pack = pack_objects(r, objs, nr, drop, dry_run, &freed, &dropped);

	if (!quiet) {
		printf("%s %lu object(s) into a pack\n",
		       dry_run ? "Would pack" : "Packed", (unsigned long)nr_pack);
		if (freed)
			printf("Removed %lu loose duplicate(s)\n",
			       (unsigned long)freed);
		if (dropped)
			printf("%s %lu superseded pack(s)\n",
			       dry_run ? "Would remove" : "Removed",
			       (unsigned long)dropped);
	}

	oid_set_release(&loose);
	oid_set_release(&f.seen);
	return 0;
}

/*
 * `--expire` takes what git's does for the common cases: `now` for no grace
 * at all, and `<n>.<unit>.ago`.  A calendar expression (`2.weeks.ago` counts
 * back from *now*, not from a date) is the whole of it here; anything else is
 * refused rather than guessed at.
 */
static long expire_seconds(const char *s)
{
	static const struct { const char *name; long secs; } units[] = {
		{ "seconds", 1L },      { "minutes", 60L },
		{ "hours", 3600L },     { "days", 86400L },
		{ "weeks", 604800L },   { "months", 2592000L },
		{ "years", 31536000L },
	};
	const char *dot;
	char *end;
	size_t i, ulen;
	long n, mult = 0;

	if (!strcmp(s, "now"))
		return 0;

	dot = strchr(s, '.');
	if (!dot || dot == s)
		gp_die("prune: cannot parse --expire=%s", s);
	n = strtol(s, &end, 10);
	if (end != dot || n < 0)
		gp_die("prune: cannot parse --expire=%s", s);
	ulen = strlen(dot + 1);
	if (ulen < 5 || strcmp(dot + 1 + ulen - 4, ".ago"))
		gp_die("prune: cannot parse --expire=%s", s);
	for (i = 0; i < sizeof units / sizeof units[0]; i++) {
		size_t l = strlen(units[i].name);

		if (ulen == l + 4 && !strncmp(dot + 1, units[i].name, l)) {
			mult = units[i].secs;
			break;
		}
	}
	if (!mult)
		gp_die("prune: cannot parse --expire=%s", s);
	return n * mult;
}

static char *grace_text(long g)
{
	if (g % 86400 == 0)
		return xstrfmt("%ld days", g / 86400);
	return xstrfmt("%ld seconds", g);
}

/*
 * Prune drops unreachable *loose* objects.  Unreachable objects inside a pack
 * are left alone: the pack is a unit git keeps intact, and the way to reclaim
 * what is in one is `repack -A -d`, which explodes the unreachable objects
 * back out where this can then see them.
 */
int cmd_prune(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct fsck f;
	struct gc_ctx g;
	const char *exp;
	size_t total;

	opts_init(&o, argc, argv, (const char *const[]){
		"-n", "--dry-run", "-v", "--verbose", "--expire=", NULL });

	no_replace(r);
	memset(&f, 0, sizeof f);
	f.r = r;
	mark_reachable(r, &f);

	g.r = r;
	g.seen = &f.seen;
	g.pruned = g.kept = g.recent = 0;
	g.grace = GC_GRACE_SECONDS;
	g.dry_run = opts_flag(&o, "-n") || opts_flag(&o, "--dry-run");
	g.verbose = opts_flag(&o, "-v") || opts_flag(&o, "--verbose");

	exp = opts_value(&o, "--expire");
	if (exp)
		g.grace = expire_seconds(exp);

	total = odb_count(&r->odb);
	odb_foreach_loose(&r->odb, gc_one, &g);

	printf("%s %lu unreachable loose object(s) of %lu\n",
	       g.dry_run ? "Would prune" : "Pruned",
	       (unsigned long)g.pruned, (unsigned long)total);
	if (g.recent) {
		char *when = grace_text(g.grace);

		printf("Kept %lu unreachable object(s) younger than %s\n",
		       (unsigned long)g.recent, when);
		free(when);
	}
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
	opts_init(&o, argc, argv, NULL);
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
	struct opts o;

	(void)r;
	opts_init(&o, argc, argv, NULL);
	printf("gitprompt %s\n", GP_VERSION);
	return 0;
}
