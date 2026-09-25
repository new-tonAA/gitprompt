"""Entry point and command dispatch.

The dispatch table mirrors `commands[]` in git's `git.c`: one row per
subcommand, each carrying flags that say what setup the command needs before
it runs.  Keeping the same two flags git uses — `RUN_SETUP` for "must be
inside a repository" and `NEED_WORK_TREE` for "and it must not be bare" —
means the same class of mistake produces the same class of error message.
"""

from __future__ import annotations

import importlib
import os
import sys
import traceback

from . import __version__
from .utils import GitPromptError, RepoNotFound, paint

# -- setup flags (values chosen to match git's own bit layout) ---------------
RUN_SETUP = 1 << 0
NEED_WORK_TREE = 1 << 1
RUN_SETUP_GENTLY = 1 << 2
NO_PARSEOPT = 1 << 3

# name -> (module under gitprompt.cmd, function, flags, one-line summary)
COMMANDS = {
    # -- create / configure ------------------------------------------------
    "init":        ("init_cmds", "cmd_init", RUN_SETUP_GENTLY,
                    "Create an empty gitprompt repository"),
    "config":      ("init_cmds", "cmd_config", RUN_SETUP_GENTLY,
                    "Get and set repository or global options"),
    "clone":       ("init_cmds", "cmd_clone", 0,
                    "Clone a prompt repository into a new directory"),

    # -- authoring prompts (gitprompt-native) ------------------------------
    "session":     ("prompt_cmds", "cmd_session", RUN_SETUP | NEED_WORK_TREE,
                    "Start, end, list or inspect a prompting session"),
    "prompt":      ("prompt_cmds", "cmd_prompt", RUN_SETUP | NEED_WORK_TREE,
                    "Record a new prompt into the current session"),
    "capture":     ("prompt_cmds", "cmd_capture", RUN_SETUP | NEED_WORK_TREE,
                    "Append a prompt read from stdin to the current session"),
    "outcome":     ("prompt_cmds", "cmd_outcome", RUN_SETUP | NEED_WORK_TREE,
                    "Attach a result/outcome note to a prompt"),

    # -- snapshot layer (git porcelain) ------------------------------------
    "add":         ("snapshot", "cmd_add", RUN_SETUP | NEED_WORK_TREE,
                    "Add prompt files to the staging area"),
    "rm":          ("snapshot", "cmd_rm", RUN_SETUP | NEED_WORK_TREE,
                    "Remove files from the working tree and the index"),
    "mv":          ("snapshot", "cmd_mv", RUN_SETUP | NEED_WORK_TREE,
                    "Move or rename a file, a directory, or a symlink"),
    "status":      ("snapshot", "cmd_status", RUN_SETUP | NEED_WORK_TREE,
                    "Show the working tree status"),
    "commit":      ("snapshot", "cmd_commit", RUN_SETUP | NEED_WORK_TREE,
                    "Record staged changes to the repository"),
    "reset":       ("snapshot", "cmd_reset", RUN_SETUP,
                    "Reset current HEAD, index and/or working tree"),

    # -- history -----------------------------------------------------------
    "log":         ("history", "cmd_log", RUN_SETUP,
                    "Show the commit log"),
    "show":        ("history", "cmd_show", RUN_SETUP,
                    "Show a commit, prompt, session or tag"),
    "diff":        ("history", "cmd_diff", RUN_SETUP | NEED_WORK_TREE,
                    "Show changes between commits, the index and the worktree"),
    "branch":      ("history", "cmd_branch", RUN_SETUP,
                    "List, create or delete branches"),
    "checkout":    ("history", "cmd_checkout", RUN_SETUP | NEED_WORK_TREE,
                    "Switch branches or restore working tree files"),
    "switch":      ("history", "cmd_switch", RUN_SETUP | NEED_WORK_TREE,
                    "Switch branches"),
    "merge":       ("history", "cmd_merge", RUN_SETUP | NEED_WORK_TREE,
                    "Join two or more development histories together"),
    "tag":         ("history", "cmd_tag", RUN_SETUP,
                    "Create, list or delete tags"),
    "reflog":      ("history", "cmd_reflog", RUN_SETUP,
                    "Show where refs have pointed over time"),
    "describe":    ("history", "cmd_describe", RUN_SETUP,
                    "Describe a commit using the most recent tag"),

    # -- the gitprompt-native layer ----------------------------------------
    "replay":      ("replay_cmds", "cmd_replay", RUN_SETUP,
                    "Reconstruct the full prompt timeline, ready for an agent"),
    "timeline":    ("replay_cmds", "cmd_timeline", RUN_SETUP,
                    "Compact chronological view of every prompt"),
    "log-prompt":  ("replay_cmds", "cmd_log_prompt", RUN_SETUP,
                    "Log of prompts rather than commits"),

    # -- remotes -----------------------------------------------------------
    "remote":      ("remote_cmds", "cmd_remote", RUN_SETUP,
                    "Manage the set of tracked repositories"),
    "push":        ("remote_cmds", "cmd_push", RUN_SETUP,
                    "Update remote refs along with their objects"),
    "fetch":       ("remote_cmds", "cmd_fetch", RUN_SETUP,
                    "Download objects and refs from another repository"),
    "pull":        ("remote_cmds", "cmd_pull", RUN_SETUP | NEED_WORK_TREE,
                    "Fetch from and integrate with another repository"),
    "serve":       ("remote_cmds", "cmd_serve", RUN_SETUP_GENTLY,
                    "Serve this repository over HTTP for others to clone"),

    # -- plumbing ----------------------------------------------------------
    "hash-object":  ("plumbing", "cmd_hash_object", RUN_SETUP_GENTLY,
                     "Compute object ID and optionally create an object"),
    "cat-file":     ("plumbing", "cmd_cat_file", RUN_SETUP,
                     "Provide content or type information for objects"),
    "ls-tree":      ("plumbing", "cmd_ls_tree", RUN_SETUP,
                     "List the contents of a tree object"),
    "write-tree":   ("plumbing", "cmd_write_tree", RUN_SETUP | NEED_WORK_TREE,
                     "Create a tree object from the current index"),
    "commit-tree":  ("plumbing", "cmd_commit_tree", RUN_SETUP,
                     "Create a new commit object"),
    "rev-parse":    ("plumbing", "cmd_rev_parse", RUN_SETUP_GENTLY,
                     "Pick out and massage parameters"),
    "update-ref":   ("plumbing", "cmd_update_ref", RUN_SETUP,
                     "Update the object name stored in a ref safely"),
    "symbolic-ref": ("plumbing", "cmd_symbolic_ref", RUN_SETUP,
                     "Read, change or delete symbolic refs"),
    "count-objects": ("plumbing", "cmd_count_objects", RUN_SETUP,
                      "Count unpacked objects and their disk consumption"),
    "verify-objects": ("plumbing", "cmd_verify_objects", RUN_SETUP,
                       "Validate the object database"),
    "check-ref-format": ("plumbing", "cmd_check_ref_format", 0,
                         "Check if a ref name is well formed"),
    "for-each-ref": ("plumbing", "cmd_for_each_ref", RUN_SETUP,
                     "Output information on each ref"),

    # -- housekeeping ------------------------------------------------------
    "help":        ("misc", "cmd_help", 0,
                    "Show help for gitprompt or a command"),
    "version":     ("misc", "cmd_version", 0,
                    "Show the gitprompt version"),
    "gc":          ("misc", "cmd_gc", RUN_SETUP,
                    "Clean up unnecessary files and optimise the repository"),
    "fsck":        ("misc", "cmd_fsck", RUN_SETUP,
                    "Verify connectivity and validity of objects"),
    "stats":       ("misc", "cmd_stats", RUN_SETUP,
                    "Summarise a repository: sessions, prompts, authors, span"),
}

