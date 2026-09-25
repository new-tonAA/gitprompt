"""Timeline reconstruction — the reason gitprompt exists.

`gitprompt checkout` gives you the *files* as they were.  That is not enough
to reproduce a project that was built by prompting, because the order the
prompts arrived in, and where one AI session ended and the next began, are
part of the input.  Feeding an agent the final tree tells it what the answer
looked like; feeding it the replay tells it how the answer was reached.

A replay is assembled from three layers:

  1. every reachable `prompt` object, ordered by (timestamp, session, seq)
  2. the `session` objects, which say where context was reset
  3. the commit graph, used to attribute prompts to the branch they landed on

Prompts that share a timestamp to the second and carry no session ordering
fall back to the commit graph, so a hand-written history still replays in a
sensible order.
"""

from __future__ import annotations

import json
import os

from .utils import GitPromptError, join_path

AGENT_PREAMBLE = """\
# Project replay: {name}

You are reconstructing a project from its prompt history. The history below is
complete and in chronological order. Each section is one *session* — a single
continuous working conversation. A session boundary means the original
context was reset, so you should treat a new session as a fresh start that
still inherits everything built before it.

Work through the sessions in order. Within a session, work through the prompts
in the order given. Do not skip ahead: later prompts assume the state left by
earlier ones, and several of them correct course rather than add features.

{stats}

---
"""


class ReplayEntry:
    __slots__ = ("prompt", "session", "commit", "index")

    def __init__(self, prompt, session=None, commit=None):
        self.prompt = prompt
        self.session = session
        self.commit = commit
        self.index = 0


def build_timeline(repo, ref: str = None, sessions=None, include_outcomes: bool = True):
    """Assemble the ordered, session-grouped history.

    Returns a list of (Session|None, [Prompt, ...]) groups in chronological
    order.  Prompts whose session object is missing still replay — they are
    grouped under a synthetic entry rather than dropped, because a missing
    session object is a metadata gap, not a reason to lose a prompt.
    """
    prompts = repo.iter_prompts(ref=ref)
    if sessions:
        wanted = set(sessions)
        prompts = [p for p in prompts if (p.session or "") in wanted]

    # Attribute each prompt to the first commit that introduced it, so a
    # prompt can report which branch/commit it landed on.
    attribution = {}
    for sha, commit in repo.iter_commits(ref or repo.refs.head_sha()):
        try:
            tree = repo.read_tree(commit.tree)
        except GitPromptError:
            continue
        for path, (_mode, psha, objtype) in tree.items():
            if objtype == "prompt" and psha not in attribution:
                attribution[psha] = (sha, commit)

    session_objects = {}
    for s in repo.list_sessions():
        session_objects[s.id] = s

    groups = {}
    order = []
    for prompt in prompts:
        sid = prompt.session or ""
        if sid not in groups:
            session = session_objects.get(sid)
            groups[sid] = ReplayEntry.__new__(ReplayEntry)  # placeholder, replaced below
            groups[sid] = (session, [])
            order.append(sid)
        groups[sid][1].append(prompt)

    ordered = []
    for sid in order:
        session, items = groups[sid]
        items.sort(key=lambda p: (p.timestamp or "", p.seq or 0, p.id))
        for i, p in enumerate(items, 1):
            p.seq = p.seq if p.seq is not None else i
            p.meta["replay_index"] = i
            if p.sha in attribution:
                p.meta["commit"] = attribution[p.sha][0]
        ordered.append((session, items))

    ordered.sort(key=lambda pair: (
        (pair[0].started_at if pair[0] and pair[0].started_at
         else (pair[1][0].timestamp if pair[1] else "")), ""
    ))
    return ordered


def stats_of(groups) -> dict:
    prompts = [p for _s, items in groups for p in items]
    sessions = [s for s, _i in groups if s]
    return {
        "sessions": len(groups),
        "named_sessions": len(sessions),
        "prompts": len(prompts),
        "first": prompts[0].timestamp if prompts else None,
        "last": prompts[-1].timestamp if prompts else None,
        "models": sorted({p.model for p in prompts if p.model}),
        "authors": sorted({(p.author or {}).get("name", "") for p in prompts
                           if (p.author or {}).get("name")}),
    }


