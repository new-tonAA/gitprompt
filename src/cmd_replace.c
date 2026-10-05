/*
 * replace -- one object standing in for another when the store is read.
 *
 * A replace ref is an ordinary ref: refs/replace/<id> names the object to be
 * read wherever <id> would have been read.  Nothing is rewritten, so the object
 * as it was written is still there byte for byte, and the substitution is a
 * property of this repository and of anyone who has the ref -- a clone without
 * it sees the history exactly as it was written.  That is what makes this a
 * repair rather than a rewrite: a wrong author line, a dropped parent or a
 * commit made on the wrong branch can be papered over without moving the id
 * that everything else already points at.
 *
 * The lookup lives in odb.c, at the read, because the read is the one place
 * every reader in this tree passes through.  What is here is the ref itself:
 * making one, listing them, and taking one away.
 *
 * Two of git's liberties are not taken.  Replacing an object with itself, and
 * replacing two objects with each other, both build a chain the read has to
 * give up on; git writes them anyway and lets the read stop when its hops run
 * out, while this refuses them.  What is refused costs one error message, where
 * what is written costs every later read in that repository a hop that goes
 * nowhere.  A ref uses a different object type from the one it replaces is
 * refused whether or not --force is given, where git checks only for a ref it
 * is about to create.
 */
#include "gp.h"

#define REPLACE_PREFIX "refs/replace/"

/* the ref that says what stands in for `oid` */
static char *replace_ref(const oid_t *oid)
{
	char hex[GP_SHA1_HEXSZ + 1];

	oid_hex(oid, hex);
	return xstrfmt("%s%s", REPLACE_PREFIX, hex);
}

/*
 * The object a revision argument names.  A replace ref is written against an
 * object, so this takes anything that names one -- a ref, a full or abbreviated
 * id -- and dies on the rest, as the other commands that take a revision do.
 */
static void replace_rev(struct repo *r, const char *rev, oid_t *out)
{
	if (resolve_rev(r, rev, out) < 0 || !odb_exists(&r->odb, out))
		gp_die("replace: '%s' does not name an object", rev);
}

/*
 * Whether following the chain from `start` arrives back at `want`.  The walk is
 * the read's own, hops and all, so a chain this refuses to extend is one the
 * read would have stopped walking anyway.
 */
static int chain_reaches(struct repo *r, const oid_t *start, const oid_t *want)
{
	oid_t cur = *start;
	int i;

	for (i = 0; i < 5; i++) {
		char *ref = replace_ref(&cur);
		oid_t next;
		int rc = refs_read(&r->refs, ref, &next);

		free(ref);
		if (rc < 0)
			return 0;
		if (oid_equal(&next, want))
			return 1;
		cur = next;
	}
	return 0;
}

static int replace_set(struct repo *r, const char *rev, const char *with,
		       int force)
{
	enum obj_type ours, theirs;
	oid_t obj, repl;
	char ha[GP_SHA1_HEXSZ + 1], hb[GP_SHA1_HEXSZ + 1];
	char *ref = NULL;
	int rc = 1;

	replace_rev(r, rev, &obj);
	replace_rev(r, with, &repl);
	oid_hex(&obj, ha);
	oid_hex(&repl, hb);

	if (oid_equal(&obj, &repl)) {
		gp_error("replace: '%s' cannot stand in for itself", ha);
		return 1;
	}
	if (chain_reaches(r, &repl, &obj)) {
		gp_error("replace: '%s' is reachable from '%s' already; "
			 "that chain would not end", ha, hb);
		return 1;
	}

	if (odb_type_of(&r->odb, &obj, &ours) < 0 ||
	    odb_type_of(&r->odb, &repl, &theirs) < 0) {
		gp_error("replace: cannot read the objects");
		return 1;
	}
	if (ours != theirs) {
		gp_error("Objects must be of the same type.");
		gp_error("'%s' points to an object of type '%s',", ha,
			 obj_type_name(ours));
		gp_error("while '%s' points to an object of type '%s'.", hb,
			 obj_type_name(theirs));
		return 1;
	}

	ref = replace_ref(&obj);
	if (!force && refs_exists(&r->refs, ref)) {
		gp_error("replace: replace ref '%s' already exists", ref);
		goto out;
	}
	if (refs_write(&r->refs, ref, &repl) < 0) {
		gp_error("replace: cannot write %s", ref);
		goto out;
	}
	rc = 0;
out:
	free(ref);
	return rc;
}