ALIASES = {
    "co": "checkout", "ci": "commit", "br": "branch", "st": "status",
    "hist": "timeline", "replay": "replay",
}


def usage() -> str:
    lines = [
        "usage: gitprompt [-C <path>] [-c <name>=<value>] [--version] <command> [<args>]",
        "",
        "A distributed version control system for prompts.",
        "",
        "start a working area",
        "   init       Create an empty gitprompt repository",
        "   clone      Clone a prompt repository into a new directory",
        "   config     Get and set repository or global options",
        "",
        "record prompts into history",
        "   session    Start, end, list or inspect a prompting session",
        "   prompt     Record a new prompt into the current session",
        "   capture    Append a prompt read from stdin to the current session",
        "   outcome    Attach a result/outcome note to a prompt",
        "   add        Add prompt files to the staging area",
        "   commit     Record staged changes to the repository",
        "",
        "reconstruct a project",
        "   replay     Reconstruct the full prompt timeline, ready for an agent",
        "   timeline   Compact chronological view of every prompt",
        "   log-prompt Log of prompts rather than commits",
        "",
        "examine history",
        "   status     Show the working tree status",
        "   log        Show the commit log",
        "   show       Show a commit, prompt, session or tag",
        "   diff       Show changes between commits, the index and the worktree",
        "   reflog     Show where refs have pointed over time",
        "",
        "grow, mark and tweak your history",
        "   branch     List, create or delete branches",
        "   checkout   Switch branches or restore working tree files",
        "   switch     Switch branches",
        "   merge      Join two or more development histories together",
        "   tag        Create, list or delete tags",
        "   reset      Reset current HEAD, index and/or working tree",
        "",
        "collaborate",
        "   remote     Manage the set of tracked repositories",
        "   push       Update remote refs along with their objects",
        "   fetch      Download objects and refs from another repository",
        "   pull       Fetch from and integrate with another repository",
        "   serve      Serve this repository over HTTP for others to clone",
        "",
        "plumbing (see 'gitprompt help --plumbing')",
        "   hash-object, cat-file, ls-tree, write-tree, commit-tree,",
        "   rev-parse, update-ref, symbolic-ref, count-objects, verify-objects,",
        "   check-ref-format, for-each-ref",
        "",
        "other",
        "   stats      Summarise a repository: sessions, prompts, authors, span",
        "   gc         Clean up unnecessary files and optimise the repository",
        "   fsck       Verify connectivity and validity of objects",
        "   help       Show this help",
        "   version    Show the gitprompt version",
        "",
        f"gitprompt v{__version__}",
    ]
    return "\n".join(lines)


