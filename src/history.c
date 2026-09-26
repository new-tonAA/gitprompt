/*
 * history.c - gathering prompts back out of the object store and the work
 * tree, grouping them into sessions, and rendering the reconstruction.
 *
 * The whole point of gitprompt is this file: after cloning a repository an
 * agent must be able to read the prompt history back in the order it was
 * written, with the session boundaries intact.  Everything the order needs
 * is in the frontmatter of each file, so the reconstruction does not depend
 * on the commit graph, on ref names, or on any side channel.
 */
#include "gp.h"

/* ------------------------------------------------------------------ */
/* a list of sessions, internal to this file                           */

struct session_list {
	struct session **e;
	size_t nr, alloc;
};

static void session_list_push(struct session_list *l, struct session *s)
{
	if (l->nr == l->alloc) {
		l->alloc = l->alloc ? l->alloc * 2 : 8;
		l->e = xrealloc(l->e, l->alloc * sizeof(*l->e));
	}
	l->e[l->nr++] = s;
}

/*
 * The sessions themselves are handed on to the session_groups, which own
 * them from then on, so only the array is released here.
 */
static struct session *session_list_get(struct session_list *l, const char *id)
{
	size_t i;
	for (i = 0; i < l->nr; i++)
		if (l->e[i]->id && !strcmp(l->e[i]->id, id))
			return l->e[i];
	return NULL;
}

/* ------------------------------------------------------------------ */
/* list bookkeeping                                                    */

void prompt_list_release(struct prompt_list *l)
{
	size_t i;
	for (i = 0; i < l->nr; i++) {
		prompt_release(l->e[i].prompt);
		free(l->e[i].prompt);
		free(l->e[i].commit_sha);
		free(l->e[i].path);
	}
	free(l->e);
	l->e = NULL;
	l->nr = l->alloc = 0;
}

static void prompt_list_push(struct prompt_list *l, struct prompt *p,
			     const char *path, const oid_t *commit)
{
	if (l->nr == l->alloc) {
		l->alloc = l->alloc ? l->alloc * 2 : 16;
		l->e = xrealloc(l->e, l->alloc * sizeof(*l->e));
	}
	l->e[l->nr].prompt = p;
	l->e[l->nr].path = xstrdup(path);
	if (commit) {
		char hex[GP_SHA1_HEXSZ + 1];
		oid_hex(commit, hex);
		l->e[l->nr].commit = *commit;
		l->e[l->nr].commit_sha = xstrdup(hex);
	} else {
		memset(&l->e[l->nr].commit, 0, sizeof(oid_t));
		l->e[l->nr].commit_sha = NULL;
	}
	l->nr++;
}

static struct prompt_ref *prompt_list_find_by_id(struct prompt_list *l,
						 const char *id)
{
	size_t i;
	for (i = 0; i < l->nr; i++)
		if (l->e[i].prompt->id && !strcmp(l->e[i].prompt->id, id))
			return &l->e[i];
	return NULL;
}

/* ------------------------------------------------------------------ */
/* recognising a prompt or a session in a file's bytes                 */

/*
 * Cheaply pull `id:` out of a frontmatter block.  Files that are not
 * gitprompt files at all (a README, a config) are rejected here without
 * paying for a full parse.
 */
