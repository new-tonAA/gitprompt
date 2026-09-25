"""Prompt-authoring commands — the part that is not in git at all.

`session` groups prompts into sittings, `prompt`/`capture` record them, and
`outcome` records what came of one.  Together these are what make a history
replayable rather than just readable.
"""

from __future__ import annotations

import os
import sys

from ..argparser import Args, Opt
from ..index import stat_of
from ..prompt import prompt_filename, prompt_to_file, slugify
from ..utils import GitPromptError, now_rfc3339, paint, short_id
from ._helpers import err, matches, out, relpaths
from ..revision import short_sha


# --------------------------------------------------------------------------

def cmd_session(repo, argv):
    sub = argv[0] if argv and not argv[0].startswith("-") else "list"
    rest = argv[1:] if argv and not argv[0].startswith("-") else argv

    if sub in ("start", "new", "begin"):
        return _session_start(repo, rest)
    if sub in ("end", "stop", "finish"):
        return _session_end(repo, rest)
    if sub in ("list", "ls"):
        return _session_list(repo, rest)
    if sub in ("show", "inspect"):
        return _session_show(repo, rest)
    if sub in ("use", "resume", "switch"):
        return _session_use(repo, rest)
    if sub == "current":
        current = repo.current_session_id()
        out(current or "")
        return 0 if current else 1
    if sub in ("tag",):
        return _session_tag(repo, rest)
    raise GitPromptError(
        f"unknown session subcommand '{sub}'\n"
        "supported: start, end, list, show, use, current, tag"
    )


def _session_start(repo, argv):
    args = Args.parse([
        Opt("-t", "--title", takes_value=True),
        Opt("-m", "--model", takes_value=True),
        Opt("-n", "--note", "--notes", takes_value=True),
        Opt("-q", "--quiet"),
    ], argv)
    title = args.get("t") or " ".join(args.positional) or "untitled session"
    session_id = repo.new_session(title=title, notes=args.get("n"), model=args.get("m"))
    if not args.flag("q"):
        out(f"Started session {paint(session_id, 'cyan')} — {title}")
        out(paint("Prompts you record now will be grouped under this session.", "dim"))
    return 0


def _session_end(repo, argv):
    args = Args.parse([
        Opt("-n", "--note", "--notes", takes_value=True),
        Opt("-q", "--quiet"),
    ], argv)
    session_id = args.arg(0) or repo.current_session_id()
    if not session_id:
        raise GitPromptError("no session is currently active")
    session = repo.end_session(session_id, notes=args.get("n"))
    if not args.flag("q"):
        prompts = repo.iter_prompts(session=session_id)
        out(f"Ended session {paint(session.id, 'cyan')} — {session.title}")
        out(f"  {len(prompts)} prompt(s) recorded")
        out(f"  started {session.started_at}  ended {session.ended_at}")
    return 0


def _session_list(repo, argv):
    args = Args.parse([Opt("-a", "--all"), Opt("--no-color")], argv)
    sessions = repo.list_sessions()
    if not sessions:
        out("no sessions yet — create one with 'gitprompt session start'")
        return 0
    current = repo.current_session_id()
    counts = {}
    for prompt in repo.iter_prompts():
        counts[prompt.session] = counts.get(prompt.session, 0) + 1
    for session in sessions:
        marker = paint("* ", "green") if session.id == current else "  "
        title = session.title or "untitled"
        n = counts.get(session.id, 0)
        when = (session.started_at or "")[:19].replace("T", " ")
        line = f"{marker}{session.id}  {when}  {n:>3} prompt(s)  {title}"
        out(paint(line, "cyan") if session.id == current else line)
    return 0


def _session_show(repo, argv):
    args = Args.parse([Opt("--full"), Opt("--json")], argv)
    session_id = args.arg(0) or repo.current_session_id()
    if not session_id:
        raise GitPromptError("no session given and none is active")
    session = repo.load_session(session_id)

    if args.flag("json"):
        import json
        doc = session.to_dict()
        doc["prompts_detail"] = [
            {"index": p.meta.get("replay_index", p.seq), "id": p.id,
             "timestamp": p.timestamp, "body": p.body}
            for p in repo.iter_prompts(session=session_id)
        ]
        out(json.dumps(doc, indent=2, ensure_ascii=False))
        return 0

    out(paint(f"session {session.id}", "bold") + f"  {session.title}")
    out(f"  started  {session.started_at}")
    if session.ended_at:
        out(f"  ended    {session.ended_at}")
    if session.author.get("name"):
        out(f"  author   {session.author['name']} <{session.author.get('email', '')}>")
    if session.model:
        out(f"  model    {session.model}")
    if session.notes:
        out(f"  notes    {session.notes}")
    prompts = repo.iter_prompts(session=session_id)
    out("")
    out(f"  {len(prompts)} prompt(s):")
    for p in prompts:
        first = p.body.strip().split("\n", 1)[0]
        out(f"    {int(p.seq or 0):>3}. [{p.timestamp}] {first[:84]}")
        if args.flag("full"):
            for line in p.body.strip().split("\n")[1:]:
                out(f"         {line}")
    return 0


