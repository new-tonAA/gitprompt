/*
 * rev.c - turning a revision string into an object id, and walking the
 * commit graph.
 *
 * Understood: a full or abbreviated id, a ref name in any of its short
 * forms, HEAD, @, the ^ and ~ ancestry operators, and the ^{type} peel.
 * That is the set the commands in this program actually use.
 */
#include "gp.h"

/* ------------------------------------------------------------------ */
/* peeling                                                             */

static int read_tag_target(struct repo *r, const oid_t *tag, oid_t *out,
			   enum obj_type *type)
{
	struct buf b;
	enum obj_type t;
	const char *p, *end;
	int have_obj = 0, have_type = 0;

	buf_init(&b);
	if (odb_read(&r->odb, tag, &t, &b) < 0 || t != OBJ_TAG) {
		buf_release(&b);
		return -1;
	}
	p = (const char *)b.b;
	end = p + b.len;
	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		size_t llen = eol ? (size_t)(eol - p) : (size_t)(end - p);
		if (llen == 0)
			break;
		if (llen > 7 && !memcmp(p, "object ", 7)) {
			char *hex = xstrndup(p + 7, llen - 7);
			if (oid_parse(out, hex) == 0)
				have_obj = 1;
			free(hex);
		} else if (llen > 5 && !memcmp(p, "type ", 5)) {
			char *name = xstrndup(p + 5, llen - 5);
			*type = obj_type_from_name(name);
			have_type = 1;
			free(name);
		}
		p = eol ? eol + 1 : end;
	}
	buf_release(&b);
	return (have_obj && have_type) ? 0 : -1;
}

/*
 * Follow tag objects until something that is not a tag turns up.  If
 * `want` is set, require that type; otherwise return whatever is at the
 * bottom and report it through `got`.
 */
int commit_peel(struct repo *r, const oid_t *oid, enum obj_type want, oid_t *out)
{
	oid_t cur = *oid;
	enum obj_type t;
	int depth = 0;

	for (;;) {
		if (odb_read(&r->odb, &cur, &t, NULL) < 0)
			return -1;
		if (t != OBJ_TAG)
			break;
		if (++depth > 10)
			return -1;      /* a cycle, or absurdly deep */
		if (read_tag_target(r, &cur, &cur, &t) < 0)
			return -1;
	}
	if (want != OBJ_NONE && t != want)
		return -1;
	*out = cur;
	return 0;
}

/* ------------------------------------------------------------------ */
/* ref lookup                                                          */

