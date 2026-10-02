/*
 * cmd_rebase.c - cherry-pick, rebase and revert.
 *
 * All three take a commit made somewhere else and replay it here, so all three
 * are the three-way merge `merge` already performs read the other way round:
 * what the commit changed is its diff against its own parent, and that diff is
 * applied to the tree HEAD holds now.
 *
 * A pick and a rebase want that diff as it was made -- the commit's parent is
 * the merge base, HEAD is our side, and the commit is theirs.  A revert wants
 * the same diff the other way up: the commit is the base and the parent it is
 * measured against is theirs, so the change lands subtracted rather than
 * added.  Which parent that is on a merge is what -m names.
 *
 * A replay is where the prompt rule earns its keep.  A commit carries the
 * prompts it has that its first parent does not, so the replayed commit is
 * given exactly those prompts against *its new parent*: one replayed onto a
 * branch that already holds the prompt carries none, and one replayed onto a
 * branch that lacks it brings it along.  Nothing has to be copied from the
 * original commit for that to be true -- it is the same rule every other
 * commit follows, applied to the parent the commit actually has now.  That
 * holds for a revert as much as for a pick: undoing the code undoes the
 * prompts with it, because the tree says so, not because anything was copied.
 *
 * A replay that stops leaves .gitprompt/sequencer/ behind, holding the commits
 * still to come, the kind of replay, the parent a revert is measured against,
 * and where HEAD was.  --continue, --skip and --abort read it.  There is no
 * CHERRY_PICK_HEAD beside MERGE_HEAD: a replayed commit has one parent, so
 * there is no second one to name.
 */
#include "gp.h"

#include <ctype.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* the state a stopped replay leaves behind                            */

/*
 * The commit at the front of `todo` is the one being applied, so a stopped
 * replay needs nothing else to say where it is: --continue records what the
 * index holds for that commit, and --skip drops it.
 */
struct seq {
	char *kind;             /* "cherry-pick", "rebase" or "revert" */
	int mainline;           /* a revert's -m: which parent it undoes against */
	oid_t orig;             /* what HEAD was before the replay began */
	char *head_name;        /* the branch to move, or NULL when detached */
	oid_t *todo;
	size_t nr;
};

static char *seq_path(struct repo *r, const char *name)
{
	return repo_git_path(r, "sequencer/%s", name);
}

static char *seq_dir(struct repo *r)
{
	return repo_git_path(r, "sequencer");
}

static void trim_ws(char *s)
{
	size_t n = strlen(s);

	while (n && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
}

/*
 * An id as a file.  It is written without a newline, as git writes refs, so
 * the file and the id are the same bytes; the reader tolerates trailing space
 * all the same, since an editor may add one.
 */
static void oid_write_file(const char *path, const oid_t *oid)
{
	char hex[GP_SHA1_HEXSZ + 1];

	oid_hex(oid, hex);
	write_file(path, hex, GP_SHA1_HEXSZ);
}

static int oid_read_file(const char *path, oid_t *out)
{
	struct buf b;
	char *hex;
	int rc = -1;

	buf_init(&b);
	if (read_file(path, &b) == 0 && b.len) {
		size_t n = 0;

		while (n < b.len && !isspace((unsigned char)b.b[n]))
			n++;
		if (n >= GP_SHA1_HEXSZ) {
			hex = xstrndup((const char *)b.b, GP_SHA1_HEXSZ);
			rc = oid_parse(out, hex);
			free(hex);
		}
	}
	buf_release(&b);
	return rc;
}

static void seq_release(struct seq *s);

static void seq_save(struct repo *r, const struct seq *s)
{
	char *dir = seq_dir(r);
	char *path;
	struct buf b;
	size_t i;

	mkdir_p(dir);
	free(dir);

	path = seq_path(r, "kind");
	write_file(path, s->kind, strlen(s->kind));
	free(path);

	path = seq_path(r, "mainline");
	{
		char *n = xstrfmt("%d\n", s->mainline);

		write_file(path, n, strlen(n));
		free(n);
	}
	free(path);

	path = seq_path(r, "orig-head");
	oid_write_file(path, &s->orig);
	free(path);

	path = seq_path(r, "head-name");
	if (s->head_name)
		write_file(path, s->head_name, strlen(s->head_name));
	else
		remove(path);
	free(path);

	buf_init(&b);
	for (i = 0; i < s->nr; i++) {
		char hex[GP_SHA1_HEXSZ + 1];

		oid_hex(&s->todo[i], hex);
		buf_addf(&b, "%s\n", hex);
	}
	path = seq_path(r, "todo");
	write_file(path, b.b, b.len);
	buf_release(&b);
	free(path);
}

static int seq_load(struct repo *r, struct seq *s)
{
	char *dir = seq_dir(r);
	char *path;
	struct buf b;

	memset(s, 0, sizeof *s);
	if (!is_directory(dir)) {
		free(dir);
		return -1;
	}
	free(dir);

	path = seq_path(r, "kind");
	buf_init(&b);
	if (read_file(path, &b) < 0 || !b.len) {
		buf_release(&b);
		free(path);
		return -1;
	}
	s->kind = xstrndup((const char *)b.b, b.len);
	trim_ws(s->kind);
	buf_release(&b);
	free(path);

	path = seq_path(r, "mainline");
	buf_init(&b);
	s->mainline = 1;
	if (read_file(path, &b) == 0 && b.len) {
		char *txt = xstrndup((const char *)b.b, b.len);
		long n = strtol(txt, NULL, 10);

		if (n > 0)
			s->mainline = (int)n;
		free(txt);
	}
	buf_release(&b);
	free(path);

	path = seq_path(r, "orig-head");
	if (oid_read_file(path, &s->orig) < 0) {
		free(path);
		seq_release(s);
		return -1;
	}
	free(path);

	path = seq_path(r, "head-name");
	buf_init(&b);
	if (read_file(path, &b) == 0 && b.len) {
		s->head_name = xstrndup((const char *)b.b, b.len);
		trim_ws(s->head_name);
		if (!s->head_name[0]) {
			free(s->head_name);
			s->head_name = NULL;
		}
	}
	buf_release(&b);
	free(path);

	path = seq_path(r, "todo");
	buf_init(&b);
	if (read_file(path, &b) == 0) {
		const char *p = (const char *)b.b;
		const char *end = p + b.len;

		while (p < end) {
			const char *nl = memchr(p, '\n', (size_t)(end - p));
			size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);

			if (len >= GP_SHA1_HEXSZ) {
				char *hex = xstrndup(p, GP_SHA1_HEXSZ);
				oid_t o;

				if (oid_parse(&o, hex) == 0) {
					s->todo = xrealloc(s->todo,
						(s->nr + 1) * sizeof(*s->todo));
					s->todo[s->nr++] = o;
				}
				free(hex);
			}
			p = nl ? nl + 1 : end;
		}
	}
	buf_release(&b);
	free(path);
	return 0;
}

