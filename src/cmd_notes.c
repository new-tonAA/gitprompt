/*
 * notes -- text kept about an object, beside it.
 *
 * A note never touches the object it is about: it is a blob of its own, and
 * the tree that holds it names it after the object's id, so a note is found by
 * hashing the object rather than by scanning.  git fans those names out into a
 * directory of the first two hex digits once a tree has grown large; both
 * shapes are read here, and the flat one is written, since that is the one git
 * writes for a repository of any ordinary size.
 *
 * The tree does not hang off a ref on its own, so it hangs off a commit on
 * refs/notes/commits, exactly as git writes it: that is what makes a note
 * written here readable by git, and git's readable here.  The commit is never
 * checked out and never merged; it exists to give the tree a ref.
 *
 * Removing the last note leaves the ref in place over an empty tree, which is
 * what git does with it too -- the ref going away would be a different thing,
 * and one that reading it back would have to guess at.
 */
#include "gp.h"
#include <stdlib.h>
#include <string.h>

#define NOTES_REF "refs/notes/commits"

/*
 * Where a note sits in the tree.  git writes the object's name in full at the
 * top level and only fans it out into a directory of the first two hex digits
 * once a tree has grown past a couple of hundred notes; both shapes are read
 * back, and this writes the flat one, which is what git writes for a
 * repository of any ordinary size.
 */
static void notes_path(const oid_t *oid, char out[GP_SHA1_HEXSZ + 1])
{
	oid_hex(oid, out);
}

/* the object a name in a notes tree is about, or -1 if it is not one */
static int notes_object(const char *name, oid_t *out)
{
	char hex[GP_SHA1_HEXSZ + 1];
	size_t len = strlen(name);

	if (len == GP_SHA1_HEXSZ) {
		memcpy(hex, name, GP_SHA1_HEXSZ + 1);
		return oid_parse(out, hex);
	}
	if (len == GP_SHA1_HEXSZ + 1 && name[2] == '/') {
		hex[0] = name[0];
		hex[1] = name[1];
		memcpy(hex + 2, name + 3, GP_SHA1_HEXSZ - 2);
		hex[GP_SHA1_HEXSZ] = '\0';
		return oid_parse(out, hex);
	}
	return -1;
}

/*
 * The tree the notes ref points at.  The ref names a commit, and peeling that
 * to a tree is not what commit_peel does -- it unwraps tags and no more -- so
 * the commit is read and its tree taken.
 */
static int notes_tree(struct repo *r, const oid_t *ref, oid_t *out)
{
	oid_t oid;
	struct commit c = COMMIT_INIT;
	enum obj_type t;

	if (commit_peel(r, ref, OBJ_NONE, &oid) < 0)
		return -1;
	if (odb_read(&r->odb, &oid, &t, NULL) < 0)
		return -1;
	if (t == OBJ_TREE) {
		*out = oid;
		return 0;
	}
	if (t != OBJ_COMMIT)
		return -1;
	read_commit(r, &oid, &c);
	*out = c.tree;
	commit_release(&c);
	return 0;
}

/* the notes tree, as an index; empty when the ref is not there */
static void notes_load(struct repo *r, struct index_state *ist)
{
	oid_t ref, tree;

	index_clear(ist);
	if (refs_read(&r->refs, NOTES_REF, &ref) < 0)
		return;
	if (notes_tree(r, &ref, &tree) < 0)
		return;
	read_tree_into_index(r, ist, &tree, NULL);
}

/*
 * Write the index back as a notes commit and move the ref onto it.  The whole
 * tree is rebuilt, which is what git does too: a note is a leaf and the tree
 * above it is cheap to rewrite.
 */
static int notes_store(struct repo *r, struct index_state *ist,
		       const char *message)
{
	oid_t tree, commit, prev;
	struct commit c = COMMIT_INIT;
	struct buf b = BUF_INIT, ident = BUF_INIT;
	int had = refs_read(&r->refs, NOTES_REF, &prev) == 0;
	int rc;

	if (write_tree_from_index(r, ist, &tree) < 0)
		return -1;

	repo_ident_with_time(r, &ident);
	c.tree = tree;
	/* separate allocations: commit_release frees each in turn */
	c.author = xstrdup(buf_cstr(&ident));
	c.committer = xstrdup(buf_cstr(&ident));
	c.message = xstrdup(message);
	if (had)
		oid_array_append(&c.parents, &prev);

	buf_init(&b);
	commit_format(&c, &b);
	rc = odb_write(&r->odb, OBJ_COMMIT, b.b, b.len, &commit);
	if (rc == 0)
		rc = refs_write(&r->refs, NOTES_REF, &commit);

	buf_release(&b);
	buf_release(&ident);
	commit_release(&c);
	return rc;
}