def _force_utf8() -> None:
    """Prompts are free text and routinely contain non-ASCII.

    On Windows the default console code page is often cp936/cp1252, which
    turns an em-dash in a prompt into mojibake on the way out — and can raise
    on the way in.  Reconfiguring with errors="replace" keeps output correct
    and makes an unconvertible character degrade instead of crashing.
    """
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is None:
            continue
        try:
            reconfigure(encoding="utf-8", errors="replace")
        except (ValueError, OSError):
            pass


def main(argv=None) -> int:
    _force_utf8()
    argv = list(sys.argv[1:] if argv is None else argv)
    try:
        return _dispatch(argv)
    except RepoNotFound as exc:
        sys.stderr.write(paint(f"fatal: {exc}\n", "red"))
        return 128
    except GitPromptError as exc:
        sys.stderr.write(paint(f"fatal: {exc}\n", "red"))
        return getattr(exc, "exit_code", 1)
    except KeyboardInterrupt:
        sys.stderr.write("\n")
        return 130
    except BrokenPipeError:
        try:
            sys.stdout.close()
        except Exception:
            pass
        return 0
    except Exception:
        if os.environ.get("GITPROMPT_TRACE"):
            traceback.print_exc()
        else:
            sys.stderr.write(paint("fatal: internal error\n", "red"))
            sys.stderr.write("(set GITPROMPT_TRACE=1 for a traceback)\n")
        return 128