static int peek_id(const void *data, size_t len, char *out, size_t outsz)
{
	const char *p = data, *end = p + len;
	int in_front = 0, seen = 0;

	out[0] = '\0';
	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		size_t llen = eol ? (size_t)(eol - p) : (size_t)(end - p);

		if (llen >= 3 && !memcmp(p, "---", 3) && llen <= 4) {
			if (!seen) {
				seen = 1;
				in_front = 1;
			} else {
				return 0;       /* end of the block */
			}
			p = eol ? eol + 1 : end;
			continue;
		}
		if (in_front && llen > 4 && !memcmp(p, "id: ", 4)) {
			size_t n = llen - 4;
			if (n >= outsz)
				n = outsz - 1;
			memcpy(out, p + 4, n);
			out[n] = '\0';
			return 1;
		}
		p = eol ? eol + 1 : end;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* scanning                                                           */

struct scan_ctx {
	struct repo *r;
	struct prompt_list *prompts;     /* may be NULL */
	struct session_list *sessions;   /* may be NULL */
	const oid_t *commit;
};

static void scan_bytes(struct scan_ctx *c, const void *data, size_t len,
		       const char *path)
{
	char id[64];

	if (!peek_id(data, len, id, sizeof id))
		return;

	if (!strncmp(id, "p_", 2) && c->prompts) {
		struct prompt *p = xcalloc(1, sizeof(*p));
		struct prompt_ref *existing;

		prompt_from_file(p, data, len);
		if (!p->id) {
			prompt_release(p);
			free(p);
			return;
		}
		free(p->path);
		p->path = xstrdup(path);

		existing = prompt_list_find_by_id(c->prompts, p->id);
		if (existing) {
			/* a later scan overrides: the work tree wins over a
			 * commit, and a newer commit wins over an older one */
			prompt_release(existing->prompt);
			free(existing->prompt);
			existing->prompt = p;
			free(existing->path);
			existing->path = xstrdup(path);
			return;
		}
		prompt_list_push(c->prompts, p, path, c->commit);
		return;
	}

	if (!strncmp(id, "s_", 2) && c->sessions) {
		struct session *s = xcalloc(1, sizeof(*s));
		struct session *existing;

		session_from_file(s, data, len);
		if (!s->id) {
			session_release(s);
			free(s);
			return;
		}
		existing = session_list_get(c->sessions, s->id);
		if (existing) {
			session_release(existing);
			*existing = *s;
			free(s);
			return;
		}
		session_list_push(c->sessions, s);
	}
}

static int under_prompt_dir(struct repo *r, const char *path)
{
	const char *dir = repo_prompt_dir(r);
	size_t n;

	if (!dir[0])
		return 1;
	n = strlen(dir);
	return !strncmp(path, dir, n) && (path[n] == '/' || path[n] == '\0');
}

static void scan_worktree_file(const char *relpath, void *ud)
{
	struct scan_ctx *c = ud;
	struct buf b;
	char *full;

	if (!under_prompt_dir(c->r, relpath))
		return;
	full = c->r->root ? xstrfmt("%s/%s", c->r->root, relpath)
			  : xstrdup(relpath);
	buf_init(&b);
	if (read_file(full, &b) == 0)
		scan_bytes(c, b.b, b.len, relpath);
	buf_release(&b);
	free(full);
}

static void scan_tree_cb(const char *path, u32 mode, const oid_t *oid, void *ud)
{
	struct scan_ctx *c = ud;
	struct buf b;

	if (mode == MODE_TREE)
		return;
	if (!under_prompt_dir(c->r, path))
		return;
	buf_init(&b);
	if (odb_read(&c->r->odb, oid, NULL, &b) == 0)
		scan_bytes(c, b.b, b.len, path);
	buf_release(&b);
}

struct tip_ctx {
	struct repo *r;
	struct scan_ctx *scan;
	struct oid_array seen;
};

static void tip_ref_cb(const char *name, const oid_t *oid, void *ud)
{
	struct tip_ctx *t = ud;
	oid_t commit;

	if (oid_array_contains(&t->seen, oid))
		return;
	oid_array_append(&t->seen, oid);
	if (commit_peel(t->r, oid, OBJ_COMMIT, &commit) < 0)
		return;
	{
		struct commit c = COMMIT_INIT;
		read_commit(t->r, &commit, &c);
		t->scan->commit = &commit;
		load_tree_flat(t->r, &c.tree, "", scan_tree_cb, t->scan);
		t->scan->commit = NULL;
		commit_release(&c);
	}
}

/*
 * Gather prompts (and, if asked, sessions).  With no `only_tree` the tip
 * of every ref is scanned -- which is exactly the set of files a clone
 * would leave on disk -- and the work tree is folded in afterwards so
 * prompts that have not been committed yet still appear.
 */
static void scan_all(struct repo *r, const oid_t *only_tree,
		     struct prompt_list *prompts, struct session_list *sessions)
{
	struct scan_ctx c;
	struct tip_ctx t;

	memset(&c, 0, sizeof c);
	c.r = r;
	c.prompts = prompts;
	c.sessions = sessions;

	if (only_tree) {
		load_tree_flat(r, only_tree, "", scan_tree_cb, &c);
	} else {
		memset(&t, 0, sizeof t);
		t.r = r;
		t.scan = &c;
		refs_list(&r->refs, "refs/heads/", tip_ref_cb, &t);
		refs_list(&r->refs, "refs/tags/", tip_ref_cb, &t);
		refs_list(&r->refs, "refs/remotes/", tip_ref_cb, &t);
		refs_list_packed(&r->refs, "refs/", tip_ref_cb, &t);
		oid_array_clear(&t.seen);
	}

	/* The work tree last, so an uncommitted edit is what is reported -- but
	 * only when no revision was named.  `replay <ref>` asks what the
	 * history looked like at that point, and uncommitted files are later
	 * than every ref, so folding them in would make the argument mean
	 * nothing in any working tree that has one. */
	if (!only_tree && r->root)
		walk_worktree(r, scan_worktree_file, &c);
}

void collect_prompts(struct repo *r, struct prompt_list *out)
{
	/* the scan appends, so the caller's list has to start empty; doing it
	 * here keeps a caller that forgot PROMPT_LIST_INIT from appending into
	 * whatever the stack happened to hold */
	memset(out, 0, sizeof *out);
	scan_all(r, NULL, out, NULL);
}

void collect_prompts_from_ref(struct repo *r, const char *rev,
			      struct prompt_list *out)
{
	oid_t tree;
	if (!rev || !*rev) {
		collect_prompts(r, out);
		return;
	}
	if (resolve_rev_tree(r, rev, &tree) < 0)
		gp_die("not a tree: %s", rev);
	memset(out, 0, sizeof *out);
	scan_all(r, &tree, out, NULL);
}

/* ------------------------------------------------------------------ */
/* ordering                                                           */

static int prompt_seq_cmp(const void *a, const void *b)
{
	const struct prompt_ref *x = a, *y = b;
	const struct prompt *p = x->prompt, *q = y->prompt;

	if (p->seq != q->seq)
		return p->seq < q->seq ? -1 : 1;
	if (p->ts != q->ts)
		return p->ts < q->ts ? -1 : 1;
	return strcmp(p->id ? p->id : "", q->id ? q->id : "");
}

static int session_cmp(const void *a, const void *b)
{
	const struct session *x = *(struct session *const *)a;
	const struct session *y = *(struct session *const *)b;
	if (x->started_ts != y->started_ts)
		return x->started_ts < y->started_ts ? -1 : 1;
	return strcmp(x->id ? x->id : "", y->id ? y->id : "");
}

/* append a group; a NULL session means the unattributed bucket */
static struct session_group *group_append(struct session_groups *out,
					  struct session *s)
{
	struct session_group *g;

	if (out->nr == out->alloc) {
		out->alloc = out->alloc ? out->alloc * 2 : 8;
		out->g = xrealloc(out->g, out->alloc * sizeof(*out->g));
	}
	g = &out->g[out->nr++];
	g->session = s;
	memset(&g->prompts, 0, sizeof(g->prompts));
	return g;
}

/*
 * Copy a prompt reference into a group.  The prompt object itself stays
 * owned by the prompt_list it came from; the group only borrows the
 * pointer, and session_groups_release frees just the copies made here.
 */
static void group_take_prompt(struct session_group *g,
			      const struct prompt_ref *src)
{
	struct prompt_list *l = &g->prompts;

	if (l->nr == l->alloc) {
		l->alloc = l->alloc ? l->alloc * 2 : 8;
		l->e = xrealloc(l->e, l->alloc * sizeof(*l->e));
	}
	l->e[l->nr].prompt = src->prompt;
	l->e[l->nr].commit = src->commit;
	l->e[l->nr].path = xstrdup(src->path);
	l->e[l->nr].commit_sha = src->commit_sha ? xstrdup(src->commit_sha)
						 : NULL;
	l->nr++;
}

void group_by_session(struct repo *r, const struct prompt_list *in,
		      struct session_groups *out)
{
	struct session_list sessions = { NULL, 0, 0 };
	size_t i;

	scan_all(r, NULL, NULL, &sessions);
	if (sessions.nr > 1)
		qsort(sessions.e, sessions.nr, sizeof(*sessions.e), session_cmp);

	/* one group per known session, in start order */
	for (i = 0; i < sessions.nr; i++)
		group_append(out, sessions.e[i]);
	free(sessions.e);

	/* then place every prompt */
	for (i = 0; i < in->nr; i++) {
		const struct prompt *p = in->e[i].prompt;
		size_t g;
		struct session_group *target = NULL;

		if (p->session && p->session[0]) {
			for (g = 0; g < out->nr; g++)
				if (out->g[g].session &&
				    !strcmp(out->g[g].session->id, p->session)) {
					target = &out->g[g];
					break;
				}
			if (!target) {
				/* a session file that never made it into the
				 * repository: make a placeholder for it, so
				 * the prompts keep their boundary */
				struct session *s = xcalloc(1, sizeof(*s));
				s->id = xstrdup(p->session);
				s->title = xstrdup("(session file not recorded)");
				s->started_ts = p->ts;
				target = group_append(out, s);
			}
		} else {
			/* unattributed prompts collect in a group of their
			 * own, created on first use and kept last */
			if (!out->nr || out->g[out->nr - 1].session != NULL)
				target = group_append(out, NULL);
			else
				target = &out->g[out->nr - 1];
		}

		group_take_prompt(target, &in->e[i]);
	}

	for (i = 0; i < out->nr; i++)
		if (out->g[i].prompts.nr > 1)
			qsort(out->g[i].prompts.e, out->g[i].prompts.nr,
			      sizeof(*out->g[i].prompts.e), prompt_seq_cmp);
}

void session_groups_release(struct session_groups *g)
{
	size_t i, j;
	for (i = 0; i < g->nr; i++) {
		if (g->g[i].session) {
			session_release(g->g[i].session);
			free(g->g[i].session);
		}
		for (j = 0; j < g->g[i].prompts.nr; j++) {
			/* the prompt body belongs to the prompt_list that
			 * produced it; only the copies made here are freed */
			free(g->g[i].prompts.e[j].path);
			free(g->g[i].prompts.e[j].commit_sha);
		}
		free(g->g[i].prompts.e);
	}
	free(g->g);
	g->g = NULL;
	g->nr = g->alloc = 0;
}

/* ------------------------------------------------------------------ */
/* rendering                                                          */

static size_t total_prompts(const struct session_groups *g)
{
	size_t i, n = 0;
	for (i = 0; i < g->nr; i++)
		n += g->g[i].prompts.nr;
	return n;
}

static void quote_block(struct buf *out, const char *text)
{
	const char *p = text;
	if (!p || !*p) {
		buf_addstr(out, "> (empty)\n");
		return;
	}
	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		if (len)
			buf_addf(out, "> %.*s\n", (int)len, p);
		else
			buf_addstr(out, ">\n");
		if (!nl)
			break;
		p = nl + 1;
		if (!*p)
			break;
	}
}

