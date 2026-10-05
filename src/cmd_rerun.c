/*
 * rerun -- hand the recorded prompts back to an agent, in the order they were
 * written, so the work can be made again.
 *
 * `attach` gives an agent the history to read, in the mode where reading is all
 * it does.  This is the other half of the same idea: the same history, given to
 * an agent that is allowed to act on it, one prompt at a time.
 *
 * What makes this more than a loop over the prompt files is the conversations.
 * The prompts were said in sessions, a session was one agent context, and a task
 * moved between sessions and back -- so a faithful rerun has to move between
 * agent conversations the same way, or it collapses the very structure the
 * history was kept in order to record.  Each session is therefore mapped to one
 * agent conversation, and coming back to a session resumes the conversation it
 * was.
 *
 * It reconstructs the prompts, not the project.  The prompts are stored exactly
 * and are handed over verbatim; an agent doing the work a second time may do it
 * differently, and nothing here can promise otherwise.  Answers are stored only
 * when somebody kept them -- `response`, or `--record`, which keeps what the
 * agent prints here -- so a replay can carry the answers the history already has
 * and can add the new ones to it.  The project itself is restored exactly by
 * checking out the commit, which is what `checkout` is for; this is for the
 * other thing -- making the history go again, in front of someone who can watch
 * it happen.
 *
 * Nothing is run unless it is asked for.  A rerun starts processes that edit the
 * work tree, so at a terminal it asks which agent to replay into and then asks
 * to confirm before it begins; with no terminal to ask, it prints what would be
 * run and `--yes` is what runs it.
 *
 * The agent is asked for because a clone cannot know it: the history says what
 * was prompted and in what order, and nothing about what is installed on the
 * machine that pulled it.  The confirmation is asked for because there is
 * nothing to undo -- the agent works in the work tree, and re-running is not a
 * rollback.
 */

#include "gp.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

/*
 * The command line an agent is replayed through, as the pieces it is built
 * from.  Each is a default only: what an agent is installed as is a fact about
 * the machine, not about the history, so every piece can be replaced with
 * `gitprompt.agent.<name>.<field>` -- see `agent_cmd_resolve`.
 *
 * `resume` and `new_session` being NULL is the honest state of an agent that
 * cannot be told which conversation to use: continuation needs the id, and
 * there is no way to invent one it will accept.  Rather than run a session as a
 * string of unrelated conversations and call the result a replay, `rerun`
 * refuses, and says why.
 */
struct rerun_agent {
	const char *name;
	const char *start;            /* how a new conversation is begun */
	const char *resume;           /* flag the id follows to continue one */
	const char *new_session;      /* flag the id follows to name a new one */
	const char *model_flag;       /* flag a model name follows */
	const char *permission_flag;  /* flag a permission mode follows */
	const char *modes;            /* the values it takes, comma-separated */
	const char *mode_default;
};

static const struct rerun_agent rerun_agents[] = {
	{ "claude", "claude -p", "--resume", "--session-id", "--model",
	  "--permission-mode",
	  "acceptEdits,auto,bypassPermissions,manual,dontAsk,plan", "acceptEdits" },
	/*
	 * `codex exec` and `dsh --profile headless` each run one task and
	 * forget it: neither can be told which conversation to continue, so
	 * neither can replay a history whose sessions are interrupted.  They
	 * are listed rather than hidden because that reason is the thing
	 * worth knowing about them.
	 */
	{ "codex",  "codex exec", NULL, NULL, NULL, NULL, NULL, NULL },
	{ "dsh",    "dsh --profile headless", NULL, NULL, NULL, NULL, NULL, NULL },
};

#define RERUN_NR_AGENTS (sizeof rerun_agents / sizeof rerun_agents[0])

static const struct rerun_agent *rerun_agent_find(const char *name)
{
	size_t i;

	for (i = 0; i < RERUN_NR_AGENTS; i++)
		if (!strcmp(rerun_agents[i].name, name))
			return &rerun_agents[i];
	return NULL;
}

static void rerun_agent_names(void)
{
	size_t i;

	for (i = 0; i < RERUN_NR_AGENTS; i++)
		fprintf(stderr, "%s%s", i ? ", " : "", rerun_agents[i].name);
}

