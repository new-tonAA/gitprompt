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
 * and are handed over verbatim; what the agent answered is not stored at all, so
 * an agent doing the work a second time may do it differently, and nothing here
 * can promise otherwise.  The project itself is restored exactly by checking out
 * the commit, which is what `checkout` is for; this is for the other thing --
 * making the history go again, in front of someone who can watch it happen.
 *
 * Nothing is run unless it is asked for.  A rerun starts processes that edit the
 * work tree, so the default is to print what would be run and `--yes` is what
 * runs it.
 */

#include "gp.h"

/*
 * How to begin a conversation with an agent and how to continue one.  `start`
 * and `resume` are command prefixes; the conversation id is appended to
 * whichever applies.
 *
 * `resume` being NULL is the honest state of an agent that cannot be told which
 * conversation to use: continuation needs the id, and there is no way to invent
 * one it will accept.  Rather than run a session as a string of unrelated
 * conversations and call the result a replay, `rerun` refuses, and says why.
 */
struct rerun_agent {
	const char *name;
	const char *start;
	const char *resume;             /* or NULL when the id cannot be chosen */
	const char *const *modes;       /* allowed values of --permission-mode */
	const char *mode_default;
};

static const char *const claude_modes[] = {
	"acceptEdits", "auto", "bypassPermissions", "manual", "dontAsk", "plan",
	NULL
};

static const struct rerun_agent rerun_agents[] = {
	{ "claude", "claude -p", "--resume", claude_modes, "acceptEdits" },
	{ "codex",  "codex exec", NULL, NULL, NULL },
};

static const struct rerun_agent *rerun_agent_find(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof rerun_agents / sizeof rerun_agents[0]; i++)
		if (!strcmp(rerun_agents[i].name, name))
			return &rerun_agents[i];
	return NULL;
}

static void rerun_agent_names(void)
{
	size_t i;

	for (i = 0; i < sizeof rerun_agents / sizeof rerun_agents[0]; i++)
		fprintf(stderr, "%s%s", i ? ", " : "", rerun_agents[i].name);
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
static void rerun_build(struct buf *cmd, const struct rerun_agent *agent,
			const char *model, const char *mode, const char *uuid,
			int resume)
{
	buf_reset(cmd);
	buf_addstr(cmd, agent->start);
	if (model)
		buf_addf(cmd, " --model %s", model);
	buf_addf(cmd, " --permission-mode %s", mode);
	if (!uuid)
		return;
	if (resume)
		buf_addf(cmd, " %s %s", agent->resume, uuid);
	else
		buf_addf(cmd, " --session-id %s", uuid);
}

int cmd_rerun(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct prompt_list pl;
	struct session_groups sg;
	struct prompt_ref *order;
	struct buf cmd;
	struct rerun_conv *convs = NULL;
	size_t nr_convs = 0, i, n = 0, selected = 0;
	const struct rerun_agent *agent;
	const char *name, *model, *mode, *salt, *from, *only;
	char *msg_path;
	int run;

	opts_init(&o, argc, argv, (const char *const[]){
		"--agent=", "--model=", "--permission-mode=", "--salt=",
		"--from=", "--only-session=", "--yes", "-y", NULL });

	name = opts_value(&o, "--agent");
	if (!name)
		name = "claude";
	agent = rerun_agent_find(name);
	if (!agent) {
		fprintf(stderr, "rerun: agent must be one of ");
		rerun_agent_names();
		fprintf(stderr, "\n");
		exit(2);
	}
	if (!agent->resume)
		gp_die("rerun: %s cannot be told which conversation to continue, so a\n"
		       "        session it began could not be returned to; a rerun\n"
		       "        that ran each session as a string of unrelated\n"
		       "        conversations would not be a replay of this history",
		       agent->name);

	mode = opts_value(&o, "--permission-mode");
	if (!mode)
		mode = agent->mode_default;
	if (agent->modes) {
		const char *const *m;
		int ok = 0;

		for (m = agent->modes; *m; m++)
			if (!strcmp(*m, mode))
				ok = 1;
		if (!ok) {
			fprintf(stderr, "rerun: --permission-mode must be one of ");
			for (m = agent->modes; *m; m++)
				fprintf(stderr, "%s%s", m == agent->modes ? "" : ", ",
					*m);
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

	salt = opts_value(&o, "--salt");
	from = opts_value(&o, "--from");
	only = opts_value(&o, "--only-session");
	run = opts_flag(&o, "--yes") || opts_flag(&o, "-y");

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

	printf("rerun: %lu of %lu prompt(s), agent %s, permission mode %s\n",
	       (unsigned long)selected, (unsigned long)pl.nr, agent->name, mode);
	if (selected != pl.nr)
		printf("       from the filters given, not the whole history\n");
	if (r->root) {
		char *root = repo_root_display(r);

		printf("       the agent works in %s and may change it\n", root);
		free(root);
	}
	if (!run)
		printf("       dry run -- nothing will be run; pass --yes to run "
		       "it\n");
	printf("\n");

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

		rerun_build(&cmd, agent, model, mode, c->uuid, resume);
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
		rc = system(buf_cstr(&cmd));
		if (rc != 0) {
			char *id = xstrdup(p->id ? p->id : "?");

			remove(msg_path);
			buf_release(&cmd);
			free(msg_path);
			free(order);
			rerun_convs_release(convs, nr_convs);
			session_groups_release(&sg);
			prompt_list_release(&pl);
			gp_die("rerun: %s failed on %s (exit %d) -- is %s on "
			       "PATH?\n"
			       "        %lu prompt(s) before it were done; "
			       "`rerun --from %s --yes` starts again there",
			       agent->name, id, rc, agent->name,
			       (unsigned long)(n - 1), id);
		}
	}

	if (run)
		remove(msg_path);
	printf("\nrerun: %lu prompt(s) across %lu conversation(s)%s\n",
	       (unsigned long)n, (unsigned long)nr_convs,
	       run ? "" : ", nothing run");

done:
	buf_release(&cmd);
	free(msg_path);
	free(order);
	rerun_convs_release(convs, nr_convs);
	session_groups_release(&sg);
	prompt_list_release(&pl);
	return 0;
}