void replay_markdown(struct repo *r, const struct session_groups *g,
		     struct buf *out, int stat_only)
{
	size_t i, j;
	const char *dir = repo_prompt_dir(r);

	buf_reset(out);
	buf_addstr(out, "# Prompt history\n\n");
	buf_addf(out, "Reconstructed by gitprompt: %lu prompt(s) in %lu session(s), "
		      "in the order they were written.\n",
		 (unsigned long)total_prompts(g), (unsigned long)g->nr);
	buf_addf(out, "\nPrompt files live under `%s/`.\n\n", dir);

	if (stat_only) {
		for (i = 0; i < g->nr; i++) {
			const struct session_group *grp = &g->g[i];
			if (grp->session)
				buf_addf(out, "- `%s` %s -- %lu prompt(s)\n",
					 grp->session->id,
					 grp->session->title
						 ? grp->session->title : "",
					 (unsigned long)grp->prompts.nr);
			else
				buf_addf(out, "- (unattributed) -- %lu prompt(s)\n",
					 (unsigned long)grp->prompts.nr);
		}
		return;
	}

	for (i = 0; i < g->nr; i++) {
		const struct session_group *grp = &g->g[i];

		if (grp->session) {
			buf_addf(out, "## %lu. %s\n\n", (unsigned long)(i + 1),
				 grp->session->title ? grp->session->title
						     : "(untitled session)");
			buf_addf(out, "- id: `%s`\n", grp->session->id);
			buf_addf(out, "- started: %s\n",
				 grp->session->started_at
					 ? grp->session->started_at : "(unknown)");
			if (grp->session->ended_at)
				buf_addf(out, "- ended: %s\n",
					 grp->session->ended_at);
			if (grp->session->author)
				buf_addf(out, "- author: %s\n",
					 grp->session->author);
			if (grp->session->notes)
				buf_addf(out, "- notes: %s\n",
					 grp->session->notes);
			buf_addf(out, "- prompts: %lu\n",
				 (unsigned long)grp->prompts.nr);
			buf_addch(out, '\n');
		} else {
			buf_addstr(out, "## Unattributed prompts\n\n");
			buf_addstr(out,
				   "These were recorded without a session.\n\n");
		}

		for (j = 0; j < grp->prompts.nr; j++) {
			const struct prompt *p = grp->prompts.e[j].prompt;

			buf_addf(out, "### %lu.%lu `%s`\n\n",
				 (unsigned long)(i + 1), (unsigned long)(j + 1),
				 p->id ? p->id : "(no id)");
			if (p->timestamp)
				buf_addf(out, "- recorded: %s\n", p->timestamp);
			if (p->author)
				buf_addf(out, "- author: %s\n", p->author);
			if (p->model)
				buf_addf(out, "- model: %s\n", p->model);
			if (p->nr_tags) {
				size_t k;
				buf_addstr(out, "- tags:");
				for (k = 0; k < p->nr_tags; k++)
					buf_addf(out, "%s %s", k ? "," : "",
						 p->tags[k]);
				buf_addch(out, '\n');
			}
			if (p->path)
				buf_addf(out, "- file: `%s`\n", p->path);
			if (p->parent_prompt)
				buf_addf(out, "- follows: `%s`\n",
					 p->parent_prompt);
			buf_addch(out, '\n');
			quote_block(out, p->body);
			if (p->outcome) {
				buf_addstr(out, "\n**Outcome.** ");
				buf_addstr(out, p->outcome);
				buf_addch(out, '\n');
			}
			buf_addch(out, '\n');
		}
	}
}