def _session_use(repo, argv):
    args = Args.parse([], argv)
    session_id = args.arg(0)
    if not session_id:
        raise GitPromptError("session use: a session id is required")
    if not repo.session_sha(session_id):
        matches_ = [s.id for s in repo.list_sessions() if s.id.startswith(session_id)]
        if len(matches_) == 1:
            session_id = matches_[0]
        else:
            raise GitPromptError(f"no such session: {session_id}")
    repo.set_current_session(session_id)
    out(f"Now recording into session {paint(session_id, 'cyan')}")
    return 0


def _session_tag(repo, argv):
    args = Args.parse([Opt("-d", "--delete", takes_value=True, repeatable=True)], argv)
    session_id = repo.current_session_id()
    if not session_id:
        raise GitPromptError("no session is currently active")
    session = repo.load_session(session_id)
    if args.get("d"):
        for tag in args.list("d"):
            if tag in session.tags:
                session.tags.remove(tag)
    for tag in args.positional:
        if tag not in session.tags:
            session.tags.append(tag)
    repo.save_session(session)
    out(f"session {session_id} tags: {', '.join(session.tags) or '(none)'}")
    return 0


def _find_prompt(repo, spec: str):
    """Resolve a reference to a prompt: id, sha prefix, or sequence number.

    A bare number means "the Nth prompt of this session", because that is the
    number printed next to a prompt while you are writing it and the only one
    a user can be expected to remember.  When the current session does not
    have that many, any session's Nth is accepted rather than failing — the
    number is unambiguous often enough to be worth the lookup.
    """
    if not spec:
        return None
    if spec.isdigit():
        n = int(spec)
        in_session = repo.iter_prompts(session=repo.current_session_id())
        for prompt in in_session:
            if prompt.seq == n:
                return prompt
        for prompt in repo.iter_prompts():
            if prompt.seq == n:
                return prompt
        return None
    for prompt in repo.iter_prompts():
        if prompt.id == spec or (prompt.sha or "").startswith(spec):
            return prompt
    return None


# --------------------------------------------------------------------------

def cmd_prompt(repo, argv):
    args = Args.parse([
        Opt("-m", "--message", takes_value=True),
        Opt("-F", "--file", takes_value=True),
        Opt("-t", "--tag", takes_value=True, repeatable=True),
        Opt("-s", "--session", takes_value=True),
        Opt("--model", takes_value=True),
        Opt("--no-add"),
        Opt("-q", "--quiet"),
        Opt("--parent", takes_value=True),
    ], argv)

    text = args.get("m")
    if text is None and args.get("F"):
        if args.get("F") == "-":
            text = sys.stdin.read()
        else:
            with open(args.get("F"), "r", encoding="utf-8") as fh:
                text = fh.read()
    if text is None:
        if not args.positional:
            raise GitPromptError(
                "prompt: no prompt text given\n"
                "hint: gitprompt prompt \"<text>\"  or  -m <text>  or  -F <file>  or  pipe on stdin"
            )
        text = " ".join(args.positional)
    if not text.strip():
        raise GitPromptError("prompt: refusing to record an empty prompt")

    session_id = args.get("s") or repo.current_session_id()
    if not session_id:
        session_id = repo.new_session(title="(implicit)")
        err(f"hint: no active session, started {session_id}")

    prompt, sha, relpath = repo.write_prompt(
        text,
        session_id=session_id,
        tags=args.list("t"),
        model=args.get("model"),
    )
    if args.get("parent"):
        prompt.parent_prompt = args.get("parent")

    if not args.flag("no_add"):
        index = repo.read_index()
        mode = 0o100640
        index.add(relpath, sha, mode, stat_of(repo._fs_path(relpath)))
        repo.write_index(index)

    if not args.flag("q"):
        out(f"[{paint(session_id, 'cyan')} #{prompt.seq}] {paint(relpath, 'green')}")
        out(paint(f"  prompt {prompt.id}  object {sha[:8]}", "dim"))
    return 0