static void seq_release(struct seq *s)
{
	free(s->kind);
	free(s->head_name);
	free(s->todo);
	memset(s, 0, sizeof *s);
}

static void seq_clear(struct repo *r)
{
	char *dir = seq_dir(r);

	remove_dir_recursive(dir);
	free(dir);
}

int replay_in_progress(struct repo *r, char **kind)
{
	struct seq s;

	if (seq_load(r, &s) < 0)
		return 0;
	if (kind)
		*kind = xstrdup(s.kind);
	seq_release(&s);
	return 1;
}

/* ------------------------------------------------------------------ */
/* shared                                                              */

static int merge_favor_of(const struct opts *o, enum merge_favor *out)
{
	const char *x = opts_value(o, "-X");

	if (!x)
		x = opts_value(o, "--strategy-option");
	if (!x)
		return 0;
	if (!strcmp(x, "ours"))
		*out = MERGE_FAVOR_OURS;
	else if (!strcmp(x, "theirs"))
		*out = MERGE_FAVOR_THEIRS;
	else {
		gp_error("unknown strategy option '%s'\n"
			 "hint: the ones there are are 'ours' and 'theirs'", x);
		return -1;
	}
	return 0;
}

/*
 * How a reflog names a replay.  git's own words: a pick is "cherry-pick: <the
 * commit's subject>", and one commit of a rebase is "rebase (pick): <the
 * commit's subject>" -- the entry the whole rebase is made of.
 */
static char *replay_reflog(const char *kind, const struct commit *c)
{
	char *line = commit_message_line(c);
	char *msg;

	if (!strcmp(kind, "rebase"))
		msg = xstrfmt("rebase (pick): %s", line ? line : "");
	else
		msg = xstrfmt("%s: %s", kind, line ? line : "");
	free(line);
	return msg;
}

static char *head_label(struct repo *r)
{
	char *t = refs_head_target(&r->refs);
	char *name = NULL;

	if (t && !strncmp(t, "refs/heads/", 11))
		name = xstrdup(t + 11);
	free(t);
	return name ? name : xstrdup("detached HEAD");
}

/* "somebody else's commit is in the way" is the same refusal three times over */
static void refuse_if_replaying(struct repo *r)
{
	char *kind = NULL;
	oid_t other;

	if (replay_in_progress(r, &kind)) {
		gp_error("a %s is already in progress", kind);
		fprintf(stderr, "hint: finish it with 'gitprompt %s --continue', "
				"skip the commit with 'gitprompt %s --skip',\n"
				"hint: or throw it away with 'gitprompt %s --abort'\n",
				kind, kind, kind);
		free(kind);
		exit(1);
	}
	if (merge_in_progress(r, &other) == 0)
		gp_die("You have not concluded your merge (MERGE_HEAD exists)\n"
		       "hint: resolve the conflicts and commit, or run "
		       "'gitprompt merge --abort'");
}

