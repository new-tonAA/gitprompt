/*
 * object.c - trees and commits, in git's exact wire format.
 *
 * A tree is a sequence of "<mode> <name>\0<20 raw bytes>", sorted by name
 * with directories ordered as though their name ended in '/'.  A commit is
 * a header block -- tree, parents, author, committer -- then a blank line,
 * then the message.  gitprompt adds two headers of its own -- gp-session,
 * the prompting session, and one gp-prompt per prompt the commit carries --
 * which git preserves untouched.
 */
#include "gp.h"

/* ------------------------------------------------------------------ */
/* trees                                                               */

void tree_release(struct tree *t)
{
	size_t i;
	for (i = 0; i < t->nr; i++)
		free(t->e[i].name);
	free(t->e);
	t->e = NULL;
	t->nr = t->alloc = 0;
}

void tree_append(struct tree *t, u32 mode, const oid_t *oid, const char *name)
{
	if (t->nr == t->alloc) {
		t->alloc = t->alloc ? t->alloc * 2 : 16;
		t->e = xrealloc(t->e, t->alloc * sizeof(*t->e));
	}
	t->e[t->nr].mode = mode;
	t->e[t->nr].oid = *oid;
	t->e[t->nr].name = xstrdup(name);
	t->nr++;
}

#define S_IS_TREE_MODE(m) (((m) & 0170000) == 0040000)

/*
 * git's tree ordering: byte order on the name, except that a directory is
 * compared as if its name had a trailing '/'.  That is what makes "a.txt"
 * sort before the directory "a" (compare 'a','.' vs 'a','/').
 */
static int tree_entry_cmp(const void *va, const void *vb)
{
	const struct tree_entry *a = va, *b = vb;
	const char *x = a->name, *y = b->name;

	for (;;) {
		int xc = (u8)*x, yc = (u8)*y;
		if (!xc && S_IS_TREE_MODE(a->mode))
			xc = '/';
		if (!yc && S_IS_TREE_MODE(b->mode))
			yc = '/';
		if (xc != yc)
			return xc - yc;
		if (!xc)
			return 0;
		x++;
		y++;
	}
}

void tree_sort_for_write(struct tree *t)
{
	if (t->nr > 1)
		qsort(t->e, t->nr, sizeof(*t->e), tree_entry_cmp);
}

void tree_parse(struct tree *t, const void *data, size_t len)
{
	const u8 *p = data, *end = p + len;

	while (p < end) {
		const u8 *sp = memchr(p, ' ', (size_t)(end - p));
		const u8 *nul;
		u32 mode = 0;
		const u8 *q;

		if (!sp)
			gp_die("corrupt tree: no space in mode");
		for (q = p; q < sp; q++) {
			if (*q < '0' || *q > '7')
				gp_die("corrupt tree: bad mode digit");
			mode = mode * 8 + (u32)(*q - '0');
		}
		nul = memchr(sp + 1, '\0', (size_t)(end - (sp + 1)));
		if (!nul)
			gp_die("corrupt tree: unterminated name");
		if (nul + 1 + GP_SHA1_RAWSZ > end)
			gp_die("corrupt tree: truncated entry");
		{
			char *name = xstrndup((const char *)sp + 1,
					      (size_t)(nul - (sp + 1)));
			oid_t oid;
			memcpy(oid.raw, nul + 1, GP_SHA1_RAWSZ);
			tree_append(t, mode, &oid, name);
			free(name);
		}
		p = nul + 1 + GP_SHA1_RAWSZ;
	}
}

void tree_format(const struct tree *t, struct buf *out)
{
	size_t i;

	buf_reset(out);
	for (i = 0; i < t->nr; i++) {
		buf_addf(out, "%o %s", t->e[i].mode, t->e[i].name);
		buf_addch(out, '\0');
		buf_add(out, t->e[i].oid.raw, GP_SHA1_RAWSZ);
	}
}

