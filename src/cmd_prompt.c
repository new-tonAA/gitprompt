/*
 * cmd_prompt.c - recording prompts and sessions, and reading the history
 * back out.
 *
 * A recorded prompt is an ordinary file under the prompt directory: a
 * frontmatter block, then the text.  `prompt` writes it and stages it, so
 * `prompt` followed by `commit` is the whole workflow.  Nothing here needs
 * a new object type, which is why a gitprompt repository is still a plain
 * git repository.
 */
#include "gp.h"

#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* shared helpers                                                     */

static int read_stdin_all(struct buf *b)
{
	char chunk[65536];
	size_t n;
	buf_reset(b);
	while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0)
		buf_add(b, chunk, n);
	return 0;
}

/* the directory prompts live in, as an owned copy (repo_prompt_dir is
 * backed by a static buffer and must not be held across calls) */
static char *prompt_dir(struct repo *r)
{
	return xstrdup(repo_prompt_dir(r));
}

static char *session_dir(struct repo *r)
{
	char *d = prompt_dir(r);
	char *p = xstrfmt("%s/sessions", d);
	free(d);
	return p;
}

/* the file a session with this id always lives in */
static char *session_path(struct repo *r, const char *id)
{
	char *d = session_dir(r);
	char *p = xstrfmt("%s/%s.md", d, id);
	free(d);
	return p;
}

static char *new_prompt_path(struct repo *r, long seq, const char *body)
{
	char *d = prompt_dir(r);
	char *slug = slugify(body ? body : "", 40);
	char *p;

	if (!slug[0]) {
		free(slug);
		slug = xstrdup("prompt");
	}
	p = xstrfmt("%s/%04ld-%s.md", d, seq, slug);
	free(slug);
	free(d);
	return p;
}

/* write a file inside the work tree, creating parent directories */
static int write_worktree_file(struct repo *r, const char *relpath,
			       const void *data, size_t len)
{
	char *full, *dir, *slash;
	int rc;

	if (!r->root) {
		gp_error("this operation needs a work tree (the repository is bare)");
		return -1;
	}
	full = xstrfmt("%s/%s", r->root, relpath);
	dir = xstrdup(full);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir_p(dir);
	}
	free(dir);
	rc = write_file(full, data, len);
	free(full);
	return rc;
}

/* ------------------------------------------------------------------ */
/* recording a prompt                                                  */

struct prompt_input {
	const char *body;
	const char *session;
	const char *model;
	const char *parent;
	const char *const *tags;
	size_t nr_tags;
};

static int record_prompt(struct repo *r, const struct prompt_input *in,
			 int stage_it)
{
	struct prompt p;
	struct buf file, who;
	long seq;
	char *rel;
	int rc;

	memset(&p, 0, sizeof p);
	p.id = new_prompt_id();
	p.session = in->session ? xstrdup(in->session) : repo_current_session(r);
	seq = repo_next_file_seq(r);
	p.seq = (int)seq;

	buf_init(&file);
	now_iso8601(&file);
	p.timestamp = xstrdup(buf_cstr(&file));

	buf_init(&who);
	repo_ident(r, &who);
	p.author = xstrdup(buf_cstr(&who));
	buf_release(&who);

	if (in->model)
		p.model = xstrdup(in->model);
	if (in->parent)
		p.parent_prompt = xstrdup(in->parent);
	p.tags = (char **)in->tags;
	p.nr_tags = in->nr_tags;

	/* the body always ends in a newline, so a diff of two prompts is a
	 * diff of their text and nothing else */
	{
		struct buf body;
		buf_init(&body);
		buf_addstr(&body, in->body ? in->body : "");
		if (body.len && body.b[body.len - 1] != '\n')
			buf_addch(&body, '\n');
		p.body = xstrdup(buf_cstr(&body));
		buf_release(&body);
	}

	rel = new_prompt_path(r, seq, p.body);

	buf_reset(&file);
	prompt_to_file(&p, &file);
	rc = write_worktree_file(r, rel, file.b, file.len);
	if (rc == 0) {
		repo_bump_file_seq(r, seq + 1);
		if (stage_it && stage_worktree_path(r, rel) < 0)
			gp_warn("recorded %s but could not stage it", rel);
		printf("%s %s\n", p.id, rel);
	}

	free(rel);
	free(p.id);
	free(p.session);
	free(p.timestamp);
	free(p.author);
	free(p.model);
	free(p.parent_prompt);
	free(p.body);
	buf_release(&file);
	return rc < 0 ? 1 : 0;
}