/*
 * A replay rewrites the work tree from a merge and --abort rewrites it again,
 * so it needs one with nothing of its own in it: a change left unrecorded
 * would be merged into the commit being replayed, or thrown away by the abort.
 * git refuses the same three things, and for the same reasons.
 */
static int require_clean(struct repo *r, const char *kind)
{
	struct index_state ist;
	struct commit hc = COMMIT_INIT;
	oid_t head, head_tree, index_tree;
	int have_head;
	int rc = 0;
	char **dirty = NULL;
	int n, i;

	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));

	if (index_has_unmerged(&ist)) {
		gp_error("cannot %s: you have unmerged paths\n"
			 "hint: resolve them first, or run 'gitprompt %s --abort'",
			 kind, kind);
		index_release(&ist);
		return -1;
	}

	have_head = refs_head(&r->refs, &head) == 0;
	if (have_head) {
		read_commit(r, &head, &hc);
		head_tree = hc.tree;
		if (write_tree_from_index(r, &ist, &index_tree) == 0 &&
		    !oid_equal(&index_tree, &head_tree)) {
			gp_error("cannot %s: you have staged changes\n"
				 "hint: commit them or unstage them first", kind);
			rc = -1;
		}
		commit_release(&hc);
	}
	index_release(&ist);
	if (rc < 0)
		return rc;

	n = worktree_dirty_paths(r, &dirty);
	if (n > 0) {
		gp_error("cannot %s: Your local changes to the following files "
			 "would be overwritten:", kind);
		for (i = 0; i < n; i++)
			fprintf(stderr, "\t%s\n", dirty[i]);
		fprintf(stderr, "hint: commit them first\n");
		path_list_free(dirty);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* replaying one commit                                                */

/*
 * What a replayed commit says.  A pick keeps the message of the commit it
 * replayed: the point of a replay is that the work, and the words for it, are
 * still that author's.
 *
 * A revert is a commit of one's own, so it says what it did instead -- the
 * sentence git writes, naming the subject it undoes and the commit it undoes
 * by id.  Undoing a revert says "Reapply" rather than "Revert" of a "Revert",
 * which is the same sentence read the other way and is what keeps a history of
 * undos legible.
 */
static char *replay_message(const char *kind, const struct commit *src,
			    const oid_t *src_oid)
{
	char *line = commit_message_line(src);
	char *msg;

	if (strcmp(kind, "revert")) {
		if (src->message && src->message[0]) {
			size_t n = strlen(src->message);

			msg = src->message[n - 1] == '\n' ? xstrdup(src->message)
							  : xstrfmt("%s\n", src->message);
		} else {
			msg = xstrdup("");
		}
	} else if (line && !strncmp(line, "Revert \"", 8) &&
		   strlen(line) > 9 && line[strlen(line) - 1] == '"') {
		char *inner = xstrndup(line + 8, strlen(line) - 9);

		msg = xstrfmt("Reapply \"%s\"\n", inner);
		free(inner);
	} else {
		char hex[GP_SHA1_HEXSZ + 1];

		oid_hex(src_oid, hex);
		msg = xstrfmt("Revert \"%s\"\n\nThis reverts commit %s.\n",
			      line ? line : "", hex);
	}
	free(line);
	return msg;
}

/*
 * Record the replayed commit and move HEAD onto it.  It is a commit like any
 * other: one parent, the message and the author it should have, and the
 * prompts it brings in against the parent it has now.
 */
static void replay_commit(struct repo *r, const oid_t *src_oid,
			  const struct index_state *ist, const oid_t *tree,
			  const oid_t *parent, const char *kind,
			  struct commit *out_c, oid_t *out_oid)
{
	struct commit src = COMMIT_INIT;
	struct commit pc = COMMIT_INIT;
	struct commit c = COMMIT_INIT;
	struct buf ident, body;
	char *branch_ref, *msg;

	read_commit(r, src_oid, &src);
	read_commit(r, parent, &pc);
	collect_commit_prompts(r, ist, &pc.tree, 1, &c);
	commit_release(&pc);

	buf_init(&ident);
	buf_init(&body);
	repo_ident_with_time(r, &ident);

	c.tree = *tree;
	oid_array_append(&c.parents, parent);
	/*
	 * A revert is a commit of one's own -- whoever ran it wrote it -- where a
	 * pick is somebody else's work landing here, and stays theirs.
	 */
	if (!strcmp(kind, "revert") || !src.author)
		c.author = xstrdup(buf_cstr(&ident));
	else
		c.author = xstrdup(src.author);
	c.committer = xstrdup(buf_cstr(&ident));
	c.message = replay_message(kind, &src, src_oid);
	c.session = repo_current_session(r);

	commit_format(&c, &body);
	if (odb_write(&r->odb, OBJ_COMMIT, body.b, body.len, out_oid) < 0)
		gp_die("cannot write the replayed commit");

	msg = replay_reflog(kind, &c);
	branch_ref = refs_head_target(&r->refs);
	if (branch_ref) {
		refs_write(&r->refs, branch_ref, out_oid);
		refs_reflog(&r->refs, branch_ref, parent, out_oid, msg);
		refs_reflog_head(&r->refs, branch_ref, parent, out_oid, msg);
		free(branch_ref);
	} else {
		refs_set_head_detached(&r->refs, out_oid);
		refs_reflog_head(&r->refs, NULL, parent, out_oid, msg);
	}
	free(msg);

	*out_c = c;
	commit_release(&src);
	buf_release(&ident);
	buf_release(&body);
}

static void print_replay_line(struct repo *r, const struct commit *c,
			      const oid_t *oid)
{
	char *br = head_label(r);
	char *sh = abbrev_oid(oid);
	char *line = commit_message_line(c);

	printf("[%s %s] %s\n", br, sh, line ? line : "");
	free(br);
	free(sh);
	free(line);
}

/*
 * Apply one commit.  Returns 0 when it was recorded, 1 when the merge
 * conflicted -- the index and the work tree hold the conflict, and the commit
 * is still the front of the todo -- 2 when it brings nothing to its new parent,
 * and -1 on an error.
 */
static int replay_one(struct repo *r, const oid_t *src_oid,
		      enum merge_favor favor, const char *kind, int mainline)
{
	struct commit src = COMMIT_INIT;
	struct commit hc = COMMIT_INIT;
	struct merge_result res;
	struct index_state mindex;
	oid_t head, head_tree, base_tree, side_tree, tree;
	const oid_t *base, *theirs;
	char *label;
	int rc;

	read_commit(r, src_oid, &src);
	if (refs_head(&r->refs, &head) < 0)
		gp_die("%s: HEAD has no commits yet", kind);
	read_commit(r, &head, &hc);
	head_tree = hc.tree;

	/*
	 * Which side of the merge each tree is on.  A pick applies the commit's
	 * own diff against its first parent, so that parent is the base and the
	 * commit is the far side; a commit with no parent changed everything it
	 * holds, so its base is empty and every path is an addition.  A revert
	 * applies the same diff subtracted, so the commit is the base and the
	 * parent it is measured against -- the mainline -m names -- is the far
	 * side; with no parent at all the far side is the empty tree, which is
	 * how undoing the first commit of all removes what it added.
	 */
	if (!strcmp(kind, "revert")) {
		base = &src.tree;
		if (src.parents.nr > 0) {
			struct commit pc = COMMIT_INIT;

			read_commit(r, &src.parents.oid[mainline - 1], &pc);
			side_tree = pc.tree;
			commit_release(&pc);
		} else {
			struct index_state none;

			memset(&none, 0, sizeof none);
			if (write_tree_from_index(r, &none, &side_tree) < 0)
				gp_die("cannot write the empty tree");
		}
		theirs = &side_tree;
	} else if (src.parents.nr > 0) {
		struct commit pc = COMMIT_INIT;

		read_commit(r, &src.parents.oid[0], &pc);
		base_tree = pc.tree;
		commit_release(&pc);
		base = &base_tree;
		theirs = &src.tree;
	} else {
		base = NULL;
		theirs = &src.tree;
	}

	label = abbrev_oid(src_oid);
	memset(&res, 0, sizeof res);
	memset(&mindex, 0, sizeof mindex);
	merge_trees(r, base, &head_tree, theirs, &res, &mindex, favor, label);
	free(label);
	index_write(&mindex, repo_index_path(r));

	if (res.conflicts) {
		index_release(&mindex);
		commit_release(&src);
		commit_release(&hc);
		return 1;
	}
	if (write_tree_from_index(r, &mindex, &tree) < 0)
		gp_die("cannot write the merged tree");
	if (oid_equal(&tree, &head_tree)) {
		index_release(&mindex);
		commit_release(&src);
		commit_release(&hc);
		return 2;
	}

	{
		struct commit made = COMMIT_INIT;
		oid_t made_oid;

		replay_commit(r, src_oid, &mindex, &tree, &head, kind, &made,
			      &made_oid);
		print_replay_line(r, &made, &made_oid);
		commit_release(&made);
	}

	index_release(&mindex);
	commit_release(&src);
	commit_release(&hc);
	rc = 0;
	return rc;
}

/* ------------------------------------------------------------------ */
/* the loop                                                            */

static void report_conflict(struct repo *r, const struct seq *s,
			    const oid_t *c)
{
	struct commit cm = COMMIT_INIT;
	char *sh = abbrev_oid(c);
	char *line;

	read_commit(r, c, &cm);
	line = commit_message_line(&cm);
	gp_error("could not apply %s... %s", sh, line ? line : "");
	fprintf(stderr,
		"hint: resolve the conflicts, mark them with 'gitprompt add', "
		"then run\nhint:   gitprompt %s --continue\n"
		"hint: to leave the commit out of the replay instead, run\n"
		"hint:   gitprompt %s --skip\n"
		"hint: and to put everything back as it was, run\n"
		"hint:   gitprompt %s --abort\n", s->kind, s->kind, s->kind);
	free(line);
	free(sh);
	commit_release(&cm);
}

static void report_empty(struct repo *r, const struct seq *s, const oid_t *c)
{
	struct commit cm = COMMIT_INIT;
	char *sh = abbrev_oid(c);
	char *line;

	read_commit(r, c, &cm);
	line = commit_message_line(&cm);
	fprintf(stderr, "The previous %s of %s... %s is now empty, so there is "
			"nothing to record.\n", s->kind, sh, line ? line : "");
	/*
	 * Whatever the commit carried is already here, so --skip is the way past
	 * it.  There is no way to record it anyway: an empty commit left in the
	 * replay would leave the state naming a commit that has been made, and a
	 * `commit --allow-empty` would do the same, which is why it is refused
	 * while a replay is in progress.
	 */
	fprintf(stderr,
		"hint: 'gitprompt %s --skip' leaves it out of the replay\n"
		"hint: and 'gitprompt %s --abort' puts everything back as it was\n",
		s->kind, s->kind);
	free(line);
	free(sh);
	commit_release(&cm);
}

/*
 * The last commit has landed.  A rebase has been running on a detached HEAD so
 * that the branch stayed where it was until the whole replay was done, and this
 * is where it is put back -- on the replayed commits, which is what makes the
 * rebase have happened.  A cherry-pick moved the branch as it went, so there is
 * nothing left to do but forget the state.
 */
static void seq_finish(struct repo *r, struct seq *s)
{
	oid_t head;

	if (!strcmp(s->kind, "rebase") && s->head_name &&
	    refs_head(&r->refs, &head) == 0) {
		oid_t old;
		int had = refs_read(&r->refs, s->head_name, &old) == 0;

		refs_write(&r->refs, s->head_name, &head);
		refs_reflog(&r->refs, s->head_name, had ? &old : &null_oid, &head,
			    "rebase (finish)");
		refs_set_head(&r->refs, s->head_name);
		refs_reflog_head(&r->refs, s->head_name, had ? &old : &null_oid,
				 &head, "rebase (finish)");
		printf("Successfully rebased and updated %s.\n", s->head_name);
	}
	seq_clear(r);
}

static void seq_drop_front(struct seq *s)
{
	if (!s->nr)
		return;
	memmove(s->todo, s->todo + 1, (s->nr - 1) * sizeof(*s->todo));
	s->nr--;
}

static int seq_run(struct repo *r, struct seq *s, enum merge_favor favor)
{
	while (s->nr) {
		oid_t c = s->todo[0];
		int rc = replay_one(r, &c, favor, s->kind, s->mainline);

		if (rc < 0)
			return 1;
		if (rc == 1) {
			report_conflict(r, s, &c);
			return 1;
		}
		if (rc == 2) {
			report_empty(r, s, &c);
			return 1;
		}
		seq_drop_front(s);
		seq_save(r, s);
	}
	seq_finish(r, s);
	return 0;
}

/* ------------------------------------------------------------------ */
/* continue, skip, abort                                               */

static void report_still_unmerged(struct index_state *ist, const char *kind)
{
	size_t nconf = 0, k;
	char **conf = index_unmerged_paths(ist, &nconf);

	gp_error("You have unmerged paths.");
	for (k = 0; k < nconf; k++)
		fprintf(stderr, "\t%s\n", conf[k]);
	fprintf(stderr, "hint: resolve them and 'gitprompt add' them, then run "
			"'gitprompt %s --continue'\n", kind);
	index_paths_free(conf);
}

static int seq_continue(struct repo *r, enum merge_favor favor)
{
	struct seq s;
	struct index_state ist;
	struct commit hc = COMMIT_INIT, made = COMMIT_INIT;
	oid_t head, head_tree, tree, made_oid;
	int rc;

	if (seq_load(r, &s) < 0) {
		gp_error("no cherry-pick, rebase or revert to continue");
		return 1;
	}
	if (!s.nr) {
		seq_clear(r);
		seq_release(&s);
		gp_error("no cherry-pick, rebase or revert to continue");
		return 1;
	}

	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));
	if (index_has_unmerged(&ist)) {
		report_still_unmerged(&ist, s.kind);
		index_release(&ist);
		seq_release(&s);
		return 1;
	}
	if (refs_head(&r->refs, &head) < 0)
		gp_die("%s: HEAD has no commits yet", s.kind);
	read_commit(r, &head, &hc);
	head_tree = hc.tree;
	commit_release(&hc);

	if (write_tree_from_index(r, &ist, &tree) < 0)
		gp_die("cannot write the merged tree");
	if (oid_equal(&tree, &head_tree)) {
		/* the resolution undid the commit, or --continue was run without
		 * one: either way there is nothing here to record */
		report_empty(r, &s, &s.todo[0]);
		index_release(&ist);
		seq_release(&s);
		return 1;
	}

	replay_commit(r, &s.todo[0], &ist, &tree, &head, s.kind, &made, &made_oid);
	print_replay_line(r, &made, &made_oid);
	commit_release(&made);
	index_release(&ist);

	seq_drop_front(&s);
	seq_save(r, &s);
	rc = seq_run(r, &s, favor);
	seq_release(&s);
	return rc;
}

