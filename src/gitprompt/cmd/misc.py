"""help, version, gc, fsck, stats."""

from __future__ import annotations

import os
import shutil
import sys

from .. import __version__
from ..argparser import Args, Opt
from ..objects import TYPE_COMMIT, TYPE_PROMPT, TYPE_SESSION, TYPE_TREE, hash_object
from ..utils import GitPromptError, paint
from ._helpers import err, out

TOPICS = {
    "replay": """\
gitprompt replay [<ref>] [--format=md|json|txt] [--output=<file>]
                 [--session=<id>] [--layout=<dir>] [--stat]

Reconstructs the prompt history as an ordered document you can hand to an
agent.  This is the command gitprompt exists for: `checkout` gives you the
files a project ended up with, `replay` gives you the prompts that produced
them, grouped by the session they were written in and in the order they were
written.

  --format=md      Markdown with a preamble addressed to the agent (default)
  --format=json    Machine-readable, including every metadata field
  --format=txt     Bare prompts, session markers only
  --layout=DIR     Write one directory per session instead of one document
  --stat           Just counts, span, models and authors
""",
    "merge": """\
gitprompt merge [--no-commit] [--ff-only] [--abort] [<branch>]

Joins another history into the current one.  The merge is path-level: a path
changed on only one side is taken from that side, so two branches that added
different prompts usually merge without a word.

When the same prompt diverged on both sides it is a conflict.  The file is
left on disk with markers, each block a complete prompt file:

    <<<<<<< HEAD
    ... this branch's version ...
    ||||||| base
    ... the common ancestor, for reference ...
    =======
    ... the other branch's version ...
    >>>>>>> <branch>

Edit the file down to what you want, 'gitprompt add' it to declare the conflict
resolved, then 'gitprompt commit' -- which records both parents, so the join
stays visible in the history.

  --no-commit   Prepare the merge but leave the commit to you
  --ff-only     Refuse if the merge would not be a fast-forward
  --abort       Discard an in-progress merge, back to HEAD
""",
    "session": """\
gitprompt session <start|end|list|show|use|current|tag> [...]

A session is one continuous stretch of prompting — one conversation, one
sitting.  Sessions are what make a replay faithful: they mark where the
agent's context was reset, so the reconstruction can re-establish state at the
same boundaries the original work had.

  start [-t TITLE]   Begin a session and make it current
  end   [-n NOTES]   Close the current session
  list               Every session, with prompt counts
  show  [ID]         Details and the prompts inside one session
  use   ID           Make an existing session current again
  current            Print the active session id
""",
    "prompt": """\
gitprompt prompt [-m TEXT] [-F FILE] [-t TAG]... [--model M] [-s SESSION]

Records one prompt.  Without -m/-F the remaining arguments are joined into the
prompt text; with stdin piped, the piped text is used.  The prompt is written
to <promptdir>/NNN-slug.md, stored as a `prompt` object, and staged.

gitprompt capture   — same thing, but always reads the prompt from stdin.
gitprompt outcome   — attach a note about what a prompt produced.
""",
    "objects": """\
gitprompt stores six kinds of object, all content-addressed and zlib-
compressed under .gitprompt/objects/:

  blob      arbitrary bytes
  tree      a directory snapshot; entries under the prompt directory carry
            mode 100640 and point at `prompt` objects instead of blobs
  commit    tree + parents + author + the session it belongs to
  tag       an annotated ref
  prompt    one prompt plus its provenance (session, seq, timestamp, author,
            model, tags, outcome, attachments)
  session   a container for the prompts written in one sitting

Because both tools hash as sha1("<type> <len>\\0" + payload), object ids are
stable and two people who record the same prompt arrive at the same hash.
""",
}


def cmd_help(repo, argv):
    args = Args.parse([Opt("--plumbing"), Opt("-a", "--all"), Opt("--web")], argv)
    from ..cli import COMMANDS, usage

    if args.flag("web"):
        out("https://github.com/new-tonAA/gitprompt")
        return 0

    topic = args.arg(0)
    if topic and topic in TOPICS:
        sys.stdout.write(TOPICS[topic])
        return 0
    if topic and topic in COMMANDS:
        module_name, func_name, flags, summary = COMMANDS[topic]
        out(paint(f"gitprompt {topic}", "bold") + f" — {summary}")
        if topic in TOPICS:
            sys.stdout.write("\n" + TOPICS[topic])
        doc = _docstring_of(module_name, func_name)
        if doc:
            out("")
            sys.stdout.write(doc)
        return 0
    if topic:
        raise GitPromptError(f"no help for '{topic}'")

    if args.flag("plumbing"):
        out("gitprompt plumbing commands:")
        for name, (_m, _f, flags, summary) in sorted(COMMANDS.items()):
            if name in ("add", "commit", "status", "log", "show", "diff", "branch",
                        "checkout", "switch", "merge", "tag", "reflog", "describe",
                        "init", "config", "clone", "remote", "push", "fetch", "pull",
                        "reset", "rm", "mv", "serve", "help", "version", "gc",
                        "fsck", "stats", "session", "prompt", "capture", "outcome",
                        "replay", "timeline", "log-prompt"):
                continue
            out(f"   {name:<18} {summary}")
        return 0

    print(usage())
    out("")
    out("'gitprompt help <command>' for a single command,")
    out("'gitprompt help <topic>' for topics: "
        + ", ".join(sorted(TOPICS)))
    return 0


def _docstring_of(module_name, func_name) -> str:
    import importlib
    try:
        module = importlib.import_module(f"..cmd.{module_name}", package=__package__)
        doc = getattr(getattr(module, func_name), "__doc__", None)
        return (doc or "").strip()
    except Exception:
        return ""


