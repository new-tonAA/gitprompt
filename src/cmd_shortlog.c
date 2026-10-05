/*
 * shortlog -- the log, summarised by author.
 *
 * git's shortlog walks a revision list, groups the commits by the name on
 * their author line, and prints the subjects under each name.  This is that
 * command, with the same three shapes: the blocks (the default), the counts
 * (-s), and the email appended to the name (-e), ordered by name, or by the
 * number of commits with -n.
 *
 * The grouping key is the name alone unless -e is given, so two commits made
 * by the same person under different addresses are one group by default and
 * two groups with -e -- which is exactly how git tells a contributor who has
 * committed from two machines.  The block form sorts the subjects oldest
 * first, because git reverses the walk it read; the other shapes have nothing
 * to order inside a group.
 */
#include "gp.h"
#include <stdlib.h>
#include <string.h>

struct sl_author {
	char *name;         /* what the group is called, email included with -e */
	size_t nr;
	char **subjects;    /* in walk order, which is newest first */
	size_t nr_subjects, cap_subjects;
};

struct sl_ctx {
	int numbered;
	int email;
	int summary;
	struct sl_author *authors;
	size_t nr, cap;
};

static struct sl_author *sl_find(struct sl_ctx *c, const char *name)
{
	size_t i;

	for (i = 0; i < c->nr; i++)
		if (!strcmp(c->authors[i].name, name))
			return &c->authors[i];

	if (c->nr == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 8;
		c->authors = xrealloc(c->authors, c->cap * sizeof *c->authors);
	}
	memset(&c->authors[c->nr], 0, sizeof c->authors[c->nr]);
	c->authors[c->nr].name = xstrdup(name);
	return &c->authors[c->nr++];
}

static void sl_add_subject(struct sl_author *a, const char *subject)
{
	if (a->nr_subjects == a->cap_subjects) {
		a->cap_subjects = a->cap_subjects ? a->cap_subjects * 2 : 4;
		a->subjects = xrealloc(a->subjects,
				       a->cap_subjects * sizeof *a->subjects);
	}
	a->subjects[a->nr_subjects++] = xstrdup(subject);
}

static void sl_one(const oid_t *oid, const struct commit *commit, void *ud)
{
	struct sl_ctx *c = ud;
	struct sl_author *a;
	char *name, *email, *key, *subject;

	(void)oid;
	parse_ident(commit->author, &name, &email);
	if (!name)
		name = xstrdup("(no author)");

	if (c->email)
		key = email ? xstrfmt("%s <%s>", name, email) : xstrdup(name);
	else
		key = xstrdup(name);

	a = sl_find(c, key);
	a->nr++;

	subject = commit_message_line(commit);
	sl_add_subject(a, subject);

	free(subject);
	free(key);
	free(name);
	free(email);
}

/* b belongs before a?  by count when -n, by name otherwise */
static int sl_before(const struct sl_ctx *c, const struct sl_author *a,
		     const struct sl_author *b)
{
	if (c->numbered && a->nr != b->nr)
		return a->nr > b->nr;
	return strcmp(a->name, b->name) < 0;
}

static void sl_sort(struct sl_ctx *c)
{
	size_t i;

	for (i = 1; i < c->nr; i++) {
		struct sl_author key = c->authors[i];
		size_t j = i;

		while (j > 0 && sl_before(c, &key, &c->authors[j - 1])) {
			c->authors[j] = c->authors[j - 1];
			j--;
		}
		c->authors[j] = key;
	}
}

static void sl_print(const struct sl_ctx *c)
{
	size_t i, j;

	for (i = 0; i < c->nr; i++) {
		const struct sl_author *a = &c->authors[i];

		if (c->summary) {
			printf("%6d\t%s\n", (int)a->nr, a->name);
			continue;
		}
		printf("%s (%d):\n", a->name, (int)a->nr);
		for (j = a->nr_subjects; j > 0; j--)
			printf("      %s\n", a->subjects[j - 1]);
		printf("\n");
	}
}

static void sl_release(struct sl_ctx *c)
{
	size_t i, j;

	for (i = 0; i < c->nr; i++) {
		for (j = 0; j < c->authors[i].nr_subjects; j++)
			free(c->authors[i].subjects[j]);
		free(c->authors[i].subjects);
		free(c->authors[i].name);
	}
	free(c->authors);
}

int cmd_shortlog(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct rev_list rl;
	struct sl_ctx c;
	char *def[2];

	opts_init(&o, argc, argv, (const char *const[]){
		"-s", "--summary", "-n", "--numbered", "-e", "--email", NULL });

	memset(&c, 0, sizeof c);
	c.numbered = opts_flag(&o, "-n") || opts_flag(&o, "--numbered");
	c.email = opts_flag(&o, "-e") || opts_flag(&o, "--email");
	c.summary = opts_flag(&o, "-s") || opts_flag(&o, "--summary");

	if (o.nargs == 0) {
		oid_t start;

		if (refs_head(&r->refs, &start) < 0) {
			char *t = refs_head_target(&r->refs);
			const char *shown = (t && !strncmp(t, "refs/heads/", 11))
						    ? t + 11 : "main";

			gp_die("your current branch '%s' does not have any commits yet",
			       shown);
		}
		def[0] = (char *)"HEAD";
		def[1] = NULL;
		if (rev_list_parse(r, 1, def, &rl) < 0)
			return 128;
	} else if (rev_list_parse(r, o.nargs, o.args, &rl) < 0) {
		return 128;
	}

	rev_list_run(r, &rl, sl_one, &c);
	rev_list_release(&rl);

	sl_sort(&c);
	sl_print(&c);
	sl_release(&c);
	return 0;
}
