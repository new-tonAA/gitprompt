/*
 * trace.c - which prompt asked for which piece of the code.
 *
 * A commit is a state, and the prompts it carries are the steps that made it.
 * Each prompt file keeps the work tree as it stood when that prompt was given,
 * so a run of prompts is a chain of states -- the parent's tree, the first
 * prompt's snapshot, the second's, and so on up to the commit's own tree -- and
 * the step between two of them is one prompt's change.
 *
 * None of this is inferred from the code.  A diff says what changed; it never
 * says who asked for it, and no amount of looking at the result can recover that
 * -- two prompts that touch the same file are one diff.  So the chain is written
 * down as the prompts are given, and what this file does is read it back.
 *
 * A commit whose prompts were recorded before snapshots existed, or whose
 * snapshots are gone, has no chain; everything here then says so rather than
 * guessing, and the caller falls back to naming the commit's prompts as a whole.
 */
#include "gp.h"

/* ------------------------------------------------------------------ */
/* reading a commit's chain                                            */

/*
 * Every prompt file in a tree, as the id it holds and the snapshot it names.
 * A file that is not a prompt, or does not read back as one, is left out: the
 * caller matches it against the ids the commit lists, and an id it cannot find
 * is exactly the case where there is no chain to follow.
 */
struct snap_map {
	char **id;
	char **snap;            /* hex of the tree, or NULL */
	size_t nr, alloc;
};

struct snap_ctx {
	struct repo *r;
	const char *dir;
	size_t dl;
	struct snap_map *m;
};

static void snap_cb(const char *path, u32 mode, const oid_t *oid, void *ud)
{
	struct snap_ctx *c = ud;
	const char *rest;
	struct buf b;
	struct prompt p = PROMPT_INIT;

	(void)mode;
	if (!is_prompt_path(c->dir, c->dl, path, &rest))
		return;

	buf_init(&b);
	if (odb_read(&c->r->odb, oid, NULL, &b) < 0) {
		buf_release(&b);
		return;
	}
	if (prompt_from_file(&p, b.b, b.len) && p.id && !strncmp(p.id, "p_", 2)) {
		if (c->m->nr == c->m->alloc) {
			c->m->alloc = c->m->alloc ? c->m->alloc * 2 : 16;
			c->m->id = xrealloc(c->m->id,
					    c->m->alloc * sizeof(*c->m->id));
			c->m->snap = xrealloc(c->m->snap,
					      c->m->alloc * sizeof(*c->m->snap));
		}
		c->m->id[c->m->nr] = xstrdup(p.id);
		c->m->snap[c->m->nr] = p.snapshot ? xstrdup(p.snapshot) : NULL;
		c->m->nr++;
	}
	prompt_release(&p);
	buf_release(&b);
}

static void snap_map_release(struct snap_map *m)
{
	size_t i;

	for (i = 0; i < m->nr; i++) {
		free(m->id[i]);
		free(m->snap[i]);
	}
	free(m->id);
	free(m->snap);
	memset(m, 0, sizeof *m);
}

static const char *snap_map_lookup(const struct snap_map *m, const char *id)
{
	size_t i;

	for (i = 0; i < m->nr; i++)
		if (!strcmp(m->id[i], id))
			return m->snap[i];
	return NULL;
}

static void trace_push(struct trace *t, const char *prompt, const oid_t *from,
		       int have_from, const oid_t *to)
{
	struct trace_step *s;

	t->e = xrealloc(t->e, (t->nr + 1) * sizeof(*t->e));
	s = &t->e[t->nr++];
	memset(s, 0, sizeof *s);
	s->prompt = prompt ? xstrdup(prompt) : NULL;
	if (have_from)
		s->from = *from;
	s->have_from = have_from;
	s->to = *to;
}

void trace_of_commit(struct repo *r, const struct commit *c, struct trace *out)
{
	struct snap_map m = { NULL, NULL, 0, 0 };
	struct snap_ctx sc;
	oid_t *snaps;
	oid_t from;
	char *dir;
	size_t dl, i, n = c->nr_prompts;
	int have_from;

	memset(out, 0, sizeof *out);

	if (!n) {
		out->why = xstrdup("the commit carries no prompt");
		return;
	}

	dir = xstrdup(repo_prompt_dir(r));
	dl = strlen(dir);
	sc.r = r;
	sc.dir = dir;
	sc.dl = dl;
	sc.m = &m;
	load_tree_flat(r, &c->tree, "", snap_cb, &sc);
	free(dir);

	snaps = xcalloc(n, sizeof(*snaps));
	for (i = 0; i < n; i++) {
		const char *hex = snap_map_lookup(&m, c->prompts[i]);

		if (!hex || oid_parse(&snaps[i], hex) < 0 ||
		    !odb_exists(&r->odb, &snaps[i])) {
			out->why = xstrdup("a prompt it carries has no snapshot");
			goto done;
		}
	}

	have_from = c->parents.nr > 0;
	if (have_from) {
		struct commit parent = COMMIT_INIT;

		read_commit(r, &c->parents.oid[0], &parent);
		from = parent.tree;
		commit_release(&parent);
	}

	/*
	 * The work that was in the tree before the first prompt was recorded.
	 * Usually there is none, because a prompt is recorded onto a clean tree,
	 * and the step is dropped rather than printed empty.
	 */
	if (have_from && !oid_equal(&from, &snaps[0]))
		trace_push(out, NULL, &from, 1, &snaps[0]);

	for (i = 0; i < n; i++)
		trace_push(out, c->prompts[i], &snaps[i], 1,
			   i + 1 < n ? &snaps[i + 1] : &c->tree);

done:
	free(snaps);
	snap_map_release(&m);
}