/* the two shapes a note's name can take: flat, and the fanout git uses */
static void notes_fanout(const char *flat, char out[GP_SHA1_HEXSZ + 2])
{
	out[0] = flat[0];
	out[1] = flat[1];
	out[2] = '/';
	memcpy(out + 3, flat + 2, GP_SHA1_HEXSZ - 2);
	out[GP_SHA1_HEXSZ + 1] = '\0';
}

/*
 * The entry holding the note about `obj`, or NULL.  Both shapes are looked
 * for: which one a tree holds is git's business, not this command's.
 */
static struct index_entry *notes_entry(struct index_state *ist, const oid_t *obj)
{
	char flat[GP_SHA1_HEXSZ + 1], fan[GP_SHA1_HEXSZ + 2];
	struct index_entry *e;

	notes_path(obj, flat);
	e = index_get(ist, flat);
	if (e)
		return e;
	notes_fanout(flat, fan);
	return index_get(ist, fan);
}

/*
 * Record a note, under the flat name.  Whatever was there goes first: a note
 * that came in fanned out must not be left behind beside the new one.
 */
static void notes_put_entry(struct index_state *ist, const oid_t *obj,
			    const oid_t *blob)
{
	struct index_entry e;
	char flat[GP_SHA1_HEXSZ + 1], fan[GP_SHA1_HEXSZ + 2];

	notes_path(obj, flat);
	notes_fanout(flat, fan);
	index_remove(ist, flat);
	index_remove(ist, fan);

	memset(&e, 0, sizeof e);
	e.path = flat;          /* index_add takes a copy */
	e.mode = MODE_BLOB;
	e.oid = *blob;
	e.stage = 0;
	index_add(ist, &e);
}

static void notes_del_entry(struct index_state *ist, const oid_t *obj)
{
	char flat[GP_SHA1_HEXSZ + 1], fan[GP_SHA1_HEXSZ + 2];

	notes_path(obj, flat);
	notes_fanout(flat, fan);
	index_remove(ist, flat);
	index_remove(ist, fan);
}

/* the object a subcommand is about; HEAD when nothing is named */
static int notes_target(struct repo *r, const char *rev, oid_t *out)
{
	if (!rev)
		rev = "HEAD";
	/* a name in full is taken as it stands, so a note about an object the
	 * store no longer holds can still be read, listed and removed -- git
	 * resolves one the same way, without asking whether it is there */
	if (strlen(rev) == GP_SHA1_HEXSZ && oid_parse(out, rev) == 0)
		return 0;
	if (resolve_rev(r, rev, out) < 0) {
		gp_error("failed to resolve '%s' as a valid ref", rev);
		return -1;
	}
	return 0;
}

static int notes_no_options(struct opts *o, const char *sub)
{
	if (o->nf == 0)
		return 1;
	gp_error("'notes %s' takes no options", sub);
	return 0;
}

/* a note ends in exactly one newline, with no trailing blanks before it */
static void notes_trim(struct buf *b)
{
	while (b->len && (b->b[b->len - 1] == '\n' || b->b[b->len - 1] == ' ' ||
			  b->b[b->len - 1] == '\t' || b->b[b->len - 1] == '\r'))
		b->len--;
	if (b->len)
		buf_addch(b, '\n');
}

/* git's editor convention: a line whose first character is '#' is a comment */
static void notes_strip(struct buf *b)
{
	struct buf out = BUF_INIT;
	size_t i = 0;

	while (i < b->len) {
		size_t start = i, end;

		while (i < b->len && b->b[i] != '\n')
			i++;
		end = i;
		if (i < b->len)
			i++;

		if (end > start && b->b[start] == '#')
			continue;
		buf_add(&out, b->b + start, end - start);
		buf_addch(&out, '\n');
	}

	while (out.len && (out.b[out.len - 1] == '\n' || out.b[out.len - 1] == ' ' ||
			   out.b[out.len - 1] == '\t' || out.b[out.len - 1] == '\r'))
		out.len--;
	buf_reset(b);
	if (out.len)
		buf_addf(b, "%s\n", buf_cstr(&out));
	buf_release(&out);
}