void replay_text(struct repo *r, const struct session_groups *g,
		 struct buf *out)
{
	size_t i, j;
	(void)r;

	buf_reset(out);
	buf_addf(out, "%lu prompt(s) in %lu session(s)\n\n",
		 (unsigned long)total_prompts(g), (unsigned long)g->nr);
	for (i = 0; i < g->nr; i++) {
		const struct session_group *grp = &g->g[i];
		if (grp->session)
			buf_addf(out, "== %s  [%s]\n", grp->session->title
							       ? grp->session->title
							       : "(untitled)",
				 grp->session->id);
		else
			buf_addstr(out, "== (unattributed)\n");
		for (j = 0; j < grp->prompts.nr; j++) {
			const struct prompt *p = grp->prompts.e[j].prompt;
			const char *b = p->body ? p->body : "";
			size_t len = strcspn(b, "\n");
			buf_addf(out, "  %s  %.*s\n", p->id ? p->id : "?", (int)len,
				 b);
			if (p->outcome)
				buf_addf(out, "      outcome: %s\n", p->outcome);
		}
		buf_addch(out, '\n');
	}
}

void replay_json(struct repo *r, const struct session_groups *g, struct buf *out)
{
	size_t i, j;
	struct buf scratch;

	(void)r;
	buf_init(&scratch);
	buf_reset(out);
	buf_addstr(out, "{\n");
	buf_addf(out, "  \"prompt_count\": %lu,\n",
		 (unsigned long)total_prompts(g));
	buf_addf(out, "  \"session_count\": %lu,\n", (unsigned long)g->nr);
	buf_addstr(out, "  \"sessions\": [\n");

	for (i = 0; i < g->nr; i++) {
		const struct session_group *grp = &g->g[i];
		buf_addstr(out, "    {\n");
		if (grp->session) {
			session_to_json(grp->session, &scratch);
			buf_addstr(out, "      \"session\": ");
			buf_add(out, scratch.b, scratch.len);
			buf_addstr(out, ",\n");
		} else {
			buf_addstr(out, "      \"session\": null,\n");
		}
		buf_addstr(out, "      \"prompts\": [\n");
		for (j = 0; j < grp->prompts.nr; j++) {
			prompt_to_json(grp->prompts.e[j].prompt, &scratch);
			buf_addstr(out, "        ");
			buf_add(out, scratch.b, scratch.len);
			buf_addstr(out, j + 1 < grp->prompts.nr ? ",\n" : "\n");
		}
		buf_addstr(out, "      ]\n");
		buf_addstr(out, i + 1 < g->nr ? "    },\n" : "    }\n");
	}
	buf_addstr(out, "  ]\n}\n");
	buf_release(&scratch);
}