# --------------------------------------------------------------------------
# renderers
# --------------------------------------------------------------------------

def _fmt_stats(stats) -> str:
    bits = [f"{stats['prompts']} prompt(s) across {stats['sessions']} session(s)"]
    if stats["first"]:
        bits.append(f"spanning {stats['first']} to {stats['last']}")
    if stats["models"]:
        bits.append("models: " + ", ".join(stats["models"]))
    if stats["authors"]:
        bits.append("authors: " + ", ".join(stats["authors"]))
    return "This history contains " + "; ".join(bits) + "."


def render_markdown(groups, name="project", include_outcomes=True, preamble=True) -> str:
    stats = stats_of(groups)
    out = []
    if preamble:
        out.append(AGENT_PREAMBLE.format(name=name, stats=_fmt_stats(stats)))
    for n, (session, items) in enumerate(groups, 1):
        title = session.title if session else "unattributed prompts"
        sid = session.id if session else "-"
        started = (session.started_at if session else items[0].timestamp) or "?"
        ended = (session.ended_at if session else items[-1].timestamp) or "?"
        out.append(f"## Session {n} — {title}")
        out.append("")
        out.append(f"- session id: `{sid}`")
        out.append(f"- started: {started}")
        out.append(f"- ended: {ended}")
        out.append(f"- prompts: {len(items)}")
        if session and session.model:
            out.append(f"- model: `{session.model}`")
        if session and session.notes:
            out.append(f"- notes: {session.notes}")
        out.append("")
        for p in items:
            idx = p.meta.get("replay_index", p.seq)
            out.append(f"### Prompt {n}.{idx}")
            out.append("")
            meta = [f"at `{p.timestamp}`"]
            if p.author.get("name"):
                meta.append(f"by {p.author['name']}")
            if p.model:
                meta.append(f"model `{p.model}`")
            if p.tags:
                meta.append("tags: " + ", ".join(p.tags))
            if p.meta.get("commit"):
                meta.append(f"commit `{p.meta['commit'][:8]}`")
            out.append("*" + " · ".join(meta) + "*")
            out.append("")
            out.append(p.body.rstrip("\n"))
            out.append("")
            if include_outcomes and p.outcome:
                out.append(f"> **Outcome:** {p.outcome}")
                out.append("")
            for att in p.attachments:
                if isinstance(att, dict):
                    out.append(f"> Attachment: `{att.get('name', '?')}` "
                               f"({att.get('hash', '')[:8]})")
                else:
                    out.append(f"> Attachment: `{att}`")
            if include_outcomes and p.attachments:
                out.append("")
    return "\n".join(out).rstrip("\n") + "\n"


def render_json(groups, name="project", include_outcomes=True) -> str:
    doc = {
        "name": name,
        "generator": "gitprompt",
        "stats": stats_of(groups),
        "sessions": [],
    }
    for session, items in groups:
        doc["sessions"].append({
            "id": session.id if session else None,
            "title": session.title if session else "unattributed prompts",
            "started_at": (session.started_at if session else None),
            "ended_at": (session.ended_at if session else None),
            "model": session.model if session else None,
            "notes": session.notes if session else None,
            "prompts": [
                {
                    "index": p.meta.get("replay_index", p.seq),
                    "sha": p.sha,
                    "id": p.id,
                    "timestamp": p.timestamp,
                    "author": p.author,
                    "model": p.model,
                    "tags": p.tags,
                    "outcome": p.outcome if include_outcomes else None,
                    "attachments": p.attachments,
                    "commit": (p.meta.get("commit") or None),
                    "body": p.body,
                }
                for p in items
            ],
        })
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def render_text(groups, name="project", include_outcomes=False) -> str:
    out = []
    for n, (session, items) in enumerate(groups, 1):
        title = session.title if session else "unattributed"
        out.append(f"===== SESSION {n}: {title} "
                   f"({session.id if session else '-'}) =====")
        out.append("")
        for p in items:
            out.append(f"----- prompt {p.meta.get('replay_index', p.seq)} "
                       f"[{p.timestamp}] -----")
            out.append(p.body.rstrip("\n"))
            out.append("")
    return "\n".join(out).rstrip("\n") + "\n"