def cmd_capture(repo, argv):
    """Read one prompt from stdin — the form you use when pasting whatever
    you just typed at an agent."""
    args = Args.parse([
        Opt("-t", "--tag", takes_value=True, repeatable=True),
        Opt("-s", "--session", takes_value=True),
        Opt("-e", "--edit"),
        Opt("--model", takes_value=True),
    ], argv)
    if sys.stdin.isatty():
        err("reading prompt from stdin (end with Ctrl-Z then Enter on Windows, "
            "Ctrl-D elsewhere)")
    text = sys.stdin.read()
    if not text.strip():
        raise GitPromptError("capture: nothing on stdin")

    session_id = args.get("s") or repo.current_session_id()
    if not session_id:
        session_id = repo.new_session(title="(implicit)")
    prompt, sha, relpath = repo.write_prompt(
        text, session_id=session_id, tags=args.list("t"), model=args.get("model"),
    )
    index = repo.read_index()
    index.add(relpath, sha, 0o100640, stat_of(repo._fs_path(relpath)))
    repo.write_index(index)
    first = text.strip().split("\n", 1)[0]
    out(f"[{session_id} #{prompt.seq}] {relpath}  {first[:70]}")
    return 0


# --------------------------------------------------------------------------

def cmd_outcome(repo, argv):
    """Record what a prompt produced.

    The outcome is not the artifact — the artifact belongs in the tree.  This
    is the sentence that tells a later reader (or agent) which of the twenty
    plausible next moves this history actually took, and why.
    """
    args = Args.parse([
        Opt("-m", "--message", takes_value=True),
        Opt("-F", "--file", takes_value=True),
        Opt("--last"),
        Opt("--clear"),
        Opt("--tag", takes_value=True, repeatable=True),
    ], argv)

    # The first positional names the prompt only when the caller did not
    # already name it another way; everything else is outcome text, so
    # `outcome --last "it worked"` reads the sentence as the outcome.
    if args.flag("last") or not args.positional:
        prompts = repo.iter_prompts(session=repo.current_session_id())
        if not prompts:
            raise GitPromptError("outcome: no prompts in the current session")
        target = prompts[-1]
        words = args.positional
    else:
        target = _find_prompt(repo, args.positional[0])
        words = args.positional[1:]
    if target is None:
        raise GitPromptError(f"outcome: no prompt matches '{args.arg(0)}'")

    text = args.get("m")
    if text is None and args.get("F"):
        with open(args.get("F"), "r", encoding="utf-8") as fh:
            text = fh.read()
    if text is None and words:
        text = " ".join(words)
    if text is None and not sys.stdin.isatty():
        text = sys.stdin.read()
    if args.flag("clear"):
        text = None
    if text is None:
        raise GitPromptError("outcome: no text given")

    for tag in args.list("tag"):
        if tag not in target.tags:
            target.tags.append(tag)
    target.outcome = text.strip()

    # Rewriting a prompt means writing a new object: the old one stays, so
    # anything already referencing it still resolves.
    old_sha = target.sha
    new_sha = repo.store.write("prompt", target)
    _rewrite_prompt_file(repo, target, new_sha)
    _repoint_references(repo, old_sha, new_sha)
    out(f"Recorded outcome on prompt {target.id} "
        f"({(old_sha or '')[:8]} → {new_sha[:8]})")
    return 0


def _rewrite_prompt_file(repo, prompt, sha: str) -> None:
    """Write the updated prompt back to its file and re-stage it.

    The object is the record, but the *file* is what `add` re-hashes — so
    updating only the object leaves the two disagreeing, and the next
    `add -A` quietly reverts the edit.  Re-hashing through the same path
    `add` uses keeps file, index and object in agreement by construction.
    """
    relpath = prompt.meta.get("path")
    if not relpath:
        return
    fs_path = repo._fs_path(relpath)
    if not os.path.exists(fs_path):
        return
    os.makedirs(os.path.dirname(fs_path) or ".", exist_ok=True)
    with open(fs_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(prompt_to_file(prompt))
    index = repo.read_index()
    if index.get(relpath) is not None:
        mode, staged_sha, _kind = repo.hash_worktree_file(relpath)
        index.add(relpath, staged_sha, mode, stat_of(fs_path))
        repo.write_index(index)


def _repoint_references(repo, old_sha, new_sha):
    """Update index entries that still point at the superseded object.

    A fallback for prompts with no file on disk, where the rewrite above
    cannot fix the index by path.
    """
    if not old_sha:
        return
    index = repo.read_index()
    touched = False
    for path, entry in list(index.entries.items()):
        if entry.sha == old_sha:
            index.add(path, new_sha, entry.mode, entry.stat)
            touched = True
    if touched:
        repo.write_index(index)