void replay_layout(struct repo *r, const struct session_groups *g, const char *dir)
{
	size_t i, j;
	struct buf index, one;

	mkdir_p(dir);
	buf_init(&index);
	buf_init(&one);

	buf_addstr(&index, "# Reconstructed prompt history\n\n");
	buf_addf(&index, "%lu prompt(s) in %lu session(s).\n\n",
		 (unsigned long)total_prompts(g), (unsigned long)g->nr);

	for (i = 0; i < g->nr; i++) {
		const struct session_group *grp = &g->g[i];
		char *name;
		char *slug;
		char *path;

		slug = slugify(grp->session && grp->session->title
				       ? grp->session->title : "unattributed", 40);
		name = xstrfmt("%02lu-%s.md", (unsigned long)(i + 1), slug);
		free(slug);
		path = xstrfmt("%s/%s", dir, name);

		buf_reset(&one);
		if (grp->session) {
			buf_addf(&one, "# %s\n\n", grp->session->title
							  ? grp->session->title
							  : "(untitled session)");
			buf_addf(&one, "- id: `%s`\n", grp->session->id);
			buf_addf(&one, "- started: %s\n",
				 grp->session->started_at
					 ? grp->session->started_at : "(unknown)");
			if (grp->session->ended_at)
				buf_addf(&one, "- ended: %s\n",
					 grp->session->ended_at);
			if (grp->session->author)
				buf_addf(&one, "- author: %s\n",
					 grp->session->author);
			buf_addch(&one, '\n');
		} else {
			buf_addstr(&one, "# Unattributed prompts\n\n");
		}

		for (j = 0; j < grp->prompts.nr; j++) {
			const struct prompt *p = grp->prompts.e[j].prompt;
			buf_addf(&one, "## `%s`", p->id ? p->id : "(no id)");
			if (p->timestamp)
				buf_addf(&one, " -- %s", p->timestamp);
			buf_addstr(&one, "\n\n");
			if (p->nr_tags) {
				size_t k;
				buf_addstr(&one, "tags:");
				for (k = 0; k < p->nr_tags; k++)
					buf_addf(&one, "%s %s", k ? "," : "",
						 p->tags[k]);
				buf_addstr(&one, "\n\n");
			}
			buf_addstr(&one, p->body ? p->body : "");
			if (p->outcome)
				buf_addf(&one, "\n**Outcome.** %s\n", p->outcome);
			buf_addch(&one, '\n');
		}

		write_file(path, one.b, one.len);
		buf_addf(&index, "%lu. [%s](%s) -- %lu prompt(s)\n",
			 (unsigned long)(i + 1),
			 grp->session && grp->session->title
				 ? grp->session->title : "unattributed",
			 name, (unsigned long)grp->prompts.nr);
		free(name);
		free(path);
	}

	{
		char *ip = xstrfmt("%s/README.md", dir);
		write_file(ip, index.b, index.len);
		free(ip);
	}

	buf_release(&index);
	buf_release(&one);
}