/*
 * One agent's command line after config has had its say.  Every field is owned
 * here, and a field is NULL when nothing says what it is -- which, for the
 * fields that are flags, is the same as "this agent has none".
 */
struct agent_cmd {
	char *command;
	char *resume;
	char *new_session;
	char *model_flag;
	char *permission_flag;
	char *modes;
	char *mode_default;
};

static void agent_cmd_release(struct agent_cmd *c)
{
	free(c->command);
	free(c->resume);
	free(c->new_session);
	free(c->model_flag);
	free(c->permission_flag);
	free(c->modes);
	free(c->mode_default);
}

/*
 * Fill in an agent's command line.  `row` is the shipped table's entry, or NULL
 * for an agent this repository configures and the table has never heard of: a
 * harness that arrives later is a command line, not a rebuild.  Config wins
 * wherever it speaks, and an empty value there takes a piece away rather than
 * falling back -- which is how a machine whose agent has no such flag says so.
 */
static void agent_cmd_resolve(struct repo *r, const char *name,
			      const struct rerun_agent *row, struct agent_cmd *c)
{
	size_t i;

	memset(c, 0, sizeof *c);
	c->command = repo_agent_setting(r, name, "command",
					row ? row->start : NULL);
	c->resume = repo_agent_setting(r, name, "resume",
				       row ? row->resume : NULL);
	c->new_session = repo_agent_setting(r, name, "newSession",
					    row ? row->new_session : NULL);
	c->model_flag = repo_agent_setting(r, name, "modelFlag",
					   row ? row->model_flag : NULL);
	c->permission_flag = repo_agent_setting(r, name, "permissionFlag",
						row ? row->permission_flag : NULL);
	c->modes = repo_agent_setting(r, name, "modes", row ? row->modes : NULL);
	c->mode_default = repo_agent_setting(r, name, "modeDefault",
					     row ? row->mode_default : NULL);

	/* "" and "absent" mean the same thing from here on */
	{
		char **all[] = { &c->command, &c->resume, &c->new_session,
				 &c->model_flag, &c->permission_flag, &c->modes,
				 &c->mode_default };

		for (i = 0; i < sizeof all / sizeof all[0]; i++)
			if (*all[i] && !**all[i]) {
				free(*all[i]);
				*all[i] = NULL;
			}
	}
}

/*
 * Whether config says how this machine runs the agent, which is what makes an
 * agent the shipped table has never heard of usable.
 */
static int agent_configured(struct repo *r, const char *name)
{
	char *v = repo_agent_setting(r, name, "command", NULL);
	int yes = v && *v;

	free(v);
	return yes;
}

/* Whether `mode` is one of a comma-separated list. */
static int mode_allowed(const char *modes, const char *mode)
{
	const char *p = modes;
	size_t n = strlen(mode);

	while (*p) {
		const char *e = strchr(p, ',');

		if (!e)
			e = p + strlen(p);
		if ((size_t)(e - p) == n && !strncmp(p, mode, n))
			return 1;
		if (!*e)
			break;
		p = e + 1;
	}
	return 0;
}

/* The list, as the error message prints it. */
static void mode_list(const char *modes)
{
	const char *p = modes;

	while (*p) {
		const char *e = strchr(p, ',');

		if (!e)
			e = p + strlen(p);
		fprintf(stderr, "%.*s%s", (int)(e - p), p, *e ? ", " : "");
		if (!*e)
			break;
		p = e + 1;
	}
}

/*
 * Whether there is somebody at the terminal to ask.  Both ends are checked, not
 * just the input: a run whose output is going into a pipe is being read by
 * another program, and a question printed there is a question nobody sees --
 * while a read that waits for an answer would stop a script that only wanted the
 * list.  Printing the plan and letting it stand is what that case already does.
 */
static int rerun_interactive(void)
{
#ifdef _WIN32
	/* the name the runtime really implements, rather than whatever the
	 * C library happens to have aliased it to */
	return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
	return isatty(0) && isatty(1);
#endif
}

/* One line from the terminal, or NULL at end of input.  Stdio is in binary
 * mode, so a carriage return is stripped here rather than by the runtime. */