static int seq_skip(struct repo *r, enum merge_favor favor)
{
	struct seq s;
	struct commit hc = COMMIT_INIT;
	oid_t head, head_tree;
	int rc;

	if (seq_load(r, &s) < 0 || !s.nr) {
		seq_release(&s);
		gp_error("no cherry-pick, rebase or revert to skip");
		return 1;
	}
	if (refs_head(&r->refs, &head) < 0)
		gp_die("%s: HEAD has no commits yet", s.kind);
	read_commit(r, &head, &hc);
	head_tree = hc.tree;
	commit_release(&hc);

	/* the commit is not being applied at all, so whatever the attempt left
	 * in the index and the work tree goes with it */
	checkout_tree(r, &head_tree, 1, 1);
	seq_drop_front(&s);
	seq_save(r, &s);
	rc = seq_run(r, &s, favor);
	seq_release(&s);
	return rc;
}

static int seq_abort(struct repo *r)
{
	struct seq s;
	struct commit orig = COMMIT_INIT;
	oid_t old;
	char *msg;
	int detached;

	if (seq_load(r, &s) < 0) {
		gp_error("no cherry-pick, rebase or revert to abort");
		return 1;
	}
	read_commit(r, &s.orig, &orig);
	msg = xstrfmt("%s (abort)", s.kind);

	detached = refs_head(&r->refs, &old) < 0;
	if (s.head_name) {
		int had = refs_read(&r->refs, s.head_name, &old) == 0;

		refs_write(&r->refs, s.head_name, &s.orig);
		refs_reflog(&r->refs, s.head_name, had ? &old : &null_oid,
			    &s.orig, msg);
		refs_set_head(&r->refs, s.head_name);
		refs_reflog_head(&r->refs, s.head_name, had ? &old : &null_oid,
				 &s.orig, msg);
	} else {
		refs_set_head_detached(&r->refs, &s.orig);
		refs_reflog_head(&r->refs, NULL, detached ? &null_oid : &old,
				 &s.orig, msg);
	}

	checkout_tree(r, &orig.tree, 1, 1);
	seq_clear(r);
	if (!strcmp(s.kind, "rebase"))
		printf("Rebase aborted.\n");
	else if (!strcmp(s.kind, "revert"))
		printf("Revert aborted.\n");
	else
		printf("Cherry-pick aborted.\n");
	commit_release(&orig);
	free(msg);
	seq_release(&s);
	return 0;
}