static void collect_tags(const struct opts *o, const char ***out, size_t *nr)
{
	size_t n = 0, i;
	const char **v = xcalloc(o->nf + 1, sizeof(*v));

	for (i = 0; i < (size_t)o->nf; i++) {
		int is_tag = !strcmp(o->flags[i].name, "-t") ||
			     !strcmp(o->flags[i].name, "--tag");
		if (is_tag && o->flags[i].value)
			v[n++] = o->flags[i].value;
	}
	*out = v;
	*nr = n;
}

int cmd_prompt(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct buf body;
	struct prompt_input in;
	const char **tags;
	size_t nr_tags = 0;
	int rc;

	opts_init(&o, argc, argv, (const char *const[]){
		"-m=", "-F=", "-t=", "--tag=", "-s=", "--session=",
		"--model=", "--parent=", "--no-stage", NULL });
	memset(&in, 0, sizeof in);

	buf_init(&body);
	if (opts_value(&o, "-m")) {
		buf_addstr(&body, opts_value(&o, "-m"));
	} else if (opts_value(&o, "-F")) {
		if (read_file(opts_value(&o, "-F"), &body) < 0) {
			gp_error("prompt: cannot read %s", opts_value(&o, "-F"));
			buf_release(&body);
			return 1;
		}
	} else if (o.nargs) {
		int i;
		for (i = 0; i < o.nargs; i++)
			buf_addf(&body, "%s%s", i ? " " : "", o.args[i]);
	} else {
		gp_error("prompt: no text given\nusage: gitprompt prompt -m "
			 "\"<text>\"  (or pipe it through 'gitprompt capture')");
		buf_release(&body);
		return 1;
	}

	collect_tags(&o, &tags, &nr_tags);

	in.body = buf_cstr(&body);
	in.session = opts_value(&o, "-s") ? opts_value(&o, "-s")
					  : opts_value(&o, "--session");
	in.model = opts_value(&o, "--model");
	in.parent = opts_value(&o, "--parent");
	in.tags = tags;
	in.nr_tags = nr_tags;

	rc = record_prompt(r, &in, !opts_flag(&o, "--no-stage"));

	free(tags);
	buf_release(&body);
	return rc;
}

int cmd_capture(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct buf body;
	struct prompt_input in;
	const char **tags;
	size_t nr_tags = 0;
	int rc;

	opts_init(&o, argc, argv, (const char *const[]){
		"-t=", "--tag=", "-s=", "--session=", "--model=",
		"--parent=", "--no-stage", NULL });
	memset(&in, 0, sizeof in);

	buf_init(&body);
	read_stdin_all(&body);
	if (!body.len) {
		gp_error("capture: nothing on stdin");
		buf_release(&body);
		return 1;
	}

	collect_tags(&o, &tags, &nr_tags);

	in.body = buf_cstr(&body);
	in.session = opts_value(&o, "-s") ? opts_value(&o, "-s")
					  : opts_value(&o, "--session");
	in.model = opts_value(&o, "--model");
	in.parent = opts_value(&o, "--parent");
	in.tags = tags;
	in.nr_tags = nr_tags;

	rc = record_prompt(r, &in, !opts_flag(&o, "--no-stage"));

	free(tags);
	buf_release(&body);
	return rc;
}

/* ------------------------------------------------------------------ */
/* outcome                                                            */

static int find_prompt_path(struct repo *r, const char *id, char **out)
{
	struct prompt_list pl = PROMPT_LIST_INIT;
	size_t i;
	int found = -1;

	collect_prompts(r, &pl);
	for (i = 0; i < pl.nr; i++) {
		if (pl.e[i].prompt->id && !strcmp(pl.e[i].prompt->id, id)) {
			*out = xstrdup(pl.e[i].path);
			found = 0;
			break;
		}
	}
	prompt_list_release(&pl);
	return found;
}