/* ------------------------------------------------------------------ */

static int notes_list(struct repo *r)
{
	struct index_state ist = INDEX_INIT;
	size_t i;

	notes_load(r, &ist);
	for (i = 0; i < ist.nr; i++) {
		oid_t obj;
		char hex[GP_SHA1_HEXSZ + 1];

		if (notes_object(ist.e[i].path, &obj) < 0)
			continue;
		oid_hex(&ist.e[i].oid, hex);
		printf("%s ", hex);
		oid_hex(&obj, hex);
		printf("%s\n", hex);
	}
	index_release(&ist);
	return 0;
}

static int notes_add(struct repo *r, struct opts *o, const char *rev)
{
	struct index_state ist = INDEX_INIT;
	struct index_entry *old;
	oid_t obj, blob;
	struct buf msg = BUF_INIT;
	int force = opts_flag(o, "-f") || opts_flag(o, "--force");
	const char *m = opts_value(o, "-m");
	const char *f = opts_value(o, "-F");
	int rc = 1;

	if (m) {
		buf_addstr(&msg, m);
	} else if (f) {
		if (read_file(f, &msg) < 0) {
			gp_error("could not read '%s'", f);
			goto out;
		}
	} else {
		gp_error("notes add: a message is required\n"
			 "hint: pass -m \"...\" or -F <file>");
		goto out;
	}
	notes_trim(&msg);
	if (!msg.len) {
		gp_error("notes add: refusing to save an empty note");
		goto out;
	}

	if (notes_target(r, rev, &obj) < 0) {
		rc = 128;
		goto out;
	}

	notes_load(r, &ist);
	old = notes_entry(&ist, &obj);
	if (old && !force) {
		char hex[GP_SHA1_HEXSZ + 1];

		oid_hex(&obj, hex);
		gp_error("Cannot add notes. Found existing notes for object %s. "
			 "Use '-f' to overwrite existing notes", hex);
		goto out;
	}

	if (odb_write(&r->odb, OBJ_BLOB, msg.b, msg.len, &blob) < 0) {
		gp_error("cannot write the note");
		goto out;
	}
	notes_put_entry(&ist, &obj, &blob);
	if (notes_store(r, &ist, "Notes added by 'gitprompt notes add'\n") < 0) {
		gp_error("cannot store the note");
		goto out;
	}
	rc = 0;
out:
	buf_release(&msg);
	index_release(&ist);
	return rc;
}

static int notes_show(struct repo *r, const char *rev)
{
	struct index_state ist = INDEX_INIT;
	struct index_entry *e;
	oid_t obj;
	struct buf b = BUF_INIT;
	enum obj_type t;
	int rc = 1;

	if (notes_target(r, rev, &obj) < 0)
		return 128;

	notes_load(r, &ist);
	e = notes_entry(&ist, &obj);
	if (!e) {
		char hex[GP_SHA1_HEXSZ + 1];

		oid_hex(&obj, hex);
		gp_error("no note found for object %s.", hex);
		goto out;
	}
	if (odb_read(&r->odb, &e->oid, &t, &b) < 0) {
		gp_error("the note object is missing");
		goto out;
	}
	fwrite(b.b, 1, b.len, stdout);
	rc = 0;
out:
	buf_release(&b);
	index_release(&ist);
	return rc;
}

static int notes_remove(struct repo *r, const char *rev)
{
	struct index_state ist = INDEX_INIT;
	oid_t obj;
	int rc = 1;

	if (notes_target(r, rev, &obj) < 0)
		return 128;

	notes_load(r, &ist);
	if (!notes_entry(&ist, &obj)) {
		gp_error("Object %s has no note", rev ? rev : "HEAD");
		goto out;
	}
	notes_del_entry(&ist, &obj);
	if (notes_store(r, &ist, "Notes removed by 'gitprompt notes remove'\n") < 0) {
		gp_error("cannot store the note");
		goto out;
	}
	printf("Removing note for object %s\n", rev ? rev : "HEAD");
	rc = 0;
out:
	index_release(&ist);
	return rc;
}