RENDERERS = {"md": render_markdown, "markdown": render_markdown,
             "json": render_json, "txt": render_text, "text": render_text}


def render(groups, fmt="md", name="project", include_outcomes=True) -> str:
    fn = RENDERERS.get(fmt.lower())
    if not fn:
        raise GitPromptError(
            f"unknown replay format '{fmt}' (expected md, json or txt)"
        )
    return fn(groups, name=name, include_outcomes=include_outcomes)


# --------------------------------------------------------------------------
# filesystem layout
# --------------------------------------------------------------------------

def write_layout(groups, dest: str, include_manifest=True) -> dict:
    """Write the history to `dest` as one directory per session.

    This is the form to hand to an agent that works by reading files rather
    than by consuming a single document: `01-session/001-prompt.md` sorts
    correctly with a plain `ls`, so the ordering survives even if the agent
    ignores every piece of metadata we give it.
    """
    from .prompt import prompt_to_file, slugify

    os.makedirs(dest, exist_ok=True)
    written = {"sessions": 0, "prompts": 0, "files": []}
    for n, (session, items) in enumerate(groups, 1):
        title = slugify(session.title if session else "unattributed", 30)
        dirname = f"{n:02d}-{title}"
        sdir = os.path.join(dest, dirname)
        os.makedirs(sdir, exist_ok=True)
        written["sessions"] += 1

        readme = [
            f"# Session {n}: {session.title if session else 'unattributed'}",
            "",
            f"- id: `{session.id if session else '-'}`",
            f"- started: {(session.started_at if session else items[0].timestamp)}",
            f"- ended: {(session.ended_at if session else items[-1].timestamp)}",
            f"- prompts: {len(items)}",
            "",
            "Prompts in this directory are in order. Read them in filename order.",
            "",
        ]
        with open(os.path.join(sdir, "README.md"), "w", encoding="utf-8") as fh:
            fh.write("\n".join(readme))

        for i, p in enumerate(items, 1):
            fname = f"{i:03d}-{slugify(p.body, 30)}.md"
            with open(os.path.join(sdir, fname), "w", encoding="utf-8") as fh:
                fh.write(prompt_to_file(p))
            written["prompts"] += 1
            written["files"].append(join_path(dirname, fname))

    if include_manifest:
        with open(os.path.join(dest, "REPLAY.md"), "w", encoding="utf-8") as fh:
            fh.write(render_markdown(groups, name=os.path.basename(dest)))
        with open(os.path.join(dest, "replay.json"), "w", encoding="utf-8") as fh:
            fh.write(render_json(groups, name=os.path.basename(dest)))
        written["files"] += ["REPLAY.md", "replay.json"]
    return written


# --------------------------------------------------------------------------
# timeline (compact view)
# --------------------------------------------------------------------------

def render_timeline(groups, color: bool = False, show_bodies: bool = False) -> str:
    from .utils import paint
    out = []
    for n, (session, items) in enumerate(groups, 1):
        head = f"session {n}: {session.title if session else 'unattributed'}"
        when = (session.started_at if session else items[0].timestamp) or "?"
        out.append(paint(head, "bold") + paint(f"   {when}", "dim"))
        if session:
            out.append(paint(f"  id {session.id}  ·  {len(items)} prompt(s)", "dim"))
        for p in items:
            idx = p.meta.get("replay_index", p.seq)
            line = f"  {int(idx):>3}. [{p.timestamp}] "
            first = p.body.strip().split("\n", 1)[0]
            out.append(line + first[:100])
            if show_bodies:
                for extra in p.body.strip().split("\n")[1:]:
                    out.append("       " + extra[:100])
        out.append("")
    return "\n".join(out).rstrip("\n") + "\n"