/*
 * `--last` means the prompt with the highest sequence number, so the outcome
 * goes to what was just recorded without retyping its id.  The list is
 * scanned rather than taken as already ordered, because collect_prompts
 * appends uncommitted work tree files after the committed ones and those two
 * orders are not the same.
 */
static int find_last_prompt(struct repo *r, char **out_path, char **out_id)
{
	struct prompt_list pl = PROMPT_LIST_INIT;
	size_t i, best = 0;
	int found = -1;

	collect_prompts(r, &pl);
	for (i = 0; i < pl.nr; i++) {
		if (!pl.e[i].prompt->path || !pl.e[i].prompt->id)
			continue;
		if (found < 0 || pl.e[i].prompt->seq >= pl.e[best].prompt->seq) {
			best = i;
			found = 0;
		}
	}
	if (found == 0) {
		*out_path = xstrdup(pl.e[best].path);
		*out_id = xstrdup(pl.e[best].prompt->id);
	}
	prompt_list_release(&pl);
	return found;
}

int cmd_outcome(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct prompt p;
	struct buf b, text;
	char *path = NULL;
	char *id = NULL;
	int last, first_text;

	opts_init(&o, argc, argv, (const char *const[]){ "--last", NULL });
	last = opts_flag(&o, "--last");

	if (last) {
		if (!o.nargs) {
			gp_error("outcome: expected --last <text>");
			return 1;
		}
		if (find_last_prompt(r, &path, &id) < 0) {
			gp_error("outcome: no prompt recorded yet");
			return 1;
		}
		first_text = 0;
	} else {
		if (!opts_arg(&o, 0) || !opts_arg(&o, 1)) {
			gp_error("outcome: expected <prompt-id> <text>");
			return 1;
		}
		if (find_prompt_path(r, opts_arg(&o, 0), &path) < 0) {
			gp_error("outcome: no prompt with id %s", opts_arg(&o, 0));
			return 1;
		}
		id = xstrdup(opts_arg(&o, 0));
		first_text = 1;
	}

	buf_init(&text);
	{
		int i;
		for (i = first_text; i < o.nargs; i++)
			buf_addf(&text, "%s%s", i > first_text ? " " : "",
				 o.args[i]);
	}

	memset(&p, 0, sizeof p);
	buf_init(&b);
	{
		char *full = xstrfmt("%s/%s", r->root, path);
		if (read_file(full, &b) < 0) {
			gp_error("outcome: cannot read %s", path);
			free(full);
			free(path);
			buf_release(&b);
			buf_release(&text);
			return 1;
		}
		free(full);
	}
	prompt_from_file(&p, b.b, b.len);
	free(p.outcome);
	p.outcome = xstrdup(buf_cstr(&text));
	buf_reset(&b);
	prompt_to_file(&p, &b);

	if (write_worktree_file(r, path, b.b, b.len) < 0) {
		gp_error("outcome: cannot write %s", path);
		prompt_release(&p);
		buf_release(&b);
		buf_release(&text);
		free(path);
		free(id);
		return 1;
	}
	stage_worktree_path(r, path);
	printf("outcome recorded for %s\n", id);

	prompt_release(&p);
	buf_release(&b);
	buf_release(&text);
	free(path);
	free(id);
	return 0;
}

/* ------------------------------------------------------------------ */
/* session                                                            */

static int write_session(struct repo *r, const struct session *s)
{
	struct buf b;
	char *path;
	int rc;

	buf_init(&b);
	session_to_file(s, &b);
	path = session_path(r, s->id);
	rc = write_worktree_file(r, path, b.b, b.len);
	if (rc == 0) {
		stage_worktree_path(r, path);
		printf("%s %s\n", s->id, path);
	}
	free(path);
	buf_release(&b);
	return rc < 0 ? 1 : 0;
}