/* ------------------------------------------------------------------ */
/* listing                                                             */

struct replace_list {
	char **v;
	size_t nr, alloc;
};

static void list_add(struct replace_list *l, const char *s)
{
	if (l->nr == l->alloc) {
		l->alloc = l->alloc ? l->alloc * 2 : 16;
		l->v = xrealloc(l->v, l->alloc * sizeof *l->v);
	}
	l->v[l->nr++] = xstrdup(s);
}

static void list_cb(const char *name, const oid_t *oid, void *data)
{
	struct replace_list *l = data;
	size_t plen = strlen(REPLACE_PREFIX);

	(void)oid;
	if (!strncmp(name, REPLACE_PREFIX, plen) && strlen(name) > plen)
		list_add(l, name + plen);
}

static int cmp_names(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/*
 * The names of the replace refs, which is what git prints -- the object being
 * replaced, not the one replacing it, since that is the id a reader would have
 * in hand.  A pattern is a glob matched against the whole name, no slash
 * anywhere in it, which is git's rule too; a plain name matches itself.
 */
static int replace_list(struct repo *r, const char *pattern)
{
	struct replace_list l = { NULL, 0, 0 };
	size_t i;

	refs_list(&r->refs, REPLACE_PREFIX, list_cb, &l);
	refs_list_packed(&r->refs, REPLACE_PREFIX, list_cb, &l);

	if (l.nr > 1)
		qsort(l.v, l.nr, sizeof *l.v, cmp_names);

	for (i = 0; i < l.nr; i++) {
		if (i && !strcmp(l.v[i], l.v[i - 1]))
			continue;       /* both the loose and the packed copy */
		if (pattern && !glob_match_name(pattern, l.v[i]))
			continue;
		printf("%s\n", l.v[i]);
	}
	for (i = 0; i < l.nr; i++)
		free(l.v[i]);
	free(l.v);
	return 0;
}

/* ------------------------------------------------------------------ */
/* taking one away                                                     */

static int replace_delete(struct repo *r, const char *rev)
{
	oid_t obj;
	char hex[GP_SHA1_HEXSZ + 1];
	char *ref;
	int rc = 0;

	/*
	 * A replace ref outlives the object it is about -- a ref left over a
	 * pruned object is exactly the case `-l` shows -- so a full id is taken
	 * as it stands when the object itself cannot be resolved.
	 */
	if (resolve_rev(r, rev, &obj) < 0 &&
	    (strlen(rev) != GP_SHA1_HEXSZ || oid_parse(&obj, rev) < 0)) {
		gp_error("replace: '%s' does not name an object", rev);
		return 1;
	}
	oid_hex(&obj, hex);
	ref = replace_ref(&obj);
	if (!refs_exists(&r->refs, ref)) {
		gp_error("replace: replace ref '%s' not found", hex);
		rc = 1;
	} else if (refs_delete(&r->refs, ref) < 0) {
		gp_error("replace: cannot remove %s", ref);
		rc = 1;
	} else {
		printf("Deleted replace ref '%s'\n", hex);
	}
	free(ref);
	return rc;
}

/* ------------------------------------------------------------------ */
/* editing one                                                         */

/*
 * What is edited is what would be read: the replacement when there is one, and
 * the object itself otherwise, so the first `-e` over an object and the second
 * are the same operation.  The bytes go to the editor, and what comes back is
 * stored as an object of the same type -- the raw content, not a parse and a
 * re-render, so a header this tree does not know about survives the round trip.
 */
static int replace_edit(struct repo *r, const char *rev)
{
	oid_t obj, cur, made;
	enum obj_type t;
	struct buf before = BUF_INIT, after = BUF_INIT;
	char hex[GP_SHA1_HEXSZ + 1];
	char *editor = NULL, *file = NULL, *ref = NULL;
	int rc = 1;

	replace_rev(r, rev, &obj);
	if (odb_type_of(&r->odb, &obj, &t) < 0) {
		gp_error("replace: cannot read '%s'", rev);
		return 1;
	}
	ref = replace_ref(&obj);
	if (refs_read(&r->refs, ref, &cur) < 0)
		cur = obj;
	if (odb_read(&r->odb, &cur, NULL, &before) < 0) {
		gp_error("replace: cannot read what '%s' is replaced by", rev);
		goto out;
	}

	editor = repo_editor_command(r);
	if (!editor) {
		gp_error("replace edit: no editor is configured\n"
			 "hint: set GIT_EDITOR or EDITOR");
		goto out;
	}
	file = repo_worktree_path(r, "REPLACE_EDIT");
	if (write_file(file, before.b ? (const void *)before.b : "",
		       before.len) < 0) {
		gp_error("replace edit: cannot write %s", file);
		goto out;
	}
	if (repo_run_editor(editor, file) < 0) {
		gp_error("replace edit: the editor exited with an error; "
			 "nothing was saved");
		goto out;
	}
	if (read_file(file, &after) < 0) {
		gp_error("replace edit: cannot read back %s", file);
		goto out;
	}
	remove_file(file);

	oid_hex(&obj, hex);
	if (after.len == before.len && !memcmp(after.b, before.b, before.len)) {
		gp_error("replace: new object is the same as the old one: '%s'",
			 hex);
		goto out;
	}
	if (odb_write(&r->odb, t, after.b, after.len, &made) < 0) {
		gp_error("replace: cannot write the edited object");
		goto out;
	}
	if (refs_write(&r->refs, ref, &made) < 0) {
		gp_error("replace: cannot write %s", ref);
		goto out;
	}
	printf("Edited replace ref '%s'\n", hex);
	rc = 0;
out:
	free(editor);
	free(file);
	free(ref);
	buf_release(&before);
	buf_release(&after);
	return rc;
}

/* ------------------------------------------------------------------ */

int cmd_replace(struct repo *r, int argc, char **argv)
{
	struct opts o;
	int force, del, list, edit;

	opts_init(&o, argc, argv, (const char *const[]){
		"-f", "--force", "-d", "--delete", "-l", "--list",
		"-e", "--edit", NULL });

	force = opts_flag(&o, "-f") || opts_flag(&o, "--force");
	del = opts_flag(&o, "-d") || opts_flag(&o, "--delete");
	list = opts_flag(&o, "-l") || opts_flag(&o, "--list");
	edit = opts_flag(&o, "-e") || opts_flag(&o, "--edit");

	if (edit) {
		if (opts_count(&o) != 1) {
			gp_error("replace: -e needs exactly one object\n"
				 "usage: gitprompt replace [-f] -e <object>");
			return 1;
		}
		return replace_edit(r, opts_arg(&o, 0));
	}
	if (del) {
		int i, rc = 0;

		if (opts_count(&o) < 1) {
			gp_error("replace: -d needs at least one object\n"
				 "usage: gitprompt replace -d <object>...");
			return 1;
		}
		for (i = 0; i < opts_count(&o); i++)
			if (replace_delete(r, opts_arg(&o, i)))
				rc = 1;
		return rc;
	}
	/* a bare `gitprompt replace` lists them, which is what git's does */
	if (list || opts_count(&o) == 0) {
		if (opts_count(&o) > 1) {
			gp_error("replace: -l takes at most one pattern");
			return 1;
		}
		return replace_list(r, opts_arg(&o, 0));
	}

	if (opts_count(&o) != 2) {
		gp_error("replace: expected <object> <replacement>\n"
			 "usage: gitprompt replace [-f] <object> <replacement>");
		return 1;
	}
	return replace_set(r, opts_arg(&o, 0), opts_arg(&o, 1), force);
}