static char *rerun_read_line(void)
{
	struct buf b;
	char *s;
	int ch;

	buf_init(&b);
	while ((ch = fgetc(stdin)) != EOF && ch != '\n')
		if (ch != '\r')
			buf_addch(&b, (char)ch);
	if (ch == EOF && !b.len) {
		buf_release(&b);
		return NULL;
	}
	s = b.len ? xstrdup(buf_cstr(&b)) : xstrdup("");
	buf_release(&b);
	return s;
}

/*
 * Which agent to replay into is a question about this machine, not about the
 * history: the prompts say what was asked and in what order, and nothing about
 * what is installed here to ask again.  So a clone does not carry the answer and
 * the person at the terminal gives it.
 *
 * An agent that cannot be told which conversation to continue is listed and
 * refused rather than hidden, because the reason it is refused -- a session
 * would become a string of unrelated conversations -- is the thing a reader of
 * this command most needs to know about it.
 */
static const struct rerun_agent *rerun_ask_agent(void)
{
	for (;;) {
		const struct rerun_agent *a;
		char *line, *end;
		unsigned long pick;
		size_t i;

		printf("Which agent should replay this history?\n");
		for (i = 0; i < RERUN_NR_AGENTS; i++) {
			const struct rerun_agent *x = &rerun_agents[i];

			printf("  %lu. %-8s %s\n", (unsigned long)(i + 1), x->name,
			       x->resume ? x->start
					 : "-- cannot resume a session, so a replay "
					   "into it would not be one");
		}
		printf("Pick one [1]: ");
		fflush(stdout);
		line = rerun_read_line();
		if (!line)
			gp_die("rerun: no agent chosen");
		end = line + strspn(line, " \t");
		pick = strtoul(end, NULL, 10);
		if (!*end)
			pick = 1;
		if (pick < 1 || pick > RERUN_NR_AGENTS) {
			printf("'%s' is not one of them.\n\n", line);
			free(line);
			continue;
		}
		a = &rerun_agents[pick - 1];
		free(line);
		if (!a->resume) {
			printf("\n%s cannot be told which conversation to continue, so a\n"
			       "session it began could not be returned to, and a rerun\n"
			       "that ran each session as a string of unrelated\n"
			       "conversations would not be a replay of this history.\n\n",
			       a->name);
			continue;
		}
		printf("\n");
		return a;
	}
}

/*
 * Run one prompt's command.  With no `answer` this is `system`, and the agent
 * writes to the terminal itself.  With one it is `popen`, so the agent's output
 * comes back to us: printed through unchanged, and kept, which is what makes the
 * answer a thing that can be recorded.
 *
 * `popen` is safe here for the same reason the redirect is: the shell does the
 * work, and `< file` is applied whether or not stdout was redirected, so the
 * prompt reaches the agent on its standard input either way.  Returns 0 on
 * success, and anything else -- a shell status on Unix -- on failure.
 */
static int rerun_run(const char *cmd, struct buf *answer)
{
	FILE *f;
	char chunk[4096];
	size_t n;
	int rc;

	if (!answer)
		return system(cmd);

	f = popen(cmd, "r");
	if (!f)
		return -1;
	while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
		fwrite(chunk, 1, n, stdout);
		buf_add(answer, chunk, n);
	}
	rc = pclose(f);
	return rc;
}

/*
 * The agent conversation a session is replayed into.
 *
 * Derived from the session id rather than handed out at random, because the same
 * history replayed twice should reach the same conversations: a run that stops
 * half way can then be started again from where it stopped and the agent still
 * knows what came before.  `--salt` is what makes a genuinely fresh set when
 * that is what is wanted.
 *
 * An agent that resumes by id wants a UUID, which is what this is shaped like:
 * sixteen bytes of a hash, with the version and variant bits set, so that it is
 * a well-formed v4 UUID and not merely the right length.  The salt and the key
 * are separated by a NUL so that no pair of them can run together into the same
 * seed as another pair.
 */