static int notes_prune(struct repo *r)
{
	struct index_state ist = INDEX_INIT;
	char **gone = NULL;
	size_t nr_gone = 0, cap = 0, i;

	notes_load(r, &ist);
	for (i = 0; i < ist.nr; i++) {
		oid_t obj;

		if (notes_object(ist.e[i].path, &obj) < 0)
			continue;
		if (odb_exists(&r->odb, &obj))
			continue;
		if (nr_gone == cap) {
			cap = cap ? cap * 2 : 8;
			gone = xrealloc(gone, cap * sizeof *gone);
		}
		gone[nr_gone++] = xstrdup(ist.e[i].path);
	}
	/* the removals wait until the scan is over: index_remove shifts the array */
	for (i = 0; i < nr_gone; i++) {
		index_remove(&ist, gone[i]);
		free(gone[i]);
	}
	free(gone);

	if (nr_gone &&
	    notes_store(r, &ist, "Notes pruned by 'gitprompt notes prune'\n") < 0) {
		gp_error("cannot store the note");
		index_release(&ist);
		return 1;
	}
	index_release(&ist);
	return 0;
}

static int notes_edit(struct repo *r, const char *rev)
{
	struct index_state ist = INDEX_INIT;
	struct index_entry *e;
	oid_t obj, blob;
	struct buf cur = BUF_INIT, b = BUF_INIT;
	char *editor = NULL, *file = NULL;
	int rc = 1;

	if (notes_target(r, rev, &obj) < 0)
		return 128;

	notes_load(r, &ist);
	e = notes_entry(&ist, &obj);
	if (e) {
		enum obj_type t;

		if (odb_read(&r->odb, &e->oid, &t, &cur) < 0) {
			gp_error("the note object is missing");
			goto out;
		}
	}

	editor = repo_editor_command(r);
	if (!editor) {
		gp_error("notes edit: no editor is configured\n"
			 "hint: set GIT_EDITOR or EDITOR");
		goto out;
	}
	file = repo_worktree_path(r, "NOTES_EDITMSG");
	if (write_file(file, cur.b ? (const void *)cur.b : "", cur.len) < 0) {
		gp_error("notes edit: cannot write %s", file);
		goto out;
	}
	if (repo_run_editor(editor, file) < 0) {
		gp_error("notes edit: the editor exited with an error; "
			 "nothing was saved");
		goto out;
	}
	if (read_file(file, &b) < 0) {
		gp_error("notes edit: cannot read back %s", file);
		goto out;
	}
	remove_file(file);
	notes_strip(&b);

	if (!b.len) {
		/* an emptied note is one that was taken away, as in git */
		if (!e) {
			rc = 0;
			goto out;
		}
		notes_del_entry(&ist, &obj);
		if (notes_store(r, &ist,
				"Notes removed by 'gitprompt notes edit'\n") < 0) {
			gp_error("cannot store the note");
			goto out;
		}
		printf("Removing note for object %s\n", rev ? rev : "HEAD");
		rc = 0;
		goto out;
	}

	if (odb_write(&r->odb, OBJ_BLOB, b.b, b.len, &blob) < 0) {
		gp_error("cannot write the note");
		goto out;
	}
	notes_put_entry(&ist, &obj, &blob);
	if (notes_store(r, &ist, "Notes added by 'gitprompt notes edit'\n") < 0) {
		gp_error("cannot store the note");
		goto out;
	}
	rc = 0;
out:
	free(editor);
	free(file);
	buf_release(&cur);
	buf_release(&b);
	index_release(&ist);
	return rc;
}

int cmd_notes(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *sub;

	opts_init(&o, argc, argv, (const char *const[]){
		"-m=", "-F=", "-f", "--force", NULL });

	sub = opts_arg(&o, 0);
	if (!sub || !strcmp(sub, "list")) {
		if (!notes_no_options(&o, "list"))
			return 1;
		return notes_list(r);
	}
	if (!strcmp(sub, "add"))
		return notes_add(r, &o, opts_arg(&o, 1));
	if (!strcmp(sub, "show")) {
		if (!notes_no_options(&o, "show"))
			return 1;
		return notes_show(r, opts_arg(&o, 1));
	}
	if (!strcmp(sub, "remove")) {
		if (!notes_no_options(&o, "remove"))
			return 1;
		return notes_remove(r, opts_arg(&o, 1));
	}
	if (!strcmp(sub, "edit")) {
		if (!notes_no_options(&o, "edit"))
			return 1;
		return notes_edit(r, opts_arg(&o, 1));
	}
	if (!strcmp(sub, "prune")) {
		if (!notes_no_options(&o, "prune"))
			return 1;
		return notes_prune(r);
	}

	gp_error("'%s' is not a notes subcommand", sub);
	return 1;
}
