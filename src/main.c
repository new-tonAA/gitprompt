/*
 * main.c - argument handling and command dispatch.
 *
 * Usage is git's: `gitprompt <command> [<args>]`, with the same command
 * names, so a habit formed on git carries over unchanged.
 */
#include "gp.h"

#include <ctype.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

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

/*
 * The options a command accepts, as a NULL-terminated list of names, passed by
 * the command to opts_init itself.  A name ending in '=' takes a value, which
 * is what the caller used to pass as a second array -- one list instead of two,
 * and no way for the two to disagree about the same option.
 *
 * The list belongs to the command rather than to the program.  A program-wide
 * list could only catch names no command has; `gitprompt log --amend` was
 * accepted and ignored, which is the worse half of the problem the check
 * exists for: an option that belongs to a different command should not look as
 * though it took effect here.
 */
static int entry_matches(const char *entry, const char *name)
{
	size_t n = strlen(entry);

	if (n && entry[n - 1] == '=')
		n--;
	return strlen(name) == n && !strncmp(entry, name, n);
}

static int takes_a_value(const char *const *allows, const char *name)
{
	size_t i;

	for (i = 0; allows[i]; i++)
		if (allows[i][0] && allows[i][strlen(allows[i]) - 1] == '=' &&
		    entry_matches(allows[i], name))
			return 1;
	return 0;
}

static int option_is_known(const char *const *allows, const char *name)
{
	size_t i;

	for (i = 0; allows[i]; i++)
		if (entry_matches(allows[i], name))
			return 1;
	return 0;
}

/*
 * Naming what the command does take, rather than only what it does not, is
 * what makes the refusal useful: an option from git that gitprompt has not
 * implemented and one that belongs to a different command look the same
 * otherwise, and the reader has to go to the help to find out which it was.
 */
static void reject_unknown_option(const char *const *allows, const char *name)
{
	struct buf hint = BUF_INIT;
	size_t i;

	gp_error("unknown option '%s'", name);
	for (i = 0; allows[i]; i++) {
		size_t n = strlen(allows[i]);
		int value = n && allows[i][n - 1] == '=';

		if (value)
			n--;
		buf_addf(&hint, "%s%.*s%s", i ? ", " : "", (int)n, allows[i],
			 value ? " <value>" : "");
	}
	if (hint.len)
		fprintf(stderr, "hint: this command takes %s\n", buf_cstr(&hint));
	else
		fprintf(stderr, "hint: this command takes no options\n");
	buf_release(&hint);
	exit(1);
}

/*
 * Parse argv into flags and positionals.  Long options accept --name=value
 * as well as --name value; short options may be bundled ("-am"), and a
 * short option that takes a value swallows the rest of its cluster.
 * Everything after a bare "--" is positional.
 */
