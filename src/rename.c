/*
 * rename.c - deciding that a path one side no longer has and a path the other
 * side has gained are one file that moved.
 *
 * A merge that reads a rename as a delete and an unrelated add will not follow
 * an edit made on the far side of it: the file we renamed is "deleted" as far
 * as the other side's change is concerned, and its contents are unrelated to
 * the add that appears beside it.  So this recognises the move before the
 * merge runs, and the merge then treats it as the same file under a new name.
 *
 * Two paths are the same file when they hold the same object -- no contents to
 * read, just the ids -- and, failing that, when enough of their text is still
 * the same, which is what catches a file that was moved *and* edited.  git
 * calls the second half inexact rename detection and scores it the same way;
 * the line between the two judgements is also git's, half the text in common.
 *
 * The pairing is one to one -- a source and a target each used once -- so two
 * copies of one file are not read as two renames of the original, and a file
 * renamed onto another is one rename rather than two.
 */
#include "gp.h"

/*
 * A moved file with half its lines still there is the same file.  git's
 * default rename threshold is the same number, arrived at the same way, and a
 * score below it is the unrelated add the file would otherwise be.
 */
#define RENAME_THRESHOLD 50

/*
 * How many candidate pairs are scored before the search gives up, git's
 * diff.renameLimit again.  The cost is the product -- every deleted path
 * against every added one -- and a merge that moved hundreds of files on both
 * sides can reach it, so past this the contents are not read and a rename that
 * was also edited comes through as a delete and an add, as it did before.
 */
#define RENAME_CANDIDATES 1000

/*
 * Only regular files.  A symlink's or a gitlink's object is not a file whose
 * bytes moved, and pairing on it would report a mode change as a rename.
 */
static int is_blob_path(const struct index_entry *e)
{
	return (e->mode & 0170000) == 0100000;
}

static int target_is_taken(const struct rename_list *rl, const char *to)
{
	size_t i;

	for (i = 0; i < rl->nr; i++)
		if (!strcmp(rl->e[i].to, to))
			return 1;
	return 0;
}

const struct rename_pair *rename_by_from(const struct rename_list *rl,
					 const char *from)
{
	size_t i;

	for (i = 0; i < rl->nr; i++)
		if (!strcmp(rl->e[i].from, from))
			return &rl->e[i];
	return NULL;
}

const struct rename_pair *rename_by_to(const struct rename_list *rl,
				       const char *to)
{
	size_t i;

	for (i = 0; i < rl->nr; i++)
		if (!strcmp(rl->e[i].to, to))
			return &rl->e[i];
	return NULL;
}

static void rename_add(struct rename_list *rl, const char *from,
		       const char *to)
{
	if (rl->nr == rl->alloc) {
		rl->alloc = rl->alloc ? rl->alloc * 2 : 8;
		rl->e = xrealloc(rl->e, rl->alloc * sizeof(*rl->e));
	}
	rl->e[rl->nr].from = xstrdup(from);
	rl->e[rl->nr].to = xstrdup(to);
	rl->nr++;
}

/*
 * The renames that turned `base` into `side`: a path `base` has and `side`
 * does not, paired with a path `side` has and `base` does not holding the same
 * file.  A path in both was not moved, and neither was one whose contents
 * survived on neither side.
 */
void renames_between(struct odb *odb, struct index_state *base,
		     struct index_state *side, struct rename_list *out)
{
	struct index_entry **del = NULL, **add = NULL;
	struct buf *add_text = NULL;
	size_t ndel = 0, nadd = 0, i, j;

	memset(out, 0, sizeof *out);

	/* the cheap half: the same object under the two names */
	for (i = 0; i < base->nr; i++) {
		const struct index_entry *b = &base->e[i];

		if (!is_blob_path(b) || index_get(side, b->path))
			continue;
		for (j = 0; j < side->nr; j++) {
			const struct index_entry *s = &side->e[j];

			if (!is_blob_path(s) || index_get(base, s->path))
				continue;
			if (!oid_equal(&b->oid, &s->oid))
				continue;
			if (target_is_taken(out, s->path))
				continue;
			rename_add(out, b->path, s->path);
			break;
		}
	}

	/*
	 * What that left: a path that moved and was edited as well, which only
	 * a look at the contents can recognise.  Both sides' leftovers are
	 * collected first, since every pair of them is a candidate and the
	 * count is what the search is bounded by.
	 */
	del = xmalloc(base->nr ? base->nr * sizeof(*del) : 1);
	add = xmalloc(side->nr ? side->nr * sizeof(*add) : 1);
	for (i = 0; i < base->nr; i++)
		if (is_blob_path(&base->e[i]) && !index_get(side, base->e[i].path)
		    && !rename_by_from(out, base->e[i].path))
			del[ndel++] = &base->e[i];
	for (j = 0; j < side->nr; j++)
		if (is_blob_path(&side->e[j]) && !index_get(base, side->e[j].path)
		    && !target_is_taken(out, side->e[j].path))
			add[nadd++] = &side->e[j];

	if (!ndel || !nadd)
		goto out;
	if ((double)ndel * (double)nadd > RENAME_CANDIDATES) {
		fprintf(stderr, "warning: %lu paths removed and %lu added is more "
			"than rename detection will look through\n"
			"hint: a moved file that was also edited is read as a "
			"delete and an add, not followed\n",
			(unsigned long)ndel, (unsigned long)nadd);
		goto out;
	}

	add_text = xcalloc(nadd, sizeof(*add_text));
	for (i = 0; i < ndel; i++) {
		struct buf del_text;
		size_t best = nadd;
		int best_score = 0;

		buf_init(&del_text);
		if (odb_read(odb, &del[i]->oid, NULL, &del_text) < 0)
			continue;
		for (j = 0; j < nadd; j++) {
			int score;

			if (target_is_taken(out, add[j]->path))
				continue;
			if (!add_text[j].b && !add_text[j].len &&
			    odb_read(odb, &add[j]->oid, NULL, &add_text[j]) < 0)
				continue;
			score = merge3_similarity(&del_text, &add_text[j]);
			if (score > best_score) {
				best_score = score;
				best = j;
			}
		}
		if (best_score >= RENAME_THRESHOLD)
			rename_add(out, del[i]->path, add[best]->path);
		buf_release(&del_text);
	}
	for (j = 0; j < nadd; j++)
		buf_release(&add_text[j]);

out:
	free(add_text);
	free(add);
	free(del);
}

void rename_list_release(struct rename_list *rl)
{
	size_t i;

	for (i = 0; i < rl->nr; i++) {
		free(rl->e[i].from);
		free(rl->e[i].to);
	}
	free(rl->e);
	memset(rl, 0, sizeof *rl);
}