static int session_start(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct session s;
	struct buf b, who;

	opts_init(&o, argc, argv, (const char *const[]){
		"-t=", "--title=", NULL });
	memset(&s, 0, sizeof s);

	s.id = new_session_id();
	{
		const char *title = opts_value(&o, "-t");
		if (!title)
			title = opts_value(&o, "--title");
		if (!title)
			title = opts_arg(&o, 0);
		s.title = xstrdup(title ? title : "untitled session");
	}
	buf_init(&b);
	now_iso8601(&b);
	s.started_at = xstrdup(buf_cstr(&b));
	buf_init(&who);
	repo_ident(r, &who);
	s.author = xstrdup(buf_cstr(&who));
	buf_release(&who);

	{
		int rc;
		repo_set_current_session(r, s.id);
		rc = write_session(r, &s);
		if (rc == 0)
			printf("session %s started\n", s.id);
		free(s.id);
		free(s.title);
		free(s.started_at);
		free(s.author);
		buf_release(&b);
		return rc;
	}
}

static int session_end(struct repo *r)
{
	char *id = repo_current_session(r);
	char *path;
	struct buf b;
	struct session s;

	if (!id) {
		gp_error("session end: no session is current");
		return 1;
	}

	memset(&s, 0, sizeof s);
	path = session_path(r, id);
	buf_init(&b);
	{
		char *full = xstrfmt("%s/%s", r->root, path);
		int rc = read_file(full, &b);
		free(full);
		if (rc < 0) {
			gp_error("session end: cannot read %s", path);
			free(id);
			free(path);
			buf_release(&b);
			return 1;
		}
	}
	session_from_file(&s, b.b, b.len);
	if (!s.id)
		s.id = xstrdup(id);

	{
		struct buf when;
		buf_init(&when);
		now_iso8601(&when);
		free(s.ended_at);
		s.ended_at = xstrdup(buf_cstr(&when));
		buf_release(&when);
	}

	buf_reset(&b);
	session_to_file(&s, &b);
	{
		int rc = write_worktree_file(r, path, b.b, b.len);
		if (rc == 0) {
			stage_worktree_path(r, path);
			repo_set_current_session(r, NULL);
			printf("session %s ended\n", id);
		}
		session_release(&s);
		buf_release(&b);
		free(path);
		free(id);
		return rc < 0 ? 1 : 0;
	}
}

static void load_groups(struct repo *r, struct prompt_list *pl,
			struct session_groups *sg)
{
	memset(pl, 0, sizeof *pl);
	memset(sg, 0, sizeof *sg);
	collect_prompts(r, pl);
	group_by_session(r, pl, sg);
}

/* one line per session; `session list` and `replay --list-sessions` both
 * print this, so the two never drift apart */
static void print_session_groups(struct repo *r, const struct session_groups *sg)
{
	size_t i;
	char *cur = repo_current_session(r);

	for (i = 0; i < sg->nr; i++) {
		const struct session_group *g = &sg->g[i];
		if (g->session) {
			printf("%s%s  %-32s %s  %lu prompt(s)\n",
			       cur && !strcmp(cur, g->session->id) ? "* " : "  ",
			       g->session->id,
			       g->session->title ? g->session->title : "",
			       g->session->started_at ? g->session->started_at
						      : "",
			       (unsigned long)g->prompts.nr);
		} else {
			printf("    (unattributed)%*s%lu prompt(s)\n", 40, "",
			       (unsigned long)g->prompts.nr);
		}
	}
	if (!sg->nr)
		printf("no sessions recorded yet\n");
	free(cur);
}

static int session_list(struct repo *r)
{
	struct prompt_list pl;
	struct session_groups sg;

	load_groups(r, &pl, &sg);
	print_session_groups(r, &sg);
	session_groups_release(&sg);
	prompt_list_release(&pl);
	return 0;
}

static int session_show(struct repo *r, const char *id)
{
	struct prompt_list pl;
	struct session_groups sg;
	size_t i, j;
	int found = 0;

	load_groups(r, &pl, &sg);
	for (i = 0; i < sg.nr; i++) {
		const struct session_group *g = &sg.g[i];
		if (!g->session || strcmp(g->session->id, id))
			continue;
		found = 1;
		printf("session %s\n", g->session->id);
		printf("  title:   %s\n", g->session->title ? g->session->title : "");
		printf("  started: %s\n", g->session->started_at
						 ? g->session->started_at : "");
		if (g->session->ended_at)
			printf("  ended:   %s\n", g->session->ended_at);
		printf("  author:  %s\n", g->session->author ? g->session->author : "");
		if (g->session->notes)
			printf("  notes:   %s\n", g->session->notes);
		printf("  prompts: %lu\n\n", (unsigned long)g->prompts.nr);
		for (j = 0; j < g->prompts.nr; j++) {
			const struct prompt *p = g->prompts.e[j].prompt;
			const char *b = p->body ? p->body : "";
			size_t len = strcspn(b, "\n");
			printf("  %2lu. %s  %s\n     %.*s\n",
			       (unsigned long)(j + 1),
			       p->timestamp ? p->timestamp : "", p->id, (int)len,
			       b);
			if (p->outcome)
				printf("     outcome: %s\n", p->outcome);
		}
	}

	session_groups_release(&sg);
	prompt_list_release(&pl);
	if (!found) {
		gp_error("session: no session with id %s", id);
		return 1;
	}
	return 0;
}