def cmd_version(repo, argv):
    args = Args.parse([Opt("--build-options")], argv)
    out(f"gitprompt version {__version__}")
    if args.flag("build_options"):
        out(f"python {sys.version.split()[0]}")
        out(f"platform {sys.platform}")
    return 0


# --------------------------------------------------------------------------

def cmd_gc(repo, argv):
    args = Args.parse([Opt("--prune", takes_value="?"), Opt("--aggressive"),
                       Opt("-q", "--quiet"), Opt("--dry-run")], argv)

    reachable = _reachable_objects(repo)
    loose = set(repo.store.iter_loose())
    garbage = loose - reachable

    if args.flag("dry_run"):
        out(f"reachable: {len(reachable)}")
        out(f"loose:     {len(loose)}")
        out(f"prunable:  {len(garbage)}")
        return 0

    removed, freed = 0, 0
    for sha in garbage:
        path = repo.store.object_path(sha)
        if os.path.exists(path):
            freed += os.path.getsize(path)
            os.remove(path)
            removed += 1
    # remove now-empty fanout directories
    objects_dir = repo.store.objects_dir
    if os.path.isdir(objects_dir):
        for sub in os.listdir(objects_dir):
            subdir = os.path.join(objects_dir, sub)
            if len(sub) == 2 and os.path.isdir(subdir) and not os.listdir(subdir):
                os.rmdir(subdir)

    if not args.flag("q"):
        out(f"Pruned {removed} unreachable object(s), freed {freed // 1024} KiB")
        out(f"Kept {len(reachable)} reachable object(s)")
    return 0


def _reachable_objects(repo) -> set:
    refs = list(repo.refs.list("refs/").values())
    head = repo.refs.head_sha()
    if head:
        refs.append(head)
    seen = set()
    frontier = list(refs)
    while frontier:
        sha = frontier.pop()
        if sha in seen:
            continue
        seen.add(sha)
        try:
            objtype, _payload = repo.store.read_raw(sha)
        except GitPromptError:
            continue
        if objtype == TYPE_COMMIT:
            commit = repo.store.read(sha)
            frontier.append(commit.tree)
            frontier.extend(commit.parents)
        elif objtype == TYPE_TREE:
            for _mode, _name, entry_sha in repo.store.read(sha):
                frontier.append(entry_sha)
    return seen


# --------------------------------------------------------------------------

def cmd_fsck(repo, argv):
    args = Args.parse([Opt("--strict"), Opt("-q", "--quiet"), Opt("--dangling")], argv)
    problems = []
    counts = {}

    for sha in repo.store.iter_loose():
        try:
            objtype, payload = repo.store.read_raw(sha)
        except Exception as exc:
            problems.append(f"{sha}: unreadable ({exc})")
            continue
        counts[objtype] = counts.get(objtype, 0) + 1
        if hash_object(objtype, payload, repo.store.fmt) != sha:
            problems.append(f"{sha}: content does not hash to its name")
            continue
        try:
            if objtype == TYPE_COMMIT:
                commit = repo.store.read(sha)
                if not repo.store.exists(commit.tree):
                    problems.append(f"{sha}: missing tree {commit.tree}")
                for parent in commit.parents:
                    if not repo.store.exists(parent):
                        problems.append(f"{sha}: missing parent {parent}")
            elif objtype == TYPE_TREE:
                for _mode, name, entry_sha in repo.store.read(sha):
                    if not repo.store.exists(entry_sha):
                        problems.append(f"{sha}: missing entry {entry_sha} ({name})")
        except GitPromptError as exc:
            problems.append(f"{sha}: {exc}")

    header = (f"Checked {sum(counts.values())} object(s): "
              + ", ".join(f"{n} {t}" for t, n in sorted(counts.items())))
    out(header)
    if problems:
        for line in problems:
            err(paint("error: " + line, "red"))
        out(f"{len(problems)} problem(s) found")
        return 1
    out("no problems found")
    return 0


# --------------------------------------------------------------------------

def cmd_stats(repo, argv):
    from ..replay import build_timeline, stats_of
    args = Args.parse([Opt("--json")], argv)
    groups = build_timeline(repo)
    stats = stats_of(groups)

    prompt_objs = sum(1 for sha in repo.store.iter_loose()
                      if _type_safe(repo, sha) == TYPE_PROMPT)
    commits = sum(1 for _ in repo.iter_commits(repo.refs.head_sha())) \
        if repo.refs.head_sha() else 0

    if args.flag("json"):
        import json
        doc = dict(stats)
        doc["commits"] = commits
        doc["prompt_objects"] = prompt_objs
        doc["objects"] = repo.store.count()
        out(json.dumps(doc, indent=2, ensure_ascii=False))
        return 0

    out(paint("repository", "bold"))
    out(f"  path        {repo.root or repo.gpdir}")
    out(f"  objects     {repo.store.count()}")
    out(f"  commits     {commits}")
    out(f"  branches    {len(repo.refs.branches())}")
    out(f"  tags        {len(repo.refs.tags())}")
    out("")
    out(paint("prompt history", "bold"))
    out(f"  sessions    {stats['sessions']}")
    out(f"  prompts     {stats['prompts']}")
    out(f"  span        {stats['first']} .. {stats['last']}")
    if stats["authors"]:
        out(f"  authors     {', '.join(stats['authors'])}")
    if stats["models"]:
        out(f"  models      {', '.join(stats['models'])}")
    if groups:
        out("")
        out(paint("sessions", "bold"))
        for session, items in groups:
            title = session.title if session else "unattributed"
            out(f"  {session.id if session else '-':<28} {len(items):>4} prompt(s)  {title}")
    return 0


def _type_safe(repo, sha):
    try:
        return repo.store.type_of(sha)
    except GitPromptError:
        return None