/* ------------------------------------------------------------------ */
/* cherry-pick                                                         */

int cmd_cherry_pick(struct repo *r, int argc, char **argv)
{
	struct opts o;
	enum merge_favor favor = MERGE_FAVOR_NONE;
	struct seq s;
	oid_t head;
	int i, rc;

	opts_init(&o, argc, argv, (const char *const[]){
		"--abort", "--continue", "--skip", "-X=", "--strategy-option=",
		NULL });
	if (merge_favor_of(&o, &favor) < 0)
		return 1;

	if (opts_flag(&o, "--abort"))
		return seq_abort(r);
	if (opts_flag(&o, "--continue"))
		return seq_continue(r, favor);
	if (opts_flag(&o, "--skip"))
		return seq_skip(r, favor);

	refuse_if_replaying(r);
	if (opts_count(&o) < 1) {
		gp_error("cherry-pick: expected a commit to pick");
		return 1;
	}
	if (require_clean(r, "cherry-pick") < 0)
		return 1;
	if (refs_head(&r->refs, &head) < 0)
		gp_die("cherry-pick: HEAD has no commits yet");

	memset(&s, 0, sizeof s);
	s.kind = xstrdup("cherry-pick");
	s.mainline = 1;
	s.orig = head;
	s.head_name = refs_head_target(&r->refs);
	for (i = 0; i < opts_count(&o); i++) {
		oid_t c;

		if (resolve_rev(r, opts_arg(&o, i), &c) < 0)
			gp_die("cherry-pick: unknown revision: %s", opts_arg(&o, i));
		if (commit_peel(r, &c, OBJ_COMMIT, &c) < 0)
			gp_die("cherry-pick: not a commit: %s", opts_arg(&o, i));
		s.todo = xrealloc(s.todo, (s.nr + 1) * sizeof(*s.todo));
		s.todo[s.nr++] = c;
	}

	seq_save(r, &s);
	rc = seq_run(r, &s, favor);
	seq_release(&s);
	return rc;
}

