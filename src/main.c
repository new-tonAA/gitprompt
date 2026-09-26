/*
 * main.c - argument handling and command dispatch.
 *
 * Usage is git's: `gitprompt <command> [<args>]`, with the same command
 * names, so a habit formed on git carries over unchanged.
 */
#include "gp.h"

#include <ctype.h>

/* ------------------------------------------------------------------ */
/* option parsing                                                      */

static void opts_add(struct opts *o, const char *name, const char *value)
{
	if (o->nf >= (int)(sizeof o->flags / sizeof o->flags[0]))
		return;
	o->flags[o->nf].name = name;
	o->flags[o->nf].value = value;
	o->nf++;
}

static int takes_a_value(const char *const *takes_value, int ntakes,
			 const char *name)
{
	int i;
	for (i = 0; i < ntakes; i++)
		if (!strcmp(takes_value[i], name))
			return 1;
	return 0;
}

/*
 * Every option name gitprompt accepts anywhere.  Anything else is a typo, and
 * saying so beats running a quietly different command: `--onelin` should not
 * be the same as `--oneline`, and `--ff-only` on the wrong command should not
 * look as though it took effect.
 *
 * The list is program-wide rather than per-command, so `gitprompt log --amend`
 * is accepted and ignored.  A per-command table would catch that too, but it
 * needs every command to declare its own list, and one list that is missing an
 * option turns a working command into a broken one -- the worse mistake.
 */
static const char *const known_options[] = {
	"-A", "-B", "-F", "-a", "-b", "-c", "-d", "-e", "-f", "-g", "-l",
	"-m", "-n", "-o", "-p", "-q", "-r", "-s", "-t", "-u", "-v", "-w",
	"--abort", "--all", "--allow-empty", "--amend", "--author", "--bare",
	"--batch", "--build-options", "--cached", "--create", "--delete",
	"--depth", "--dir", "--dry-run", "--ff-only", "--force", "--format",
	"--get", "--global", "--hard", "--help", "--initial-branch", "--json",
	"--layout", "--list", "--max-count", "--message", "--mixed", "--model",
	"--move", "--name-only", "--no-commit", "--no-stage", "--oneline",
	"--output", "--parent", "--pathspec-from-file", "--port", "--prune",
	"--quiet", "--session", "--set", "--set-upstream", "--short",
	"--single-branch", "--soft", "--source", "--stage", "--staged",
	"--stat", "--stdin", "--strict", "--tags", "--title", "--unset",
	"--update", "--verbose", "--version",
};

static int option_is_known(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof known_options / sizeof known_options[0]; i++)
		if (!strcmp(known_options[i], name))
			return 1;
	return 0;
}

static void reject_unknown_option(const char *name)
{
	gp_error("unknown option '%s'", name);
	exit(1);
}

/*
 * Parse argv into flags and positionals.  Long options accept --name=value
 * as well as --name value; short options may be bundled ("-am"), and a
 * short option that takes a value swallows the rest of its cluster.
 * Everything after a bare "--" is positional.
 */
void opts_init(struct opts *o, int argc, char **argv,
	       const char *const *takes_value, int ntakes)
{
	int i;
	int only_positional = 0;

	memset(o, 0, sizeof *o);
	o->args = xcalloc((size_t)argc + 1, sizeof(char *));

	for (i = 0; i < argc; i++) {
		const char *a = argv[i];

		if (only_positional || !a[0] || a[0] != '-' || !strcmp(a, "-")) {
			o->args[o->nargs++] = (char *)a;
			continue;
		}
		if (!strcmp(a, "--")) {
			only_positional = 1;
			continue;
		}
		if (a[1] == '-') {
			char *name = xstrdup(a);
			char *eq = strchr(name, '=');
			if (eq) {
				*eq = '\0';
				if (!option_is_known(name))
					reject_unknown_option(name);
				opts_add(o, name, eq + 1);
			} else if (takes_a_value(takes_value, ntakes, name) &&
				   i + 1 < argc) {
				opts_add(o, name, argv[++i]);
			} else {
				if (!option_is_known(name))
					reject_unknown_option(name);
				opts_add(o, name, NULL);
			}
			continue;
		}
		/* a cluster of short options */
		{
			size_t k;
			for (k = 1; a[k]; k++) {
				char shortname[3];
				shortname[0] = '-';
				shortname[1] = a[k];
				shortname[2] = '\0';
				if (!option_is_known(shortname))
					reject_unknown_option(shortname);
				if (takes_a_value(takes_value, ntakes, shortname)) {
					if (a[k + 1]) {
						opts_add(o, xstrdup(shortname), a + k + 1);
					} else if (i + 1 < argc) {
						opts_add(o, xstrdup(shortname), argv[++i]);
					} else {
						opts_add(o, xstrdup(shortname), NULL);
					}
					break;
				}
				opts_add(o, xstrdup(shortname), NULL);
			}
		}
	}
}