int cmd_session(struct repo *r, int argc, char **argv)
{
	const char *sub = argc > 0 ? argv[0] : NULL;

	if (!sub)
		gp_die("session: expected a subcommand\nusage: gitprompt session "
		       "<start|end|list|show|use|current> [...]");

	if (!strcmp(sub, "start"))
		return session_start(r, argc - 1, argv + 1);
	if (!strcmp(sub, "end"))
		return session_end(r);
	if (!strcmp(sub, "list"))
		return session_list(r);
	if (!strcmp(sub, "show")) {
		char *cur;
		int rc;
		if (argc >= 2)
			return session_show(r, argv[1]);
		/* no id means the session being recorded into, which is the one
		 * a caller doing `session show` mid-work means by it */
		cur = repo_current_session(r);
		if (!cur)
			gp_die("session show: no session is open");
		rc = session_show(r, cur);
		free(cur);
		return rc;
	}
	if (!strcmp(sub, "use")) {
		if (argc < 2)
			gp_die("session use: expected a session id");
		if (repo_set_current_session(r, argv[1]) < 0)
			gp_die("session use: cannot write the session pointer");
		printf("now recording into %s\n", argv[1]);
		return 0;
	}
	if (!strcmp(sub, "current")) {
		char *id = repo_current_session(r);
		if (!id) {
			printf("(no session)\n");
			return 1;
		}
		printf("%s\n", id);
		free(id);
		return 0;
	}
	gp_die("session: unknown subcommand '%s'", sub);
	return 1;
}

/* ------------------------------------------------------------------ */
/* replay / timeline / log-prompt                                     */

int cmd_replay(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct prompt_list pl;
	struct session_groups sg;
	struct buf out;
	const char *fmt;
	int stat_only;

	opts_init(&o, argc, argv, (const char *const[]){
		"--format=", "-o=", "--output=", "--layout=", "--stat",
		"--list-sessions", NULL });
	fmt = opts_value(&o, "--format");
	if (!fmt)
		fmt = "md";
	stat_only = opts_flag(&o, "--stat");

	/*
	 * `replay [<ref>]` replays that point in the history, not whatever HEAD
	 * happens to be.  An unnamed ref means the current one; a named one that
	 * does not resolve is an error rather than a silent replay of HEAD,
	 * which is what collect_prompts_from_ref reports.
	 */
	if (o.nargs > 0) {
		memset(&pl, 0, sizeof pl);
		memset(&sg, 0, sizeof sg);
		collect_prompts_from_ref(r, o.args[0], &pl);
		group_by_session(r, &pl, &sg);
	} else {
		load_groups(r, &pl, &sg);
	}

	/* the listing stands in for the document, so it prints and stops */
	if (opts_flag(&o, "--list-sessions")) {
		print_session_groups(r, &sg);
		session_groups_release(&sg);
		prompt_list_release(&pl);
		return 0;
	}

	buf_init(&out);
	if (!strcmp(fmt, "md") || !strcmp(fmt, "markdown")) {
		replay_markdown(r, &sg, &out, stat_only);
	} else if (!strcmp(fmt, "json")) {
		replay_json(r, &sg, &out);
	} else if (!strcmp(fmt, "txt") || !strcmp(fmt, "text")) {
		replay_text(r, &sg, &out);
	} else {
		gp_die("replay: unknown format '%s' (md, json or txt)", fmt);
	}

	{
		const char *layout = opts_value(&o, "--layout");
		if (layout)
			replay_layout(r, &sg, layout);
	}

	{
		const char *path = opts_value(&o, "-o")
					   ? opts_value(&o, "-o")
					   : opts_value(&o, "--output");
		if (path) {
			if (write_file(path, out.b, out.len) < 0)
				gp_die("replay: cannot write %s", path);
			printf("wrote %s (%lu bytes)\n", path,
			       (unsigned long)out.len);
		} else {
			fwrite(out.b, 1, out.len, stdout);
		}
	}

	buf_release(&out);
	session_groups_release(&sg);
	prompt_list_release(&pl);
	(void)argc;
	return 0;
}