/* ------------------------------------------------------------------ */
/* rebase                                                              */

struct oid_list {
	oid_t *v;
	size_t nr, alloc;
};

static void oid_list_push(struct oid_list *l, const oid_t *o)
{
	if (l->nr == l->alloc) {
		l->alloc = l->alloc ? l->alloc * 2 : 16;
		l->v = xrealloc(l->v, l->alloc * sizeof(*l->v));
	}
	l->v[l->nr++] = *o;
}

/*
 * The commits a rebase replays.  A merge commit is left out: a rebase makes a
 * line, and a merge's whole content is that two lines met -- which the commits
 * it joined already say, since they are in the list too when the base does not
 * reach them.  git leaves merges out for the same reason, and says so only
 * under --rebase-merges, which replays a merge as a merge.
 */
static void rebase_collect(const oid_t *oid, const struct commit *c, void *ud)
{
	if (c->parents.nr > 1)
		return;
	oid_list_push(ud, oid);
}

/* put the branch back on the commit the branch was already on */
static void fast_forward(struct repo *r, const oid_t *head, const oid_t *to,
			 const oid_t *to_tree, const char *name)
{
	char *ref = refs_head_target(&r->refs);
	char *msg = xstrfmt("rebase: fast-forward to %s", name);

	if (ref) {
		refs_write(&r->refs, ref, to);
		refs_reflog(&r->refs, ref, head, to, msg);
		refs_reflog_head(&r->refs, ref, head, to, msg);
		free(ref);
	} else {
		refs_set_head_detached(&r->refs, to);
		refs_reflog_head(&r->refs, NULL, head, to, msg);
	}
	free(msg);
	checkout_tree(r, to_tree, 1, 1);
}