int opts_flag(const struct opts *o, const char *name)
{
	int i;
	for (i = 0; i < o->nf; i++)
		if (!strcmp(o->flags[i].name, name))
			return 1;
	return 0;
}

const char *opts_value(const struct opts *o, const char *name)
{
	int i;
	for (i = o->nf - 1; i >= 0; i--)
		if (!strcmp(o->flags[i].name, name))
			return o->flags[i].value;
	return NULL;
}

const char *opts_arg(const struct opts *o, int i)
{
	return (i >= 0 && i < o->nargs) ? o->args[i] : NULL;
}

int opts_count(const struct opts *o)
{
	return o->nargs;
}

/* ------------------------------------------------------------------ */
/* the command table                                                   */

const struct command commands[] = {
	/* start a working area */
	{ "init",       cmd_init,       "Create an empty gitprompt repository",
	  "init [--bare] [<dir>]" },
	{ "clone",      cmd_clone,      "Clone a prompt repository into a new directory",
	  "clone <url> [<dir>]" },
	{ "config",     cmd_config,     "Get and set repository or global options",
	  "config [--global] [--list] [--unset] <key> [<value>]" },

	/* record prompts */
	{ "session",    cmd_session,    "Begin, end and inspect prompting sessions",
	  "session <start|end|list|show|use|current> [...]" },
	{ "prompt",     cmd_prompt,     "Record one prompt",
	  "prompt [-m TEXT] [-F FILE] [-t TAG]... [--model M] [-s SESSION]" },
	{ "capture",    cmd_capture,    "Record a prompt read from stdin",
	  "capture [--model M] [-t TAG]..." },
	{ "outcome",    cmd_outcome,    "Attach a note about what a prompt produced",
	  "outcome <prompt-id> <text>" },
	{ "add",        cmd_add,        "Add file contents to the index",
	  "add [-A] [-u] [-n] [--] <path>..." },
	{ "commit",     cmd_commit,     "Record the staged changes",
	  "commit [-m MSG] [-a] [--amend] [--allow-empty]" },

	/* reconstruct a project */
	{ "replay",     cmd_replay,     "Reconstruct the prompt history as one document",
	  "replay [<ref>] [--format=md|json|txt] [-o FILE] [--layout=DIR] [--stat]" },
	{ "timeline",   cmd_timeline,   "Show the history as a compact list",
	  "timeline [<ref>]" },
	{ "log-prompt", cmd_log_prompt, "List prompts without commit noise",
	  "log-prompt [<ref>] [--oneline]" },

	/* examine the history */
	{ "status",     cmd_status,     "Show the working tree status",
	  "status [--short]" },
	{ "log",        cmd_log,        "Show the commit log",
	  "log [--oneline] [-n N] [<ref>]" },
	{ "show",       cmd_show,       "Show a commit, prompt or object",
	  "show [--stat] <rev|prompt-id>" },
	{ "diff",       cmd_diff,       "Show changes between commits, index and work tree",
	  "diff [--cached] [--stat] [<rev>] [<rev>]" },
	{ "reflog",     cmd_reflog,     "Show where refs have pointed",
	  "reflog [<ref>]" },

	/* grow, mark and tweak */
	{ "branch",     cmd_branch,     "List, create or delete branches",
	  "branch [-d] [-m] [-a] [<name> [<start>]]" },
	{ "checkout",   cmd_checkout,   "Switch branches or restore files",
	  "checkout [-b <name>] [-f] [<rev>|-- <path>...]" },
	{ "switch",     cmd_switch,     "Switch branches",
	  "switch [-c <name>] [<branch>]" },
	{ "merge",      cmd_merge,      "Join another history into this one",
	  "merge [--no-commit] [--ff-only] [--abort] [<rev>]" },
	{ "tag",        cmd_tag,        "Create, list or delete tags",
	  "tag [-d] [-l] [<name> [<rev>]]" },
	{ "reset",      cmd_reset,      "Move HEAD and the index",
	  "reset [--soft|--mixed|--hard] [<rev>]" },
	{ "describe",   cmd_describe,   "Name a commit by its nearest tag",
	  "describe [<rev>]" },

	/* collaborate */
	{ "remote",     cmd_remote,     "Manage the set of remotes",
	  "remote [-v] [add|remove|set-url|rename] ..." },
	{ "push",       cmd_push,       "Update remote refs along with objects",
	  "push [-u] [<remote>] [<refspec>...]" },
	{ "fetch",      cmd_fetch,      "Download objects and refs from a remote",
	  "fetch [<remote>] [<refspec>...]" },
	{ "pull",       cmd_pull,       "Fetch and merge in one step",
	  "pull [<remote>] [<branch>]" },
	{ "serve",      cmd_serve,      "Serve this repository over gitprompt's HTTP transport",
	  "serve [--port N] [--dir DIR]" },

	/* plumbing */
	{ "hash-object",      cmd_hash_object,      "Compute an object id and optionally store the object",
	  "hash-object [-t TYPE] [-w] [--stdin] [<file>...]" },
	{ "cat-file",         cmd_cat_file,         "Show an object's type, size or contents",
	  "cat-file (-t|-s|-p|-e) <object>" },
	{ "ls-tree",          cmd_ls_tree,          "List the contents of a tree",
	  "ls-tree [-r] [<tree>]" },
	{ "write-tree",       cmd_write_tree,       "Create a tree from the index",
	  "write-tree" },
	{ "commit-tree",      cmd_commit_tree,      "Create a commit object",
	  "commit-tree <tree> [-p <parent>]... [-m <msg>]" },
	{ "rev-parse",        cmd_rev_parse,        "Resolve a revision to an object id",
	  "rev-parse [--short] [<rev>...]" },
	{ "update-ref",       cmd_update_ref,       "Set a ref to an object id",
	  "update-ref <ref> <new> [<old>]" },
	{ "symbolic-ref",     cmd_symbolic_ref,     "Read or set a symbolic ref",
	  "symbolic-ref <name> [<ref>]" },
	{ "for-each-ref",     cmd_for_each_ref,     "List refs with their ids",
	  "for-each-ref [<prefix>]" },
	{ "ls-files",         cmd_ls_files,         "List the files in the index",
	  "ls-files [-s] [<path>...]" },
	{ "count-objects",    cmd_count_objects,    "Count the loose objects",
	  "count-objects [-v]" },
	{ "verify-objects",   cmd_verify_objects,   "Verify the object store",
	  "verify-objects" },
	{ "check-ref-format", cmd_check_ref_format, "Check whether a ref name is valid",
	  "check-ref-format <name>" },

	/* other */
	{ "stats",      cmd_stats,      "Summarise the repository and its prompt history",
	  "stats [--json]" },
	{ "gc",         cmd_gc,         "Prune unreachable objects",
	  "gc [--prune] [--dry-run] [-q]" },
	{ "fsck",       cmd_fsck,       "Check the repository for corruption",
	  "fsck [--strict] [-q]" },
	{ "help",       cmd_help,       "Show help for a command or topic",
	  "help [<command>]" },
	{ "version",    cmd_version,    "Show the version",
	  "version [--build-options]" },
};