static int resolve_refname(struct repo *r, const char *name, oid_t *out)
{
	static const char *prefixes[] = {
		"", "refs/", "refs/heads/", "refs/tags/",
		"refs/remotes/", "refs/sessions/",
	};
	size_t i;
	oid_t o;

	for (i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
		char *full = xstrfmt("%s%s", prefixes[i], name);
		int rc = refs_read(&r->refs, full, &o);
		free(full);
		if (rc == 0) {
			*out = o;
			return 0;
		}
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* revision strings                                                    */

/*
 * One step of ancestry: "^" or "^N" takes a parent, "~N" walks N first
 * parents.  Returns a pointer past what it consumed.
 */
static const char *apply_ancestry(struct repo *r, const oid_t *start,
				  const char *p, oid_t *out)
{
	oid_t cur;

	/* walking parents needs a commit, and a tag name is not one */
	if (commit_peel(r, start, OBJ_COMMIT, &cur) < 0)
		return NULL;

	while (*p == '^' || *p == '~') {
		char op = *p++;
		int n = 1;
		int step;

		if (*p >= '0' && *p <= '9') {
			n = 0;
			while (*p >= '0' && *p <= '9')
				n = n * 10 + (*p++ - '0');
		}
		if (n == 0)
			continue;       /* ^0 and ~0 both name the commit itself */
		/*
		 * ^N is "the Nth parent"; ~N is "N first parents".  Both end
		 * up taking a parent n times, just from a different slot.
		 */
		for (step = 0; step < n; step++) {
			struct commit c = COMMIT_INIT;
			oid_t next;
			int have;

			read_commit(r, &cur, &c);
			if (op == '^')
				have = c.parents.nr >= (size_t)n;
			else
				have = c.parents.nr > 0;
			if (!have) {
				commit_release(&c);
				return NULL;
			}
			next = (op == '^') ? c.parents.oid[n - 1] : c.parents.oid[0];
			commit_release(&c);
			cur = next;
		}
	}
	if (*p)
		return NULL;            /* trailing junk: HEAD~1x is not a revision */
	*out = cur;
	return p;
}

int resolve_rev(struct repo *r, const char *rev, oid_t *out)
{
	char *name;
	const char *suffix;
	oid_t base;
	int rc = -1;

	if (!rev || !*rev)
		return -1;

	/* ^{type} suffix */
	name = xstrdup(rev);
	{
		char *peel = strstr(name, "^{");
		enum obj_type want = OBJ_NONE;
		if (peel) {
			char *close = strchr(peel + 2, '}');
			int bare, after;

			if (!close) {
				free(name);
				return -1;
			}
			/* "^{}" is the whole peel, with no type named */
			bare = close == peel + 2;
			after = close[1] != '\0';
			*close = '\0';
			want = obj_type_from_name(peel + 2);
			if (!bare && want == OBJ_NONE) {
				free(name);
				return -1;
			}
			if (after) {
				/* "A^{commit}~1" peels and then walks; walking the peel
				 * is not read here, and answering with the peel alone
				 * would be an answer to a different question. */
				gp_error("bad revision: %s", rev);
				free(name);
				return -1;
			}
			/* the name ends where the suffix starts, not where it ends */
			*peel = '\0';
			{
				oid_t tmp;
				char *basepart = name;   /* now NUL-terminated */
				if (strcmp(basepart, "") == 0 ||
				    resolve_rev(r, basepart, &tmp) < 0) {
					free(name);
					return -1;
				}
				rc = commit_peel(r, &tmp, want, out);
				free(name);
				return rc;
			}
		}
	}

	/* Split off the ancestry suffix.  The suffix has to be remembered from
	 * the original string, not from the truncated copy: truncating writes
	 * the NUL that the suffix would have started at, so checking the
	 * truncated name for a suffix always says "none" and `HEAD~1` would
	 * quietly resolve to HEAD. */
	suffix = NULL;
	{
		char *caret = strpbrk(name, "^~");
		if (caret) {
			suffix = rev + (caret - name);
			*caret = '\0';
		}
	}

	if (!strcmp(name, "") || !strcmp(name, "@")) {
		if (refs_head(&r->refs, &base) < 0) {
			free(name);
			return -1;
		}
	} else if (strlen(name) >= 4 && strlen(name) <= GP_SHA1_HEXSZ &&
		   strspn(name, "0123456789abcdefABCDEF") == strlen(name)) {
		int pr = odb_resolve_prefix(&r->odb, name, &base);
		if (pr != 0) {
			if (pr == -2)
				gp_error("ambiguous object name: %s", name);
			free(name);
			return -1;
		}
	} else if (resolve_refname(r, name, &base) < 0) {
		free(name);
		return -1;
	}

	rc = 0;
	if (suffix) {
		oid_t after;
		if (apply_ancestry(r, &base, suffix, &after))
			*out = after;
		else {
			gp_error("bad revision: %s", rev);
			rc = -1;
		}
	} else {
		*out = base;
	}
	free(name);
	return rc;
}

int resolve_rev_tree(struct repo *r, const char *rev, oid_t *out)
{
	oid_t oid, tree;
	struct commit c = COMMIT_INIT;
	enum obj_type t;

	if (resolve_rev(r, rev, &oid) < 0)
		return -1;
	if (commit_peel(r, &oid, OBJ_NONE, &oid) < 0)
		return -1;
	if (odb_read(&r->odb, &oid, &t, NULL) < 0)
		return -1;

	if (t == OBJ_TREE) {
		*out = oid;
		return 0;
	}
	if (t != OBJ_COMMIT) {
		gp_error("not a commit or tree: %s", rev);
		return -1;
	}
	read_commit(r, &oid, &c);
	tree = c.tree;
	commit_release(&c);
	*out = tree;
	return 0;
}

/* ------------------------------------------------------------------ */
/* graph walking                                                       */

struct walk_item {
	oid_t oid;
	i64 time;
	size_t seq;
};

struct walk_ctx {
	struct repo *r;
	struct walk_item *items;
	size_t nr, alloc;
	struct oid_array seen;
};

/*
 * Breadth-first over the parents with an explicit stack rather than
 * recursion: history depth is unbounded, and a long linear history would
 * otherwise run the C stack out.
 */
static void walk_push(struct walk_ctx *c, const oid_t *oid)
{
	struct oid_array frontier = OID_ARRAY_INIT;

	oid_array_append(&frontier, oid);
	while (frontier.nr) {
		oid_t cur = frontier.oid[--frontier.nr];
		struct commit cm = COMMIT_INIT;
		size_t i;

		if (oid_array_contains(&c->seen, &cur))
			continue;
		oid_array_append(&c->seen, &cur);

		read_commit(c->r, &cur, &cm);
		if (c->nr == c->alloc) {
			c->alloc = c->alloc ? c->alloc * 2 : 32;
			c->items = xrealloc(c->items, c->alloc * sizeof(*c->items));
		}
		c->items[c->nr].oid = cur;
		c->items[c->nr].time = commit_time(&cm);
		c->items[c->nr].seq = c->nr;
		c->nr++;

		for (i = 0; i < cm.parents.nr; i++)
			oid_array_append(&frontier, &cm.parents.oid[i]);
		commit_release(&cm);
	}
	oid_array_clear(&frontier);
}

static int walk_cmp(const void *a, const void *b)
{
	const struct walk_item *x = a, *y = b;
	if (x->time != y->time)
		return x->time > y->time ? -1 : 1;   /* newest first */
	return x->seq < y->seq ? -1 : (x->seq > y->seq ? 1 : 0);
}

static void walk_ctx_init(struct walk_ctx *c, struct repo *r)
{
	memset(c, 0, sizeof *c);
	c->r = r;
}

static void walk_ctx_clear(struct walk_ctx *c)
{
	free(c->items);
	oid_array_clear(&c->seen);
}

/* mark every commit reachable from `tips` as walked */
static void walk_ctx_push_all(struct walk_ctx *c, const struct oid_array *tips)
{
	size_t i;

	for (i = 0; i < tips->nr; i++) {
		oid_t peeled;
		if (commit_peel(c->r, &tips->oid[i], OBJ_COMMIT, &peeled) == 0)
			walk_push(c, &peeled);
	}
}

/*
 * Every commit reachable from `tips`, newest first.  git's own default is
 * reverse chronological with the traversal order as a tie-break, which is
 * what this reproduces.
 */
void walk_commits(struct repo *r, const struct oid_array *tips,
		  void (*fn)(const oid_t *, const struct commit *, void *),
		  void *data)
{
	struct walk_ctx c;
	size_t i;

	walk_ctx_init(&c, r);
	walk_ctx_push_all(&c, tips);

	if (c.nr > 1)
		qsort(c.items, c.nr, sizeof(*c.items), walk_cmp);

	for (i = 0; i < c.nr; i++) {
		struct commit cm = COMMIT_INIT;
		read_commit(r, &c.items[i].oid, &cm);
		fn(&c.items[i].oid, &cm, data);
		commit_release(&cm);
	}

	walk_ctx_clear(&c);
}

/*
 * Every commit reachable from `tip`, `tip` included.  The order is the walk's
 * own, not sorted; a caller that wants newest first sorts for itself.
 */
void commit_ancestors(struct repo *r, const oid_t *tip, struct oid_array *out)
{
	struct walk_ctx c;
	oid_t peeled;
	size_t i;

	/* a tag names a commit without being one; the walk needs the commit */
	if (commit_peel(r, tip, OBJ_COMMIT, &peeled) < 0)
		return;

	walk_ctx_init(&c, r);
	walk_push(&c, &peeled);
	for (i = 0; i < c.nr; i++)
		oid_array_append(out, &c.items[i].oid);
	walk_ctx_clear(&c);
}

/*
 * The best common ancestors of two commits: the ones reachable from both that
 * are not themselves an ancestor of another one.  Histories that never meet
 * have none.
 *
 * Each common ancestor visited strikes out everything below it, so a commit
 * already struck out needs no walk of its own: whoever struck it out walked
 * its history too.  The number of walks is therefore the number of answers,
 * not the size of the shared history.
 */
size_t merge_bases(struct repo *r, const oid_t *a, const oid_t *b,
		   struct oid_array *out)
{
	struct oid_array anc_a = OID_ARRAY_INIT, anc_b = OID_ARRAY_INIT;
	struct oid_array dead = OID_ARRAY_INIT;
	size_t i, j;

	commit_ancestors(r, a, &anc_a);
	commit_ancestors(r, b, &anc_b);
	oid_array_clear(out);

	for (i = 0; i < anc_a.nr; i++) {
		struct oid_array below = OID_ARRAY_INIT;

		if (!oid_array_contains(&anc_b, &anc_a.oid[i]))
			continue;
		if (oid_array_contains(&dead, &anc_a.oid[i]))
			continue;

		commit_ancestors(r, &anc_a.oid[i], &below);
		for (j = 0; j < below.nr; j++) {
			if (oid_equal(&below.oid[j], &anc_a.oid[i]))
				continue;
			if (!oid_array_contains(&anc_b, &below.oid[j]))
				continue;
			if (!oid_array_contains(&dead, &below.oid[j]))
				oid_array_append(&dead, &below.oid[j]);
		}
		oid_array_clear(&below);
	}

	for (i = 0; i < anc_a.nr; i++)
		if (oid_array_contains(&anc_b, &anc_a.oid[i]) &&
		    !oid_array_contains(&dead, &anc_a.oid[i]))
			oid_array_append(out, &anc_a.oid[i]);

	oid_array_clear(&anc_a);
	oid_array_clear(&anc_b);
	oid_array_clear(&dead);
	return out->nr;
}

/* ------------------------------------------------------------------ */
/* revision argument lists                                             */

/* one end of a range: an empty end means HEAD, the way git reads it */
static int rev_end(struct repo *r, const char *text, size_t len, oid_t *out)
{
	char *name;
	int rc;

	if (!len)
		return resolve_rev(r, "HEAD", out);
	name = xstrndup(text, len);
	rc = resolve_rev(r, name, out);
	free(name);
	return rc;
}

/*
 * Read a revision argument list the way git's walkers do.  A plain revision
 * names what to include, ^<rev> names what to leave out, and the two range
 * shorthands expand into those same two sets:
 *
 *   A..B    B and its ancestors, less A and its ancestors
 *   A...B   what A or B can reach but the other cannot, which is two sets
 *           rather than one, so it is kept apart
 *
 * A...B is only ever one range, because folding two of them into a single
 * include/exclude pair is not something the shorthand can express.
 */
int rev_list_parse(struct repo *r, int argc, char **argv, struct rev_list *out)
{
	int i;

	memset(out, 0, sizeof *out);
	for (i = 0; i < argc; i++) {
		const char *arg = argv[i];
		const char *dots;
		oid_t oid;

		if (arg[0] == '^') {
			if (resolve_rev(r, arg + 1, &oid) < 0) {
				gp_error("unknown revision: %s", arg);
				return -1;
			}
			oid_array_append(&out->exclude, &oid);
			continue;
		}

		dots = strstr(arg, "...");
		if (dots) {
			oid_t left, right;

			if (out->has_second) {
				gp_error("only one A...B range at a time");
				return -1;
			}
			if (rev_end(r, arg, (size_t)(dots - arg), &left) < 0 ||
			    rev_end(r, dots + 3, strlen(dots + 3), &right) < 0) {
				gp_error("bad revision range: %s", arg);
				return -1;
			}
			oid_array_append(&out->include, &left);
			oid_array_append(&out->exclude, &right);
			oid_array_append(&out->include2, &right);
			oid_array_append(&out->exclude2, &left);
			out->has_second = 1;
			continue;
		}

		dots = strstr(arg, "..");
		if (dots) {
			oid_t left, right;

			if (rev_end(r, arg, (size_t)(dots - arg), &left) < 0 ||
			    rev_end(r, dots + 2, strlen(dots + 2), &right) < 0) {
				gp_error("bad revision range: %s", arg);
				return -1;
			}
			oid_array_append(&out->exclude, &left);
			oid_array_append(&out->include, &right);
			continue;
		}

		if (resolve_rev(r, arg, &oid) < 0) {
			gp_error("unknown revision: %s", arg);
			return -1;
		}
		oid_array_append(&out->include, &oid);
	}
	return 0;
}

void rev_list_release(struct rev_list *l)
{
	oid_array_clear(&l->include);
	oid_array_clear(&l->exclude);
	oid_array_clear(&l->include2);
	oid_array_clear(&l->exclude2);
}

/* commits `include` can reach that `exclude` cannot, newest first */
static void rev_list_side(struct repo *r, const struct oid_array *include,
			  const struct oid_array *exclude,
			  struct walk_ctx *out)
{
	struct walk_ctx cut;
	size_t i, j;

	walk_ctx_init(&cut, r);
	walk_ctx_push_all(&cut, exclude);

	walk_ctx_init(out, r);
	walk_ctx_push_all(out, include);

	for (i = j = 0; i < out->nr; i++) {
		if (oid_array_contains(&cut.seen, &out->items[i].oid))
			continue;
		out->items[j++] = out->items[i];
	}
	out->nr = j;
	walk_ctx_clear(&cut);
}

void rev_list_run(struct repo *r, const struct rev_list *l,
		  void (*fn)(const oid_t *, const struct commit *, void *),
		  void *data)
{
	struct walk_ctx a, b;
	size_t i, n;

	rev_list_side(r, &l->include, &l->exclude, &a);
	walk_ctx_init(&b, r);
	if (l->has_second)
		rev_list_side(r, &l->include2, &l->exclude2, &b);

	/*
	 * The two sides of A...B exclude each other's history, so no commit can
	 * be on both and the lists simply join.
	 */
	if (b.nr) {
		if (a.nr + b.nr > a.alloc) {
			a.alloc = a.nr + b.nr;
			a.items = xrealloc(a.items, a.alloc * sizeof(*a.items));
		}
		for (i = 0; i < b.nr; i++)
			a.items[a.nr + i] = b.items[i];
		a.nr += b.nr;
	}
	/* the two walks counted from zero each, so renumber before relying on it */
	for (i = 0; i < a.nr; i++)
		a.items[i].seq = i;

	if (a.nr > 1)
		qsort(a.items, a.nr, sizeof(*a.items), walk_cmp);

	n = a.nr;
	for (i = 0; i < n; i++) {
		struct commit cm = COMMIT_INIT;
		read_commit(r, &a.items[i].oid, &cm);
		fn(&a.items[i].oid, &cm, data);
		commit_release(&cm);
	}

	walk_ctx_clear(&a);
	walk_ctx_clear(&b);
}

int is_ancestor(struct repo *r, const oid_t *ancestor, const oid_t *tip)
{
	struct oid_array seen = OID_ARRAY_INIT;
	struct oid_array frontier = OID_ARRAY_INIT;
	int found = 0;

	oid_array_append(&frontier, tip);
	while (frontier.nr && !found) {
		oid_t cur = frontier.oid[--frontier.nr];
		struct commit cm = COMMIT_INIT;
		size_t i;

		if (oid_array_contains(&seen, &cur))
			continue;
		oid_array_append(&seen, &cur);

		if (oid_equal(&cur, ancestor)) {
			found = 1;
			break;
		}
		if (odb_read(&r->odb, &cur, NULL, NULL) < 0)
			continue;
		read_commit(r, &cur, &cm);
		for (i = 0; i < cm.parents.nr; i++)
			oid_array_append(&frontier, &cm.parents.oid[i]);
		commit_release(&cm);
	}
	oid_array_clear(&seen);
	oid_array_clear(&frontier);
	return found;
}