int cmd_rebase(struct repo *r, int argc, char **argv)
{
	struct opts o;
	enum merge_favor favor = MERGE_FAVOR_NONE;
	struct seq s;
	struct commit uc = COMMIT_INIT;
	struct oid_array bases = OID_ARRAY_INIT;
	struct rev_list rl;
	struct oid_list picks = { NULL, 0, 0 };
	oid_t head, up, up_tree, base;
	char *label;
	size_t i;
	int rc;

	opts_init(&o, argc, argv, (const char *const[]){
		"--abort", "--continue", "--skip", "-X=", "--strategy-option=",
		NULL });
	if (merge_favor_of(&o, &favor) < 0)
		return 1;

	if (opts_flag(&o, "--abort"))
		return seq_abort(r);
	if (opts_flag(&o, "--continue"))
		return seq_continue(r, favor);
	if (opts_flag(&o, "--skip"))
		return seq_skip(r, favor);

	refuse_if_replaying(r);
	if (opts_count(&o) != 1) {
		gp_error("rebase: expected the one revision to rebase onto");
		return 1;
	}
	label = xstrdup(opts_arg(&o, 0));

	if (require_clean(r, "rebase") < 0) {
		free(label);
		return 1;
	}
	if (refs_head(&r->refs, &head) < 0)
		gp_die("rebase: HEAD has no commits yet");
	if (resolve_rev(r, label, &up) < 0)
		gp_die("rebase: unknown revision: %s", label);
	if (commit_peel(r, &up, OBJ_COMMIT, &up) < 0)
		gp_die("rebase: not a commit: %s", label);
	read_commit(r, &up, &uc);
	up_tree = uc.tree;
	commit_release(&uc);

	if (oid_equal(&head, &up) || is_ancestor(r, &up, &head)) {
		char *br = head_label(r);

		printf("Current branch %s is up to date.\n", br);
		free(br);
		free(label);
		return 0;
	}

	if (is_ancestor(r, &head, &up)) {
		char *br = head_label(r);

		fast_forward(r, &head, &up, &up_tree, label);
		printf("Fast-forwarded %s to %s.\n", br, label);
		free(br);
		free(label);
		return 0;
	}

	if (merge_bases(r, &head, &up, &bases) == 0)
		gp_die("rebase: %s and %s have no common ancestor\n"
		       "hint: a rebase would have to replay the whole of one "
		       "history onto the other, which is not what it is for",
		       label, "HEAD");
	base = bases.oid[0];
	oid_array_clear(&bases);

	memset(&rl, 0, sizeof rl);
	oid_array_append(&rl.include, &head);
	oid_array_append(&rl.exclude, &base);
	rev_list_run(r, &rl, rebase_collect, &picks);
	rev_list_release(&rl);
	if (!picks.nr) {
		/* the base is HEAD's own tip: nothing is left to replay */
		char *br = head_label(r);

		printf("Current branch %s is up to date.\n", br);
		free(br);
		free(picks.v);
		free(label);
		return 0;
	}

	memset(&s, 0, sizeof s);
	s.kind = xstrdup("rebase");
	s.mainline = 1;
	s.orig = head;
	s.head_name = refs_head_target(&r->refs);
	/* rev-list answers newest first, and a replay goes the other way */
	for (i = picks.nr; i > 0; i--) {
		s.todo = xrealloc(s.todo, (s.nr + 1) * sizeof(*s.todo));
		s.todo[s.nr++] = picks.v[i - 1];
	}
	free(picks.v);
	seq_save(r, &s);

	/*
	 * The replay runs on a detached HEAD so the branch does not name a
	 * half-replayed history: until the last commit has landed, the branch is
	 * still the one that was there before, and --abort has only to check it
	 * out again.  seq_finish puts the branch on the result.
	 */
	{
		char *msg = xstrfmt("rebase (start): checkout %s", label);

		refs_set_head_detached(&r->refs, &up);
		refs_reflog_head(&r->refs, NULL, &head, &up, msg);
		free(msg);
	}
	checkout_tree(r, &up_tree, 1, 1);

	rc = seq_run(r, &s, favor);
	seq_release(&s);
	free(label);
	return rc;
}