int tree_lookup(const struct tree *t, const char *name, u32 *mode, oid_t *oid,
		size_t *pos)
{
	size_t i;
	for (i = 0; i < t->nr; i++) {
		if (!strcmp(t->e[i].name, name)) {
			if (mode)
				*mode = t->e[i].mode;
			if (oid)
				*oid = t->e[i].oid;
			if (pos)
				*pos = i;
			return 1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* commits                                                             */

void commit_release(struct commit *c)
{
	oid_array_clear(&c->parents);
	free(c->author);
	free(c->committer);
	free(c->message);
	free(c->session);
	{
		size_t i;
		for (i = 0; i < c->nr_prompts; i++)
			free(c->prompts[i]);
	}
	free(c->prompts);
	c->author = c->committer = c->message = c->session = NULL;
	c->prompts = NULL;
	c->nr_prompts = 0;
}

void commit_parse(struct commit *c, const void *data, size_t len)
{
	const char *p = data, *end = p + len;

	commit_release(c);
	memset(&c->tree, 0, sizeof c->tree);

	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		const char *line_end = eol ? eol : end;

		if (line_end == p) {           /* blank line: message follows */
			p = eol ? eol + 1 : end;
			break;
		}
		{
			size_t llen = (size_t)(line_end - p);
			if (llen > 5 && !memcmp(p, "tree ", 5)) {
				char *hex = xstrndup(p + 5, llen - 5);
				if (oid_parse(&c->tree, hex) < 0)
					gp_die("corrupt commit: bad tree id");
				free(hex);
			} else if (llen > 7 && !memcmp(p, "parent ", 7)) {
				char *hex = xstrndup(p + 7, llen - 7);
				oid_t o;
				if (oid_parse(&o, hex) < 0)
					gp_die("corrupt commit: bad parent id");
				oid_array_append(&c->parents, &o);
				free(hex);
			} else if (llen > 7 && !memcmp(p, "author ", 7)) {
				free(c->author);
				c->author = xstrndup(p + 7, llen - 7);
			} else if (llen > 10 && !memcmp(p, "committer ", 10)) {
				free(c->committer);
				c->committer = xstrndup(p + 10, llen - 10);
			} else if (llen > 11 && !memcmp(p, "gp-session ", 11)) {
				free(c->session);
				c->session = xstrndup(p + 11, llen - 11);
			} else if (llen > 10 && !memcmp(p, "gp-prompt ", 10)) {
				c->prompts = xrealloc(c->prompts,
					(c->nr_prompts + 1) * sizeof(*c->prompts));
				c->prompts[c->nr_prompts++] =
					xstrndup(p + 10, llen - 10);
			}
		}
		p = eol ? eol + 1 : end;
	}
	free(c->message);
	c->message = xstrndup(p, (size_t)(end - p));
}

void commit_format(const struct commit *c, struct buf *out)
{
	char hex[GP_SHA1_HEXSZ + 1];
	size_t i;

	buf_reset(out);
	oid_hex(&c->tree, hex);
	buf_addf(out, "tree %s\n", hex);
	for (i = 0; i < c->parents.nr; i++) {
		oid_hex(&c->parents.oid[i], hex);
		buf_addf(out, "parent %s\n", hex);
	}
	buf_addf(out, "author %s\n", c->author ? c->author : "");
	buf_addf(out, "committer %s\n", c->committer ? c->committer : "");
	if (c->session && c->session[0])
		buf_addf(out, "gp-session %s\n", c->session);
	for (i = 0; i < c->nr_prompts; i++)
		if (c->prompts[i] && c->prompts[i][0])
			buf_addf(out, "gp-prompt %s\n", c->prompts[i]);
	buf_addch(out, '\n');
	if (c->message)
		buf_add(out, c->message, strlen(c->message));
}

/* ------------------------------------------------------------------ */

void read_commit(struct repo *r, const oid_t *oid, struct commit *c)
{
	struct buf b;
	enum obj_type t;

	buf_init(&b);
	if (odb_read(&r->odb, oid, &t, &b) < 0 || t != OBJ_COMMIT)
		gp_die("not a commit: %s", abbrev_oid(oid));
	commit_parse(c, b.b, b.len);
	buf_release(&b);
}

void read_tree_obj(struct repo *r, const oid_t *oid, struct tree *t)
{
	struct buf b;
	enum obj_type ty;

	buf_init(&b);
	if (odb_read(&r->odb, oid, &ty, &b) < 0 || ty != OBJ_TREE)
		gp_die("not a tree: %s", abbrev_oid(oid));
	tree_parse(t, b.b, b.len);
	buf_release(&b);
}

/* the first line of the message, for one-line log output */
char *commit_message_line(const struct commit *c)
{
	const char *nl;
	size_t n;
	if (!c->message || !c->message[0])
		return xstrdup("");
	nl = strchr(c->message, '\n');
	n = nl ? (size_t)(nl - c->message) : strlen(c->message);
	return xstrndup(c->message, n);
}

/*
 * The committer timestamp.  The ident line ends in "<epoch> <tz>"; if it
 * cannot be read, fall back to the author line and finally to 0, so a
 * hand-written commit never makes log crash.
 */
i64 commit_time(const struct commit *c)
{
	const char *line = c->committer ? c->committer : c->author;
	const char *p;

	if (!line)
		return 0;
	p = line + strlen(line);
	/* walk back over "<+hhmm>" then whitespace then the digits */
	while (p > line && p[-1] != ' ')
		p--;
	if (p > line) {
		const char *q = p - 1;
		while (q > line && q[-1] != ' ')
			q--;
		return parse_timestamp(q);
	}
	return 0;
}

/*
 * Split an identity line -- "Name <email> <epoch> <tzzone>" -- into its name
 * and its email.  Either out pointer may be NULL, and either result is left
 * NULL when that part is not in the line.  Both are freshly allocated, and the
 * whitespace before "<" is not part of the name, so a hand-written ident does
 * not carry it into a group heading.  This is what `shortlog` groups on and
 * what `format_author_line` renders, so the two cannot disagree about where a
 * name ends.
 */
void parse_ident(const char *raw, char **name, char **email)
{
	const char *lt, *gt;

	if (name)
		*name = NULL;
	if (email)
		*email = NULL;
	if (!raw)
		return;

	lt = strchr(raw, '<');
	gt = lt ? strchr(lt, '>') : NULL;
	if (lt && gt) {
		size_t n = (size_t)(lt - raw);

		if (email)
			*email = xstrndup(lt + 1, (size_t)(gt - lt) - 1);
		while (n && raw[n - 1] == ' ')
			n--;
		if (name)
			*name = xstrndup(raw, n);
	} else if (name) {
		*name = xstrdup(raw);
	}
}