void trace_release(struct trace *t)
{
	size_t i;

	for (i = 0; i < t->nr; i++)
		free(t->e[i].prompt);
	free(t->e);
	free(t->why);
	memset(t, 0, sizeof *t);
}

/*
 * A step always carries the prompt file that was written during it, because a
 * prompt file is not in the index and so cannot be in any snapshot -- it shows
 * up as a file the commit added.  Naming that file underneath its own prompt
 * says nothing about the code, so its block is dropped, which leaves the
 * annotation about what was actually asked for.
 */
static void drop_prompt_blocks(struct repo *r, struct buf *b)
{
	const char *dir = repo_prompt_dir(r);
	size_t dl = strlen(dir);
	struct buf keep;
	const char *p, *end;
	int skip = 0;

	if (!b->len)
		return;

	buf_init(&keep);
	p = (const char *)b->b;
	end = p + b->len;
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t l = nl ? (size_t)(nl - p) + 1 : (size_t)(end - p);

		if (l > 13 && !memcmp(p, "diff --git a/", 13)) {
			size_t k;

			skip = 0;
			for (k = 13; k + 3 < l; k++) {
				if (p[k] == ' ' && p[k + 1] == 'b' &&
				    p[k + 2] == '/') {
					struct buf pth;
					const char *rest;

					buf_init(&pth);
					buf_add(&pth, p + 13,
						(size_t)(p + k - (p + 13)));
					skip = is_prompt_path(dir, dl,
							      buf_cstr(&pth),
							      &rest);
					buf_release(&pth);
					break;
				}
			}
		}
		if (!skip)
			buf_add(&keep, p, l);
		p += l;
	}
	buf_release(b);
	*b = keep;
}

void trace_render(struct repo *r, const struct trace *t, struct buf *out)
{
	size_t i;

	for (i = 0; i < t->nr; i++) {
		struct buf d;

		buf_init(&d);
		diff_trees(r, t->e[i].have_from ? &t->e[i].from : NULL,
			   &t->e[i].to, &d, 0);
		drop_prompt_blocks(r, &d);
		if (d.len) {
			if (out->len)
				buf_addch(out, '\n');
			if (t->e[i].prompt)
				buf_addf(out, "prompt %s\n", t->e[i].prompt);
			else
				buf_addstr(out, "prompt (none)\n");
			buf_add(out, d.b, d.len);
		}
		buf_release(&d);
	}
}

/* ------------------------------------------------------------------ */
/* one file, line by line                                              */

/* the file at a state, or 0 when that state does not hold it */
static int blob_at(struct repo *r, const oid_t *tree, int have,
		   const char *path, struct buf *out)
{
	struct index_state ist;
	struct index_entry *e;
	int found = 0;

	buf_reset(out);
	if (!have)
		return 0;

	memset(&ist, 0, sizeof ist);
	read_tree_into_index(r, &ist, tree, "");
	e = index_get(&ist, path);
	if (e && odb_read(&r->odb, &e->oid, NULL, out) == 0)
		found = 1;
	else
		buf_reset(out);
	index_release(&ist);
	return found;
}

int trace_file_prompts(struct repo *r, const struct trace *t, const char *path,
		       const char ***out, size_t *nr)
{
	struct buf a, b;
	const char **attr;
	size_t n = 0, i;
	int a_present = 0;

	if (!t->nr)
		return -1;

	buf_init(&a);
	buf_init(&b);

	/* the lines the file had when the chain begins: nobody's yet */
	a_present = blob_at(r, &t->e[0].from, t->e[0].have_from, path, &a);
	{
		struct dline *la = NULL;
		size_t na = 0;

		diff_split_lines(a_present ? (const char *)a.b : "",
				 a_present ? a.len : 0, &la, &na);
		attr = xcalloc(na + 1, sizeof(*attr));
		n = na;
		free(la);
	}

	for (i = 0; i < t->nr; i++) {
		struct dline *la = NULL, *lb = NULL;
		struct oline *o = NULL;
		struct stat_counts counts;
		const char **next;
		size_t na = 0, nb = 0, no = 0, k;
		int b_present;

		b_present = blob_at(r, &t->e[i].to, 1, path, &b);
		diff_split_lines(a_present ? (const char *)a.b : "",
				 a_present ? a.len : 0, &la, &na);
		diff_split_lines(b_present ? (const char *)b.b : "",
				 b_present ? b.len : 0,
			    &lb, &nb);
		lcs_diff(la, na, lb, nb, &o, &no, &counts);

		/* every line of the new state is either one that carried
		 * through or one this step added, and a line added here is one
		 * this prompt asked for */
		next = xcalloc(nb + 1, sizeof(*next));
		for (k = 0; k < no; k++) {
			if (o[k].op == ' ')
				next[o[k].b] = attr[o[k].a];
			else if (o[k].op == '+')
				next[o[k].b] = t->e[i].prompt;
		}

		free(attr);
		attr = next;
		n = nb;
		free(la);
		free(lb);
		free(o);

		buf_release(&a);
		a = b;
		buf_init(&b);
		a_present = b_present;
	}

	buf_release(&a);
	buf_release(&b);
	*out = attr;
	*nr = n;
	return 0;
}