def _dispatch(argv) -> int:
    # -- global options, applied before the subcommand name ------------------
    overrides = []
    while argv:
        token = argv[0]
        if token == "-C":
            if len(argv) < 2:
                raise GitPromptError("option '-C' requires a value")
            os.chdir(argv[1])
            argv = argv[2:]
        elif token.startswith("-C") and len(token) > 2:
            os.chdir(token[2:])
            argv = argv[1:]
        elif token == "-c":
            if len(argv) < 2 or "=" not in argv[1]:
                raise GitPromptError("option '-c' requires <name>=<value>")
            overrides.append(argv[1])
            argv = argv[2:]
        elif token in ("--version", "-v"):
            print(f"gitprompt version {__version__}")
            return 0
        elif token in ("--help", "-h"):
            print(usage())
            return 0
        elif token in ("--no-pager", "--no-color", "--no-optional-locks"):
            argv = argv[1:]
        elif token == "--":
            argv = argv[1:]
            break
        else:
            break

    if overrides:
        os.environ.setdefault("GITPROMPT_CONFIG_OVERRIDES", ",".join(overrides))

    if not argv:
        print(usage())
        return 1

    name = argv[0]
    rest = argv[1:]
    name = ALIASES.get(name, name)

    # `gitprompt --help` style, and `gitprompt <cmd> --help`
    if name not in COMMANDS:
        close = [c for c in COMMANDS if c.startswith(name[:3])]
        hint = f"\n\nThe most similar command is\n\t{close[0]}" if close else ""
        sys.stderr.write(f"gitprompt: '{name}' is not a gitprompt command. "
                         f"See 'gitprompt help'.{hint}\n")
        return 1

    module_name, func_name, flags, _summary = COMMANDS[name]

    repo = None
    if flags & RUN_SETUP:
        repo = _setup_repo(require_worktree=bool(flags & NEED_WORK_TREE))
    elif flags & RUN_SETUP_GENTLY:
        repo = _setup_repo(gently=True, require_worktree=False)

    module = importlib.import_module(f".cmd.{module_name}", package=__package__)
    handler = getattr(module, func_name)

    applied, rest = _apply_config_overrides(overrides, rest)
    try:
        return handler(repo, rest) or 0
    finally:
        for path, key, old in reversed(applied):
            _restore(path, key, old)


def _apply_config_overrides(overrides, argv):
    """`-c key=value` must take effect for the command that follows it only.

    Implemented by writing into the repository-local config file around the
    call and restoring afterwards, which keeps the override visible to every
    code path that reads config instead of only the ones that were taught
    about it.
    """
    if not overrides:
        return [], argv
    from .config import Config
    repo = _existing_repo()
    if repo is None:
        return [], argv
    path = Config.local_path(repo.gpdir)
    cfg = Config.load(repo)
    applied = []
    for item in overrides:
        key, _, value = item.partition("=")
        old = cfg.get(key)
        cfg.set(key, value, scope="local")
        applied.append((path, key, old))
        repo.reload_config()
    return applied, argv


def _restore(path, key, old_value):
    repo = _existing_repo()
    if repo is None:
        return
    cfg = repo.config
    if old_value is None:
        cfg.unset(key, scope="local")
    else:
        cfg.set(key, old_value, scope="local")
    repo.reload_config()


def _existing_repo():
    from .repo import Repository
    try:
        return Repository.discover(required=False)
    except Exception:
        return None


def _setup_repo(gently: bool = False, require_worktree: bool = False):
    from .repo import Repository
    repo = Repository.discover(required=not gently)
    if repo is None:
        return None
    if require_worktree and repo.is_bare:
        raise GitPromptError(
            "this operation must be run in a work tree\n"
            f"(the repository at {repo.gpdir} is bare)"
        )
    return repo


if __name__ == "__main__":
    sys.exit(main())