static char *rerun_uuid(const char *salt, const char *key)
{
	struct buf seed;
	u8 d[GP_SHA1_RAWSZ];

	buf_init(&seed);
	buf_add(&seed, "gitprompt rerun", 14);
	buf_addch(&seed, '\0');
	buf_addstr(&seed, salt ? salt : "");
	buf_addch(&seed, '\0');
	buf_addstr(&seed, key);
	gp_sha1(seed.b, seed.len, d);
	buf_release(&seed);

	d[6] = (u8)((d[6] & 0x0f) | 0x40);
	d[8] = (u8)((d[8] & 0x3f) | 0x80);

	return xstrfmt("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
		       "%02x%02x%02x%02x%02x%02x",
		       d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
		       d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
}

/* one conversation per session, remembered for the length of the run */
struct rerun_conv {
	char *key;              /* the session id, or the prompt id when there is none */
	char *uuid;
	int started;            /* has this conversation been begun in this run? */
};

static struct rerun_conv *rerun_conv_for(struct rerun_conv **convs, size_t *nr,
					 const char *key, const char *salt)
{
	size_t i;
	struct rerun_conv *c;

	for (i = 0; i < *nr; i++)
		if (!strcmp((*convs)[i].key, key))
			return &(*convs)[i];

	*convs = xrealloc(*convs, (*nr + 1) * sizeof(**convs));
	c = &(*convs)[*nr];
	c->key = xstrdup(key);
	c->uuid = rerun_uuid(salt, key);
	c->started = 0;
	(*nr)++;
	return c;
}

static void rerun_convs_release(struct rerun_conv *c, size_t nr)
{
	size_t i;

	for (i = 0; i < nr; i++) {
		free(c[i].key);
		free(c[i].uuid);
	}
	free(c);
}

/*
 * A path the shell will read back as a single word.  Doubled quotes are enough
 * for spaces, which a store path may well have, but cmd.exe reads `%` even
 * inside them and a quote ends the run early, so both are refused rather than
 * guessed at.
 */
static int rerun_quote_path(const char *path, struct buf *out)
{
	if (strpbrk(path, "\"%\r\n")) {
		gp_error("rerun: cannot pass the path '%s' to the shell", path);
		return -1;
	}
	buf_addch(out, '"');
	buf_addstr(out, path);
	buf_addch(out, '"');
	return 0;
}

/*
 * Build the command for one prompt.  The agent is never given the prompt on its
 * command line: a prompt is arbitrary text, full of quotes, newlines and percent
 * signs as a matter of course, and text like that cannot be put where a shell
 * will read it without either breaking or being escaped, which is one mistake
 * from running something else instead.  It is written to a file and handed over
 * on standard input, which removes the question.
 */
static void rerun_build(struct buf *cmd, const struct agent_cmd *agent,
			const char *model, const char *mode, const char *uuid,
			int resume)
{
	buf_reset(cmd);
	buf_addstr(cmd, agent->command);
	if (model)
		buf_addf(cmd, " %s %s", agent->model_flag, model);
	if (mode)
		buf_addf(cmd, " %s %s", agent->permission_flag, mode);
	if (!uuid)
		return;
	if (resume)
		buf_addf(cmd, " %s %s", agent->resume, uuid);
	else
		buf_addf(cmd, " %s %s", agent->new_session, uuid);
}

/*
 * The program an agent's command line starts with.  The line is read by a shell,
 * so only the first word names it -- and a word may be quoted, which is how a
 * command points at a path with a space in it.  NULL when there is no word at
 * all, which is a command line that could not run anyway.
 */
static char *command_program(const char *command)
{
	const char *p = command;
	struct buf b;
	char *s;

	while (*p == ' ' || *p == '\t')
		p++;
	buf_init(&b);
	if (*p == '"')
		for (p++; *p && *p != '"'; p++)
			buf_addch(&b, *p);
	else
		for (; *p && *p != ' ' && *p != '\t'; p++)
			buf_addch(&b, *p);
	if (!b.len) {
		buf_release(&b);
		return NULL;
	}
	s = xstrdup(buf_cstr(&b));
	buf_release(&b);
	return s;
}

/*
 * Whether a shell would find this program.  The question is put to a shell
 * rather than answered by walking PATH here, because a shell is what will run
 * the command line and its answer is the one that matters: on Windows that is
 * cmd.exe, where PATHEXT and the working directory count, and on Unix it is
 * /bin/sh.  A name that cannot even be asked about is reported as found, so an
 * odd quoting is a clear failure from the shell rather than a refusal here.
 */
static int program_found(const char *program)
{
	struct buf probe;
	int rc;

	buf_init(&probe);
#ifdef _WIN32
	buf_addstr(&probe, "where ");
#else
	/* `command -v` is the POSIX spelling of the question */
	buf_addstr(&probe, "command -v ");
#endif
	if (rerun_quote_path(program, &probe) < 0) {
		buf_release(&probe);
		return 1;
	}
#ifdef _WIN32
	buf_addstr(&probe, " > NUL 2> NUL");
#else
	buf_addstr(&probe, " > /dev/null 2>&1");
#endif
	rc = system(buf_cstr(&probe));
	buf_release(&probe);
	return rc == 0;
}

int cmd_rerun(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct prompt_list pl;
	struct session_groups sg;
	struct prompt_ref *order;
	struct buf cmd, answer;
	struct rerun_conv *convs = NULL;
	struct agent_cmd agent;
	size_t nr_convs = 0, i, n = 0, selected = 0;
	const struct rerun_agent *row;
	const char *name, *model, *mode, *salt, *from, *only;
	char *msg_path, *prog = NULL;
	int run, record, interactive;

	opts_init(&o, argc, argv, (const char *const[]){
		"--agent=", "--model=", "--permission-mode=", "--salt=",
		"--from=", "--only-session=", "--yes", "-y", "--record", NULL });

	name = opts_value(&o, "--agent");
	interactive = rerun_interactive();
	if (name) {
		row = rerun_agent_find(name);
		/*
		 * An agent the table has never heard of is still usable when
		 * the repository says how this machine runs it -- a harness
		 * that arrives later is a command line, not a rebuild.
		 */
		if (!row && !agent_configured(r, name)) {
			fprintf(stderr, "rerun: agent must be one of ");
			rerun_agent_names();
			fprintf(stderr,
				"\n        or one this repository configures, with\n"
				"        gitprompt.agent.<name>.command\n");
			exit(2);
		}
	} else if (interactive) {
		row = rerun_ask_agent();
		name = row->name;
	} else {
		name = "claude";
		row = rerun_agent_find(name);
	}
	agent_cmd_resolve(r, name, row, &agent);
	if (!agent.command)
		gp_die("rerun: nothing says how this machine runs %s\n"
		       "        set gitprompt.agent.%s.command", name, name);

	if (!agent.resume || !agent.new_session)
		gp_die("rerun: %s cannot be told which conversation to continue, so a\n"
		       "        session it began could not be returned to; a rerun\n"
		       "        that ran each session as a string of unrelated\n"
		       "        conversations would not be a replay of this history\n"
		       "        (gitprompt.agent.%s.resume and .newSession say how a\n"
		       "        conversation is named; empty means it cannot be)",
		       name, name);

	mode = opts_value(&o, "--permission-mode");
	if (!agent.permission_flag) {
		if (mode)
			gp_die("rerun: %s has no permission mode to set\n"
			       "        gitprompt.agent.%s.permissionFlag is empty",
			       name, name);
	} else {
		if (!mode)
			mode = agent.mode_default;
		if (mode && agent.modes && !mode_allowed(agent.modes, mode)) {
			fprintf(stderr, "rerun: --permission-mode must be one of ");
			mode_list(agent.modes);
			fprintf(stderr, "\n");
			exit(2);
		}
	}

	/*
	 * A model name reaches the shell as part of the command line, so it is
	 * held to what a model name can be.  It comes from a fixed set of
	 * vendors, not from free text, and refusing anything else costs nothing.
	 */
	model = opts_value(&o, "--model");
	if (model && (!*model ||
		      strspn(model, "abcdefghijklmnopqrstuvwxyz"
				    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
				    "0123456789.-_") != strlen(model)))
		gp_die("rerun: --model takes a model name, not '%s'", model);
	if (model && !agent.model_flag)
		gp_die("rerun: %s has no model to set\n"
		       "        gitprompt.agent.%s.modelFlag is empty", name, name);

	salt = opts_value(&o, "--salt");
	from = opts_value(&o, "--from");
	only = opts_value(&o, "--only-session");
	run = opts_flag(&o, "--yes") || opts_flag(&o, "-y");
	record = opts_flag(&o, "--record");

	if (opts_count(&o) > 1)
		gp_die("rerun: at most one revision");
	memset(&pl, 0, sizeof pl);
	memset(&sg, 0, sizeof sg);
	if (opts_arg(&o, 0)) {
		collect_prompts_from_ref(r, opts_arg(&o, 0), &pl);
		group_by_session(r, &pl, &sg);
	} else {
		load_groups(r, &pl, &sg);
	}
	order = flat_order_alloc(&pl);
	msg_path = xstrfmt("%s/RERUN_MSG", r->gpdir);
	buf_init(&cmd);
	buf_init(&answer);

	if (!pl.nr) {
		printf("no prompts recorded yet\n");
		goto done;
	}

	/*
	 * What the run covers, decided before anything is printed, so that the
	 * number in the header is the number of prompts that will actually run
	 * rather than the size of the history they were picked out of.
	 */
	for (i = 0; i < pl.nr; i++) {
		const struct prompt *p = order[i].prompt;

		if (from && (!p->id || strcmp(p->id, from)))
			continue;
		if (only && (!p->session || strcmp(p->session, only)))
			continue;
		selected++;
	}
	if (!selected) {
		if (from)
			gp_die("rerun: no prompt with id %s in this history", from);
		gp_die("rerun: no prompt in session %s", only ? only : "?");
	}

	if (mode)
		printf("rerun: %lu of %lu prompt(s), agent %s, permission mode %s\n",
		       (unsigned long)selected, (unsigned long)pl.nr, name, mode);
	else
		printf("rerun: %lu of %lu prompt(s), agent %s\n",
		       (unsigned long)selected, (unsigned long)pl.nr, name);
	if (selected != pl.nr)
		printf("       from the filters given, not the whole history\n");

	/*
	 * What the run is, before it is confirmed: how many conversations it will
	 * move between, and when the prompts were said.  A session was one agent
	 * conversation, so a history that crossed sessions is replayed by crossing
	 * the same ones -- and the dates are the dates they were recorded at,
	 * offsets and all, because the run happens now, however long ago that was.
	 */
	{
		const char **seen = NULL;
		const char *first = NULL, *last = NULL;
		i64 first_v = 0, last_v = 0;
		size_t convs = 0, nr_seen = 0, j, k;

		for (j = 0; j < pl.nr; j++) {
			const struct prompt *p = order[j].prompt;

			if (from && (!p->id || strcmp(p->id, from)))
				continue;
			if (only && (!p->session || strcmp(p->session, only)))
				continue;
			if (!p->session) {
				/* said outside any conversation, so it is one */
				convs++;
			} else {
				for (k = 0; k < nr_seen; k++)
					if (!strcmp(seen[k], p->session))
						break;
				if (k == nr_seen) {
					seen = xrealloc(seen,
						(nr_seen + 1) * sizeof(*seen));
					seen[nr_seen++] = p->session;
					convs++;
				}
			}
			if (p->timestamp) {
				if (!first || p->ts < first_v) {
					first = p->timestamp;
					first_v = p->ts;
				}
				if (!last || p->ts > last_v) {
					last = p->timestamp;
					last_v = p->ts;
				}
			}
		}
		free(seen);

		printf("       %lu conversation(s): one per session, and a session\n"
		       "         returned to is resumed rather than begun again,\n"
		       "         so the replay crosses them as the history did\n",
		       (unsigned long)convs);
		if (first && last && strcmp(first, last))
			printf("       recorded %s .. %s\n", first, last);
		else if (first)
			printf("       recorded %s\n", first);
		printf("       replayed in seq order, which is the order they were\n"
		       "         written -- the one thing a clock cannot say for "
		       "them\n");
	}

	if (r->root) {
		char *root = repo_root_display(r);

		printf("       the agent works in %s and may change it\n", root);
		free(root);
	}
	if (!run && !interactive)
		printf("       dry run -- nothing will be run; pass --yes to run "
		       "it\n");
	if (record)
		printf("       each answer will be recorded against its prompt%s\n",
		       run ? "" : " when run");
	printf("\n");

	/*
	 * A run edits the work tree as it goes, so it is confirmed before it
	 * starts.  The answer defaults to no: a bare return on a command that
	 * changes files should do nothing, and `--yes` is there for when there is
	 * nobody to ask.
	 */
	if (!run && interactive) {
		char *line;

		printf("Start the replay now? [y/N] ");
		fflush(stdout);
		line = rerun_read_line();
		if (line && (*line == 'y' || *line == 'Y'))
			run = 1;
		free(line);
		printf("\n");
		if (!run)
			printf("not started; nothing was run\n\n");
	}

	/*
	 * Whether this machine has the agent at all is the one thing about the
	 * run that the history cannot say, and it is asked now rather than
	 * discovered by the first prompt: an agent that is not here should be
	 * one sentence, not a run that stopped half way with the rest of the
	 * prompts still to go.
	 */
	prog = command_program(agent.command);
	if (run && !program_found(prog ? prog : agent.command))
		gp_die("rerun: cannot run %s -- %s is not on PATH\n"
		       "        set gitprompt.agent.%s.command to how this "
		       "machine runs it", name, prog ? prog : agent.command, name);

	for (i = 0; i < pl.nr; i++) {
		const struct prompt *p = order[i].prompt;
		struct rerun_conv *c;
		const char *sid;
		int resume, rc;

		if (from && (!p->id || strcmp(p->id, from)))
			continue;
		if (only && (!p->session || strcmp(p->session, only)))
			continue;

		/*
		 * A prompt with no session was recorded outside any
		 * conversation, so it is not part of one: it gets a conversation
		 * of its own, keyed by the prompt, and is always begun.
		 */
		sid = p->session;
		c = rerun_conv_for(&convs, &nr_convs,
				   sid ? sid : (p->id ? p->id : "?"), salt);
		resume = c->started;

		n++;
		printf("%-6s %lu  %-11s %-24s %s\n",
		       run ? "run" : "would", (unsigned long)n,
		       p->id ? p->id : "?",
		       sid ? sid : "(no session)",
		       resume ? "resume" : "start");

		rerun_build(&cmd, &agent, model, mode, c->uuid, resume);
		if (!run) {
			printf("         %s\n", buf_cstr(&cmd));
			c->started = 1;
			continue;
		}

		if (!p->body)
			gp_die("rerun: prompt %s has no text",
			       p->id ? p->id : "?");
		if (write_file(msg_path, p->body, strlen(p->body)) < 0)
			gp_die("rerun: cannot write %s", msg_path);

		buf_addstr(&cmd, " < ");
		if (rerun_quote_path(msg_path, &cmd) < 0) {
			remove(msg_path);
			gp_die("rerun: cannot pass the prompt to the agent");
		}

		c->started = 1;
		/*
		 * The agent writes to the same terminal, so our line has to be
		 * out before it starts, or the transcript reads as though the
		 * answer came before the question.
		 */
		fflush(stdout);
		buf_reset(&answer);
		rc = rerun_run(buf_cstr(&cmd), record ? &answer : NULL);
		if (rc != 0) {
			char *id = xstrdup(p->id ? p->id : "?");

			remove(msg_path);
			buf_release(&cmd);
			buf_release(&answer);
			free(msg_path);
			free(order);
			rerun_convs_release(convs, nr_convs);
			session_groups_release(&sg);
			prompt_list_release(&pl);
			gp_die("rerun: %s failed on %s (exit %d) -- is %s on "
			       "PATH?\n"
			       "        %lu prompt(s) before it were done; "
			       "`rerun --from %s --yes` starts again there",
			       name, id, rc, prog ? prog : name,
			       (unsigned long)(n - 1), id);
		}

		/*
		 * The answer is recorded only now that the run of it has
		 * finished, so a half-written one is never kept.  An empty
		 * answer is no answer: an agent that said nothing has nothing
		 * to record, and recording an empty body would make it look as
		 * though it had replied.
		 */
		if (record && answer.len) {
			if (record_response(r, p->id, buf_cstr(&answer),
					    model ? model : name, NULL, 1) != 0)
				gp_warn("rerun: could not record the answer to "
					"%s", p->id ? p->id : "?");
		}
	}

	if (run)
		remove(msg_path);
	printf("\nrerun: %lu prompt(s) across %lu conversation(s)%s\n",
	       (unsigned long)n, (unsigned long)nr_convs,
	       run ? "" : ", nothing run");

done:
	buf_release(&cmd);
	buf_release(&answer);
	free(msg_path);
	free(prog);
	free(order);
	agent_cmd_release(&agent);
	rerun_convs_release(convs, nr_convs);
	session_groups_release(&sg);
	prompt_list_release(&pl);
	return 0;
}