void opts_init(struct opts *o, int argc, char **argv,
	       const char *const *allows)
{
	static const char *const none[] = { NULL };
	int i;
	int only_positional = 0;

	if (!allows)
		allows = none;
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
				if (!option_is_known(allows, name))
					reject_unknown_option(allows, name);
				opts_add(o, name, eq + 1);
			} else if (takes_a_value(allows, name) &&
				   i + 1 < argc) {
				opts_add(o, name, argv[++i]);
			} else {
				if (!option_is_known(allows, name))
					reject_unknown_option(allows, name);
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
				if (!option_is_known(allows, shortname))
					reject_unknown_option(allows, shortname);
				if (takes_a_value(allows, shortname)) {
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
	  "init [--bare] [-b <name>] [<dir>]" },
	{ "clone",      cmd_clone,      "Clone a prompt repository into a new directory",
	  "clone <url> [<dir>]" },
	{ "config",     cmd_config,     "Get and set repository or global options",
	  "config [--global] [--list] [--unset] <key> [<value>]" },

	/* record prompts */
	{ "session",    cmd_session,    "Begin, end and inspect prompting sessions",
	  "session <start|end|list|show|use|current> [...]" },
	{ "prompt",     cmd_prompt,     "Record one prompt",
	  "prompt [-m TEXT] [-F FILE] [-t TAG]... [--model M] [--parent ID]"
	  " [-s SESSION] [--no-stage]" },
	{ "capture",    cmd_capture,    "Record a prompt read from stdin",
	  "capture [-t TAG]... [--model M] [--parent ID] [-s SESSION]"
	  " [--no-stage]" },
	{ "outcome",    cmd_outcome,    "Attach a note about what a prompt produced",
	  "outcome <prompt-id> <text>\n   outcome --last <text>" },
	{ "add",        cmd_add,        "Add file contents to the index",
	  "add [-A] [-u] [-n] [--pathspec-from-file F] [--] <path>..." },
	{ "rm",         cmd_rm,         "Remove files from the work tree and the index",
	  "rm [--cached] <path>..." },
	{ "mv",         cmd_mv,         "Move or rename a file, and update the index",
	  "mv <source> <destination>" },
	{ "commit",     cmd_commit,     "Record the staged changes",
	  "commit [-m MSG] [-F FILE] [-e] [--no-edit] [-a] [--amend]"
	  " [--allow-empty] [-q]" },

	/* reconstruct a project */
	{ "replay",     cmd_replay,     "Reconstruct the prompt history as one document",
	  "replay [<ref>] [--format=md|json|txt] [-o FILE] [--layout=DIR] [--stat]\n"
	  "   replay <ref> --list-sessions" },
	{ "timeline",   cmd_timeline,   "Show the history as a compact list",
	  "timeline [<ref>]" },
	{ "log-prompt", cmd_log_prompt, "List prompts without commit noise",
	  "log-prompt [<ref>] [--oneline]" },
	{ "attach",     cmd_attach,     "Write the history where an agent reads its context",
	  "attach [--agent=claude|codex] [<ref>] [-o FILE] [--dry-run] [--force]" },

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
	  "branch [-d <name>] [-m <new>] [-v] [-a] [<name> [<start>]]" },
	{ "checkout",   cmd_checkout,   "Switch branches or restore files",
	  "checkout [-b <name>] [-B <name>] [-f] [<rev>|-- <path>...]" },
	{ "switch",     cmd_switch,     "Switch branches",
	  "switch [-c <name>] [-f] [<branch>]" },
	{ "merge",      cmd_merge,      "Join another history into this one",
	  "merge [-m MSG] [--no-commit] [--ff-only] [--no-ff] [--squash] [-X ours|theirs]\n"
	  "   merge --abort" },
	{ "tag",        cmd_tag,        "Create, list or delete tags",
	  "tag [-d <name>] [-m MSG] [-l] [<name> [<rev>]]" },
	{ "reset",      cmd_reset,      "Move HEAD and the index",
	  "reset [--soft|--mixed|--hard] [<rev>]" },
	{ "describe",   cmd_describe,   "Name a commit by its nearest tag",
	  "describe [--tags] [<rev>]" },

	/* collaborate */
	{ "remote",     cmd_remote,     "Manage the set of remotes",
	  "remote [-v] [add|remove|set-url|rename] ..." },
	{ "push",       cmd_push,       "Update remote refs along with objects",
	  "push [-u] [-f] [--tags] [<remote>] [<refspec>...]" },
	{ "fetch",      cmd_fetch,      "Download objects and refs from a remote",
	  "fetch [<remote>] [<refspec>...]" },
	{ "pull",       cmd_pull,       "Fetch and merge in one step",
	  "pull [<remote>] [<branch>]" },
	{ "serve",      cmd_serve,      "Serve this repository over gitprompt's HTTP transport",
	  "serve [--port N] [--host HOST] [--dir DIR]" },

	/* plumbing */
	{ "hash-object",      cmd_hash_object,      "Compute an object id and optionally store the object",
	  "hash-object [-t TYPE] [-w] [--stdin] [<file>...]" },
	{ "cat-file",         cmd_cat_file,         "Show an object's type, size or contents",
	  "cat-file (-t|-s|-p|-e) <object>" },
	{ "ls-tree",          cmd_ls_tree,          "List the contents of a tree",
	  "ls-tree [-r] [--name-only] [<tree>]" },
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
	  "gc [-n] [--dry-run]" },
	{ "fsck",       cmd_fsck,       "Check the repository for corruption",
	  "fsck [-v] [--verbose]" },
	{ "help",       cmd_help,       "Show help for a command or topic",
	  "help [<command>]" },
	{ "version",    cmd_version,    "Show the version",
	  "version" },
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
 * put it in.  `serve` is here because it can be pointed at a repository with
 * --dir, which is the only way to serve a bare store or one that is not the
 * current directory; inside a repository it serves that one, and outside one
 * with no --dir it says so itself rather than letting the dispatcher refuse
 * before it has had a chance to look.
 */
static int repo_is_optional(const char *name)
{
	return !strcmp(name, "config") || !strcmp(name, "hash-object") ||
	       !strcmp(name, "serve");
}

static void die_usage(void)
{
	fprintf(stderr, "usage: gitprompt <command> [<args>]\n");
	fprintf(stderr, "'gitprompt help' lists the commands.\n");
	exit(129);
}

#ifdef _WIN32
/*
 * Windows hands a C program its arguments in the ANSI code page, so a prompt
 * typed in anything but ASCII arrives as those bytes -- stored that way, a
 * prompt file is not the UTF-8 that every other part of the store is, and
 * `replay` writes a document no reader will decode.  git reads its command
 * line as UTF-16 and converts it, for the same reason; this does that.  The
 * split is the one the runtime would have made, so only the encoding differs.
 */
static char **argv_as_utf8(int *argcp)
{
	wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), argcp);
	char **out;
	int i;

	if (!wide)
		return NULL;

	out = xcalloc((size_t)*argcp + 1, sizeof(*out));
	for (i = 0; i < *argcp; i++) {
		int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0,
					    NULL, NULL);

		if (n <= 0) {
			LocalFree(wide);
			return NULL;
		}
		out[i] = xmalloc((size_t)n);
		WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, out[i], n,
				    NULL, NULL);
	}
	LocalFree(wide);
	return out;
}
#endif

int main(int argc, char **argv)
{
	const struct command *cmd;
	struct repo repo;
	struct repo *rp = NULL;
	int rc;

	/*
	 * Windows would otherwise turn every '\n' written to a stream into
	 * "\r\n", including the ones inside generated prompt files and the
	 * documents `replay` writes -- carriage returns that no git object, no
	 * diff and no other implementation expects to find.  git puts its own
	 * streams in binary mode for the same reason.
	 */
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stderr), _O_BINARY);

	{
		int wargc;
		char **wargv = argv_as_utf8(&wargc);

		if (wargv) {
			argc = wargc;
			argv = wargv;
		}
	}
#endif

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
