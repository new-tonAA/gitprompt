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
	oid_t cur = *start;

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
			if (!close) {
				free(name);
				return -1;
			}
			*close = '\0';
			want = obj_type_from_name(peel + 2);
			if (want == OBJ_NONE) {
				free(name);
				return -1;
			}
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

	memset(&c, 0, sizeof c);
	c.r = r;

	for (i = 0; i < tips->nr; i++) {
		oid_t peeled;
		if (commit_peel(r, &tips->oid[i], OBJ_COMMIT, &peeled) == 0)
			walk_push(&c, &peeled);
	}

	if (c.nr > 1)
		qsort(c.items, c.nr, sizeof(*c.items), walk_cmp);

	for (i = 0; i < c.nr; i++) {
		struct commit cm = COMMIT_INIT;
		read_commit(r, &c.items[i].oid, &cm);
		fn(&c.items[i].oid, &cm, data);
		commit_release(&cm);
	}

	free(c.items);
	oid_array_clear(&c.seen);
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