int cmd_timeline(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct prompt_list pl;
	struct session_groups sg;
	size_t i, j;

	opts_init(&o, argc, argv, NULL);
	/* an optional ref, as replay has: the history as it stood then */
	if (o.nargs > 0) {
		memset(&pl, 0, sizeof pl);
		memset(&sg, 0, sizeof sg);
		collect_prompts_from_ref(r, o.args[0], &pl);
		group_by_session(r, &pl, &sg);
	} else {
		load_groups(r, &pl, &sg);
	}

	for (i = 0; i < sg.nr; i++) {
		const struct session_group *g = &sg.g[i];
		if (g->session)
			printf("[%s] %s\n", g->session->id,
			       g->session->title ? g->session->title : "");
		else
			printf("[unattributed]\n");
		for (j = 0; j < g->prompts.nr; j++) {
			const struct prompt *p = g->prompts.e[j].prompt;
			const char *b = p->body ? p->body : "";
			size_t len = strcspn(b, "\n");
			printf("  %s  %s  %.*s\n",
			       p->timestamp ? p->timestamp : "(no time)",
			       p->id, (int)len, b);
		}
		printf("\n");
	}
	if (!sg.nr)
		printf("no prompts recorded yet\n");

	session_groups_release(&sg);
	prompt_list_release(&pl);
	return 0;
}

int cmd_log_prompt(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct prompt_list pl;
	size_t i;
	int oneline;

	opts_init(&o, argc, argv, (const char *const[]){ "--oneline", NULL });
	oneline = opts_flag(&o, "--oneline");

	collect_prompts(r, &pl);
	/* the list arrives in scan order; sort it into recorded order */
	for (i = 0; i + 1 < pl.nr; i++) {
		size_t k, best = i;
		for (k = i + 1; k < pl.nr; k++) {
			const struct prompt *a = pl.e[k].prompt;
			const struct prompt *b = pl.e[best].prompt;
			if (a->seq < b->seq ||
			    (a->seq == b->seq && a->ts < b->ts))
				best = k;
		}
		if (best != i) {
			struct prompt_ref tmp = pl.e[i];
			pl.e[i] = pl.e[best];
			pl.e[best] = tmp;
		}
	}

	for (i = 0; i < pl.nr; i++) {
		const struct prompt *p = pl.e[i].prompt;
		if (oneline) {
			const char *b = p->body ? p->body : "";
			printf("%s %s\n", p->id, b);
			(void)strcspn(b, "\n");
		} else {
			const char *b = p->body ? p->body : "";
			size_t len = strcspn(b, "\n");
			printf("%s  %s  %s", p->id,
			       p->timestamp ? p->timestamp : "(no time)",
			       p->session ? p->session : "(no session)");
			if (p->model)
				printf("  %s", p->model);
			printf("\n    %.*s\n", (int)len, b);
			if (p->outcome)
				printf("    outcome: %s\n", p->outcome);
		}
	}
	if (!pl.nr)
		printf("no prompts recorded yet\n");

	prompt_list_release(&pl);
	return 0;
}

/* ------------------------------------------------------------------ */
/* stats                                                              */

struct stats {
	size_t prompts;
	size_t sessions;
	size_t attributed;
	size_t with_outcome;
	size_t tagged;
	i64 first_ts, last_ts;
	size_t commits;
	size_t objects;
	size_t refs;
	size_t tags;
};

struct count_ctx {
	size_t *n;
};

static void count_ref_cb(const char *name, const oid_t *oid, void *ud)
{
	struct count_ctx *c = ud;
	(void)oid;
	(void)name;
	(*c->n)++;
}