/* ------------------------------------------------------------------ */
/* revert                                                              */

/*
 * Which parent a revert undoes against.  A merge has more than one, and
 * undoing the merge itself means going back to one of them, so git asks which
 * rather than guessing -- the guess would silently keep one side's work and
 * drop the other's.  The number is the one a reader counts, so 1 is the first
 * parent; a commit with one parent needs no answer.
 */
static int revert_mainline(const struct opts *o, const struct commit *c,
			   const oid_t *oid, int *out)
{
	char hex[GP_SHA1_HEXSZ + 1];
	const char *x = opts_value(o, "-m");

	if (!x)
		x = opts_value(o, "--mainline");
	oid_hex(oid, hex);

	if (!x) {
		if (c->parents.nr > 1) {
			gp_error("commit %s is a merge but no -m option was given", hex);
			fprintf(stderr, "hint: -m <parent number> says which side of "
					"the merge the undo goes back to\n");
			return -1;
		}
		*out = 1;
		return 0;
	}
	{
		char *end;
		long n = strtol(x, &end, 10);

		if (*end || n < 1 || (size_t)n > c->parents.nr) {
			gp_error("commit %s does not have parent %s", hex, x);
			return -1;
		}
		*out = (int)n;
	}
	return 0;
}

int cmd_revert(struct repo *r, int argc, char **argv)
{
	struct opts o;
	enum merge_favor favor = MERGE_FAVOR_NONE;
	struct seq s;
	oid_t head;
	int i, rc, mainline = 0;

	opts_init(&o, argc, argv, (const char *const[]){
		"--abort", "--continue", "--skip", "-X=", "--strategy-option=",
		"-m=", "--mainline=", NULL });
	if (merge_favor_of(&o, &favor) < 0)
		return 1;

	if (opts_flag(&o, "--abort"))
		return seq_abort(r);
	if (opts_flag(&o, "--continue"))
		return seq_continue(r, favor);
	if (opts_flag(&o, "--skip"))
		return seq_skip(r, favor);

	refuse_if_replaying(r);
	if (opts_count(&o) < 1) {
		gp_error("revert: expected a commit to revert");
		return 1;
	}
	if (require_clean(r, "revert") < 0)
		return 1;
	if (refs_head(&r->refs, &head) < 0)
		gp_die("revert: HEAD has no commits yet");

	memset(&s, 0, sizeof s);
	s.kind = xstrdup("revert");
	s.orig = head;
	s.head_name = refs_head_target(&r->refs);
	for (i = 0; i < opts_count(&o); i++) {
		struct commit c = COMMIT_INIT;
		oid_t coid;
		int ml;

		if (resolve_rev(r, opts_arg(&o, i), &coid) < 0)
			gp_die("revert: unknown revision: %s", opts_arg(&o, i));
		if (commit_peel(r, &coid, OBJ_COMMIT, &coid) < 0)
			gp_die("revert: not a commit: %s", opts_arg(&o, i));
		read_commit(r, &coid, &c);
		rc = revert_mainline(&o, &c, &coid, &ml);
		commit_release(&c);
		if (rc < 0) {
			seq_release(&s);
			return 1;
		}
		if (!i)
			mainline = ml;
		s.todo = xrealloc(s.todo, (s.nr + 1) * sizeof(*s.todo));
		s.todo[s.nr++] = coid;
	}
	/*
	 * -m is one answer for the whole invocation, as it is in git, and every
	 * commit above has now been checked against it.
	 */
	s.mainline = mainline;

	seq_save(r, &s);
	rc = seq_run(r, &s, favor);
	seq_release(&s);
	return rc;
}