const size_t commands_nr = sizeof commands / sizeof commands[0];

/* ------------------------------------------------------------------ */

static const struct command *lookup(const char *name)
{
	size_t i;
	for (i = 0; i < commands_nr; i++)
		if (!strcmp(commands[i].name, name))
			return &commands[i];
	return NULL;
}

/* commands that make sense with no repository in sight */
static int needs_no_repo(const char *name)
{
	return !strcmp(name, "init") || !strcmp(name, "clone") ||
	       !strcmp(name, "version") || !strcmp(name, "help");
}

/*
 * Commands that work outside a repository but want to know about one when
 * there is one.  `config` is here because `config --global` has to run before
 * a repository exists, while `config user.email` inside a project means that
 * project's config, and passing a NULL repository would send every one of
 * those reads to the global file.  `hash-object` is here because hashing a
 * file is a pure computation -- `git hash-object foo` works in an empty
 * directory -- and only `-w`, which stores what it hashed, needs a store to
 * put it in.
 */
static int repo_is_optional(const char *name)
{
	return !strcmp(name, "config") || !strcmp(name, "hash-object");
}

static void die_usage(void)
{
	fprintf(stderr, "usage: gitprompt <command> [<args>]\n");
	fprintf(stderr, "'gitprompt help' lists the commands.\n");
	exit(129);
}

int main(int argc, char **argv)
{
	const struct command *cmd;
	struct repo repo;
	struct repo *rp = NULL;
	int rc;

	if (argc < 2)
		die_usage();

	if (!strcmp(argv[1], "--version")) {
		printf("gitprompt version %s\n", GP_VERSION);
		return 0;
	}
	if (!strcmp(argv[1], "--help")) {
		argv[1] = (char *)"help";
	}

	cmd = lookup(argv[1]);
	if (!cmd) {
		fprintf(stderr, "gitprompt: '%s' is not a gitprompt command. "
			"See 'gitprompt help'.\n", argv[1]);
		return 1;
	}

	if (!needs_no_repo(cmd->name)) {
		if (repo_find(&repo, ".") < 0) {
			if (!repo_is_optional(cmd->name)) {
				fprintf(stderr,
					"fatal: not a gitprompt repository (or any parent up to the "
					"filesystem root)\n"
					"Stop at the root of a project and run 'gitprompt init', "
					"or clone one.\n");
				return 128;
			}
		} else {
			struct buf id;
			buf_init(&id);
			/* a reflog line ends "Name <email> 1700000000 +0800", so
			 * the identity kept for it has to carry the time */
			repo_ident_with_time(&repo, &id);
			repo.refs.ident = xstrdup(buf_cstr(&id));
			buf_release(&id);
			rp = &repo;
		}
	}

	rc = cmd->fn(rp, argc - 2, argv + 2);

	if (rp) {
		struct repo *r = rp;
		free(r->refs.ident);
		r->refs.ident = NULL;
		repo_release(r);
	}
	return rc;
}
