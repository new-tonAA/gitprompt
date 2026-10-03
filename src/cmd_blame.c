/*
 * blame.c - which prompt asked for which line.
 *
 * Blame is a diff read backwards: instead of asking what a commit changed, it
 * asks who is answerable for a line that is there now.  That answer is not
 * written on the line -- code does not carry a note saying which prompt wanted
 * it -- so it is recovered in two steps.
 *
 * The first step is ordinary blame.  Walk the file back through the commits,
 * comparing each version with its first parent's: a line that is new in a
 * commit is that commit's doing, and a line that carried through belongs to
 * whichever ancestor it came from.  Every line of the blamed version lands on
 * exactly one commit, and the walk ends at the root.
 *
 * The second step lands it on a prompt.  Inside a commit the prompts form a
 * chain of snapshots (see trace.c), and the prompt whose step introduced the
 * line is the one to name.  A commit that has no chain -- its prompts were
 * recorded before snapshots were kept, or its snapshots are gone -- can only
 * offer the prompts it carries as a whole, and those lines are marked with a
 * `?` so that "this prompt" stays distinguishable from "one of this commit's
 * prompts".  A line no prompt claims at all shows `-`: it was already in the
 * tree when the first prompt was recorded, or the commit carries no prompt.
 */
#include "gp.h"

struct blame_line {
	char *prompt;
	oid_t commit;
	int fallback;   /* the prompt is the commit's, not the block's */
	int done;
};

/* the file at a state, or 0 when that state does not hold it */
static int file_at(struct repo *r, const oid_t *tree, const char *path,
		   struct buf *out)
{
	struct index_state ist;
	struct index_entry *e;
	int found = 0;