static void count_commit_cb(const oid_t *oid, const struct commit *c, void *ud)
{
	struct count_ctx *ctx = ud;
	(void)oid;
	(void)c;
	(*ctx->n)++;
}

int cmd_stats(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct stats st;
	struct prompt_list pl;
	struct session_groups sg;
	struct count_ctx cc;
	size_t i;
	struct oid_array tips = OID_ARRAY_INIT;
	oid_t head;

	opts_init(&o, argc, argv, (const char *const[]){ "--json", NULL });
	memset(&st, 0, sizeof st);
	st.objects = odb_count(&r->odb);

	cc.n = &st.refs;
	refs_list(&r->refs, "refs/", count_ref_cb, &cc);
	refs_list_packed(&r->refs, "refs/", count_ref_cb, &cc);
	cc.n = &st.tags;
	refs_list(&r->refs, "refs/tags/", count_ref_cb, &cc);

	load_groups(r, &pl, &sg);
	st.prompts = pl.nr;
	st.sessions = sg.nr;
	for (i = 0; i < pl.nr; i++) {
		const struct prompt *p = pl.e[i].prompt;
		if (p->session && p->session[0])
			st.attributed++;
		if (p->outcome)
			st.with_outcome++;
		if (p->nr_tags)
			st.tagged++;
		if (p->ts && (!st.first_ts || p->ts < st.first_ts))
			st.first_ts = p->ts;
		if (p->ts > st.last_ts)
			st.last_ts = p->ts;
	}

	if (refs_head(&r->refs, &head) == 0) {
		struct count_ctx cbc;
		cbc.n = &st.commits;
		oid_array_append(&tips, &head);
		walk_commits(r, &tips, count_commit_cb, &cbc);
	}

	{
		struct buf a, b;
		buf_init(&a);
		buf_init(&b);
		if (st.first_ts)
			epoch_to_iso8601(st.first_ts, &a);
		if (st.last_ts)
			epoch_to_iso8601(st.last_ts, &b);

		if (opts_flag(&o, "--json")) {
			printf("{\n");
			printf("  \"prompts\": %lu,\n", (unsigned long)st.prompts);
			printf("  \"sessions\": %lu,\n", (unsigned long)st.sessions);
			printf("  \"attributed_prompts\": %lu,\n",
			       (unsigned long)st.attributed);
			printf("  \"prompts_with_outcome\": %lu,\n",
			       (unsigned long)st.with_outcome);
			printf("  \"tagged_prompts\": %lu,\n",
			       (unsigned long)st.tagged);
			printf("  \"commits\": %lu,\n", (unsigned long)st.commits);
			printf("  \"refs\": %lu,\n", (unsigned long)st.refs);
			printf("  \"tags\": %lu,\n", (unsigned long)st.tags);
			printf("  \"loose_objects\": %lu,\n",
			       (unsigned long)st.objects);
			printf("  \"first_prompt\": \"%s\",\n", buf_cstr(&a));
			printf("  \"last_prompt\": \"%s\"\n", buf_cstr(&b));
			printf("}\n");
		} else {
			printf("prompts:          %lu\n",
			       (unsigned long)st.prompts);
			printf("sessions:         %lu\n", (unsigned long)st.sessions);
			printf("  attributed:     %lu\n",
			       (unsigned long)st.attributed);
			printf("  with outcome:   %lu\n",
			       (unsigned long)st.with_outcome);
			printf("  tagged:         %lu\n",
			       (unsigned long)st.tagged);
			printf("commits:          %lu\n",
			       (unsigned long)st.commits);
			printf("refs:             %lu\n", (unsigned long)st.refs);
			printf("tags:             %lu\n", (unsigned long)st.tags);
			printf("loose objects:    %lu\n",
			       (unsigned long)st.objects);
			if (st.first_ts) {
				printf("first prompt:     %s\n", buf_cstr(&a));
				printf("last prompt:      %s\n", buf_cstr(&b));
			}
		}
		buf_release(&a);
		buf_release(&b);
	}

	oid_array_clear(&tips);
	session_groups_release(&sg);
	prompt_list_release(&pl);
	(void)argc;
	return 0;
}
