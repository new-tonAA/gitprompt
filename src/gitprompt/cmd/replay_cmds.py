"""Replay and timeline — turn a repository back into an ordered set of prompts."""

from __future__ import annotations

import os
import sys

from ..argparser import Args, Opt
from ..replay import build_timeline, render, render_timeline, stats_of, write_layout
from ..utils import GitPromptError, paint
from ._helpers import err, out


def cmd_replay(repo, argv):
    args = Args.parse([
        Opt("-f", "--format", takes_value=True, default="md"),
        Opt("-o", "--output", takes_value=True),
        Opt("--session", takes_value=True, repeatable=True),
        Opt("--layout", takes_value=True),
        Opt("--no-outcomes"),
        Opt("--no-preamble"),
        Opt("--json"),
        Opt("--stat"),
        Opt("--list-sessions"),
        Opt("-q", "--quiet"),
    ], argv)

    ref = args.positional[0] if args.positional else None
    sessions = args.list("session") or None

    if args.flag("list_sessions"):
        for n, (session, items) in enumerate(build_timeline(repo, ref=ref), 1):
            title = session.title if session else "unattributed"
            first = items[0].timestamp if items else "-"
            last = items[-1].timestamp if items else "-"
            out(f"{n:>3}. {session.id if session else '-':<28} {len(items):>4} prompt(s)  "
                f"{first} .. {last}  {title}")
        return 0

    groups = build_timeline(repo, ref=ref, sessions=sessions)
    if not groups:
        raise GitPromptError(
            "no prompts found in this repository\n"
            "hint: record one with 'gitprompt prompt \"<text>\"', "
            "or check that the history contains prompt objects"
        )

    if args.flag("stat") or args.flag("q") and not args.get("o"):
        stats = stats_of(groups)
        out(f"sessions: {stats['sessions']}")
        out(f"prompts:  {stats['prompts']}")
        out(f"span:     {stats['first']} .. {stats['last']}")
        if stats["models"]:
            out(f"models:   {', '.join(stats['models'])}")
        if stats["authors"]:
            out(f"authors:  {', '.join(stats['authors'])}")
        if args.flag("stat"):
            return 0

    if args.get("layout"):
        dest = args.get("layout")
        if args.get("o"):
            raise GitPromptError(
                "replay: --layout writes a directory tree and -o writes one file; "
                "ask for one of them"
            )
        written = write_layout(groups, dest)
        out(f"Wrote {written['prompts']} prompt(s) in {written['sessions']} session(s) to {dest}")
        out(paint(f"  start with {os.path.join(dest, 'REPLAY.md')}", "dim"))
        return 0

    fmt = "json" if args.flag("json") else args.get("f", "md")
    name = os.path.basename(repo.root or repo.gpdir)
    text = render(groups, fmt=fmt, name=name,
                  include_outcomes=not args.flag("no_outcomes"))

    if args.flag("no_preamble") and fmt in ("md", "markdown"):
        idx = text.find("\n---\n")
        if idx != -1:
            text = text[idx + len("\n---\n"):].lstrip("\n")

    if args.get("o"):
        dest = args.get("o")
        os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
        with open(dest, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        stats = stats_of(groups)
        err(f"Wrote {stats['prompts']} prompt(s) across {stats['sessions']} session(s) to {dest}")
    else:
        sys.stdout.write(text)
    return 0


def cmd_timeline(repo, argv):
    args = Args.parse([
        Opt("-f", "--full"), Opt("--body"),
        Opt("--session", takes_value=True, repeatable=True),
        Opt("--no-color"),
    ], argv)
    ref = args.positional[0] if args.positional else None
    groups = build_timeline(repo, ref=ref, sessions=args.list("session") or None)
    if not groups:
        err("no prompts recorded yet")
        return 0
    sys.stdout.write(render_timeline(
        groups,
        color=not args.flag("no_color"),
        show_bodies=args.flag("full") or args.flag("body"),
    ))
    return 0


def cmd_log_prompt(repo, argv):
    args = Args.parse([
        Opt("-n", "--max-count", takes_value=True),
        Opt("--session", takes_value=True),
        Opt("--oneline"), Opt("--reverse"), Opt("--stat"),
        Opt("--no-color"), Opt("--json"),
    ], argv)
    limit = int(args.get("n") or args.get("max_count") or 0)
    prompts = repo.iter_prompts(session=args.get("session"))
    if args.flag("reverse"):
        prompts.reverse()
    if limit:
        prompts = prompts[:limit]

    if args.flag("json"):
        import json
        out(json.dumps([
            {"id": p.id, "sha": p.sha, "session": p.session, "seq": p.seq,
             "timestamp": p.timestamp, "tags": p.tags, "outcome": p.outcome,
             "body": p.body}
            for p in prompts
        ], indent=2, ensure_ascii=False))
        return 0

    current = repo.current_session_id()
    last_session = None
    for p in prompts:
        if p.session != last_session:
            last_session = p.session
            label = f"session {p.session}" if p.session else "session (unattributed)"
            if p.session == current:
                label += "  (current)"
            out("")
            out(paint(label, "cyan"))
        head = f"  {int(p.seq or 0):>3}. {p.timestamp}"
        if args.flag("stat"):
            out(head)
            out(paint(f"       id {p.id}  sha {p.sha[:8] if p.sha else '-'}", "dim"))
            first = p.body.strip().split("\n", 1)[0]
            out(f"       {first[:100]}")
            if p.tags:
                out(paint("       tags: " + ", ".join(p.tags), "dim"))
            if p.outcome:
                out(paint(f"       outcome: {p.outcome}", "green"))
        else:
            first = p.body.strip().split("\n", 1)[0]
            out(f"{head}  {first[:96]}")
    return 0