	buf_reset(out);
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

/*
 * One line of the blamed version, answered.  `carry` maps a line of the
 * version being compared to the line of the blamed version it came from, so a
 * line the walk meets deep in the history still knows which line of the output
 * it is.  A line already answered is left alone: an older commit cannot take
 * back what a newer one did.
 */
static void attribute(struct blame_line *att, const size_t *carry, size_t k,
		      const char *prompt, int fallback, const oid_t *commit_oid)
{
	size_t idx = carry[k];

	if (idx == (size_t)-1 || att[idx].done)
		return;
	att[idx].done = 1;
	att[idx].prompt = prompt ? xstrdup(prompt) : NULL;
	att[idx].fallback = fallback;
	att[idx].commit = *commit_oid;
}

/* every line still unanswered is this commit's doing */
static void attribute_rest(struct blame_line *att, const size_t *carry,
			   size_t ncarry, const char **lines, size_t nlines,
			   const struct commit *c, const oid_t *commit_oid)
{
	const char *whole = (c->nr_prompts && c->prompts[0]) ? c->prompts[0] : NULL;
	size_t k;

	for (k = 0; k < ncarry; k++) {
		const char *one = NULL;
		int fallback = 0;

		if (lines) {
			one = k < nlines ? lines[k] : NULL;
		} else {
			/* no chain: the commit's prompts, as a whole */
			one = whole;
			fallback = whole ? 1 : 0;
		}
		attribute(att, carry, k, one, fallback, commit_oid);
	}
}

int cmd_blame(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *rev, *path;
	struct buf norm, cur, revtext;
	struct commit c = COMMIT_INIT;
	struct dline *rd = NULL;
	struct blame_line *att;
	size_t nrev = 0, i;
	oid_t oid;
	enum obj_type t;

	opts_init(&o, argc, argv, (const char *const[]){ NULL });
	if (opts_count(&o) < 1 || opts_count(&o) > 2) {
		gp_error("blame: expected [<rev>] [--] <file>");
		return 1;
	}
	if (opts_count(&o) == 2) {
		rev = opts_arg(&o, 0);
		path = opts_arg(&o, 1);
	} else {
		rev = "HEAD";
		path = opts_arg(&o, 0);
	}

	buf_init(&norm);
	path_normalize(path, &norm);
	if (!norm.len) {
		gp_error("blame: expected a file to blame");
		buf_release(&norm);
		return 1;
	}

	if (resolve_rev(r, rev, &oid) < 0)
		gp_die("blame: unknown revision: %s", rev);
	if (odb_type_of(&r->odb, &oid, &t) < 0 || t != OBJ_COMMIT)
		gp_die("blame: %s does not name a commit", rev);

	buf_init(&cur);
	read_commit(r, &oid, &c);
	if (!file_at(r, &c.tree, buf_cstr(&norm), &cur))
		gp_die("blame: %s does not exist in %s", buf_cstr(&norm), rev);

	/* the walk replaces `cur` as it goes back, so the blamed version is
	 * kept apart from it */
	buf_init(&revtext);
	buf_add(&revtext, cur.b, cur.len);

	diff_split_lines(revtext.b ? revtext.b : "", revtext.len, &rd, &nrev);
	att = xcalloc(nrev + 1, sizeof(*att));

	{
		/* how many lines the version being compared has, and which line
		 * of the blamed version each of them came from */
		size_t ncarry = nrev;
		size_t *carry = xmalloc((nrev + 1) * sizeof(*carry));

		for (i = 0; i < nrev; i++)
			carry[i] = i;

		for (;;) {
			struct trace tr;
			const char **lines = NULL;
			size_t nlines = 0;

			trace_of_commit(r, &c, &tr);
			if (tr.nr)
				trace_file_prompts(r, &tr, buf_cstr(&norm), &lines,
						   &nlines);

			if (!c.parents.nr) {
				attribute_rest(att, carry, ncarry, lines, nlines,
					       &c, &oid);
				free(lines);
				trace_release(&tr);
				break;
			}

			{
				struct commit parent = COMMIT_INIT;
				struct buf par;
				struct dline *lc = NULL, *lp = NULL;
				struct oline *ol = NULL;
				struct stat_counts counts;
				size_t nc = 0, np = 0, no = 0, k;
				size_t *next;
				oid_t parent_oid = c.parents.oid[0];

				buf_init(&par);
				read_commit(r, &parent_oid, &parent);

				if (!file_at(r, &parent.tree, buf_cstr(&norm),
					     &par)) {
					/* the file begins here, so every line
					 * of it begins here too */
					attribute_rest(att, carry, ncarry, lines,
						       nlines, &c, &oid);
					free(lines);
					trace_release(&tr);
					commit_release(&parent);
					buf_release(&par);
					break;
				}

				diff_split_lines(cur.b ? cur.b : "", cur.len,
						 &lc, &nc);
				diff_split_lines(par.b ? par.b : "", par.len,
						 &lp, &np);
				lcs_diff(lp, np, lc, nc, &ol, &no, &counts);

				next = xmalloc((np + 1) * sizeof(*next));
				for (k = 0; k < np; k++)
					next[k] = (size_t)-1;

				for (k = 0; k < no; k++) {
					if (ol[k].op == ' ') {
						if (ol[k].a < np && ol[k].b < nc)
							next[ol[k].a] =
								carry[ol[k].b];
					} else if (ol[k].op == '+') {
						/* new here: this commit's line */
						const char *one = NULL;
						int fallback = 0;

						if (lines) {
							one = ol[k].b < nlines
							      ? lines[ol[k].b]
							      : NULL;
						} else if (c.nr_prompts &&
							   c.prompts[0]) {
							one = c.prompts[0];
							fallback = 1;
						}
						if (ol[k].b < nc)
							attribute(att, carry,
								  ol[k].b, one,
								  fallback, &oid);
					}
				}

				free(lines);
				trace_release(&tr);
				free(lc);
				free(lp);
				free(ol);
				free(carry);
				carry = next;

				buf_release(&cur);
				cur = par;
				buf_init(&par);

				commit_release(&c);
				c = parent;
				oid = parent_oid;

				/* the version being compared is now the
				 * parent's, so that is what `carry` maps from */
				ncarry = np;
			}
		}

		free(carry);
	}

	{
		long w = 1, m = (long)nrev;

		while (m >= 10) {
			m /= 10;
			w++;
		}
		for (i = 0; i < nrev; i++) {
			struct blame_line *b = &att[i];
			char col[GP_SHA1_HEXSZ + 4];
			char *ab = abbrev_oid(&b->commit);
			size_t l = rd[i].len;

			if (b->prompt)
				snprintf(col, sizeof col, "%s%s", b->prompt,
					 b->fallback ? "?" : "");
			else
				snprintf(col, sizeof col, "-");
			if (l && rd[i].p[l - 1] == '\n')
				l--;
			printf("%-11s %-7s %*ld) %.*s\n", col, ab, (int)w,
			       (long)(i + 1), (int)l, rd[i].p);
			free(ab);
			free(b->prompt);
		}
	}

	free(att);
	free(rd);
	buf_release(&revtext);
	buf_release(&cur);
	buf_release(&norm);
	return 0;
}
