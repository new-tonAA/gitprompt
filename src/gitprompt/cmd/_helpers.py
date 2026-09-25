"""Shared helpers for command implementations."""

from __future__ import annotations

import os
import sys

from ..argparser import Args, Opt
from ..objects import TYPE_COMMIT, Commit
from ..utils import GitPromptError, normalize_path, paint, parse_rfc3339


def out(text: str = "", end: str = "\n") -> None:
    sys.stdout.write(text + end)


def err(text: str) -> None:
    sys.stderr.write(text + "\n")


def relpaths(repo, args) -> list:
    """Turn CLI paths into repo-relative ones, rejecting anything outside."""
    if not args:
        return []
    rel = []
    for raw in args:
        candidate = os.path.abspath(raw)
        if not os.path.exists(candidate):
            rel.append(normalize_path(raw))
            continue
        try:
            common = os.path.commonpath([repo.root, candidate])
        except ValueError:
            raise GitPromptError(f"'{raw}' is outside the repository")
        if common != repo.root:
            raise GitPromptError(f"'{raw}' is outside the repository")
        rel.append(os.path.relpath(candidate, repo.root).replace("\\", "/"))
    return rel


def resolve_paths(repo, args, allow_all=True):
    """Expand CLI pathspecs, supporting a bare directory meaning everything
    tracked or untracked beneath it."""
    paths = relpaths(repo, args)
    if not paths and allow_all:
        return None
    return paths


def matches(relpath: str, pathspecs) -> bool:
    if not pathspecs:
        return True
    for spec in pathspecs:
        if relpath == spec or relpath.startswith(spec.rstrip("/") + "/"):
            return True
    return False


def commit_line(repo, sha, commit, *, decorate="", color=True) -> str:
    """One-line commit summary: `abc1234 (HEAD -> main) subject`."""
    from ..revision import short_sha
    subject = (commit.message or "").strip().split("\n", 1)[0]
    head = paint(short_sha(repo, sha, 7), "yellow") if color else short_sha(repo, sha, 7)
    tail = paint(decorate, "cyan") if (decorate and color) else decorate
    return f"{head}{(' ' + tail) if tail else ''} {subject}"


def describe_sessions(repo, commit) -> str:
    """`session:short` marker for a commit's session, if it has one."""
    if not commit.session:
        return ""
    return commit.session


def fmt_iso(ts) -> str:
    return ts or "-"


def parse_date_filter(value: str):
    """--since/--until values → a comparable RFC3339 string."""
    if not value:
        return None
    lowered = value.strip().lower()
    if lowered in ("now", "today"):
        from ..utils import now_rfc3339
        return now_rfc3339()
    if lowered == "yesterday":
        from datetime import datetime, timedelta
        return (datetime.now() - timedelta(days=1)).replace(microsecond=0).isoformat()
    try:
        return parse_rfc3339(value).isoformat()
    except GitPromptError:
        raise GitPromptError(
            f"cannot parse date '{value}' "
            "(use ISO-8601, e.g. 2026-09-26T10:00:00+08:00)"
        )


def in_date_range(ts: str, since, until) -> bool:
    if not ts:
        return True
    try:
        value = parse_rfc3339(ts).isoformat()
    except GitPromptError:
        return True
    if since and value < since:
        return False
    if until and value > until:
        return False
    return True


def require_clean_or_force(repo, force: bool, action: str) -> None:
    if force:
        return
    index = repo.read_index()
    head = repo.head_tree()
    status = working_status(repo, index, head)
    if status["staged"] or status["modified"]:
        raise GitPromptError(
            f"your local changes would be overwritten by {action}\n"
            "commit them, or pass --force to discard them"
        )


def working_status(repo, index=None, head_tree=None) -> dict:
    """Classify every path into staged / modified / untracked.

    Shared by `status`, `commit` and the checkout guard, so all three agree on
    what "changed" means instead of each re-deriving it slightly differently.
    """
    index = index if index is not None else repo.read_index()
    head_tree = head_tree if head_tree is not None else repo.head_tree()

    staged, modified, untracked, deleted = [], [], [], []
    tracked = set(index.paths())

    # Walk the union, not just the index.  A path in HEAD but absent from the
    # index already has its deletion staged, and that state is reached without
    # anyone editing files — a push or reset that moved the ref out from under
    # the checkout leaves exactly this.  Iterating the index alone reports such
    # a tree as clean, which hides the deletion until the next `add -A` commits
    # it for real.
    for path in sorted(set(head_tree) | tracked):
        entry = index.get(path)
        if entry is None:
            staged.append(path)
            continue
        head_entry = head_tree.get(path)
        if head_entry is None or head_entry[1] != entry.sha:
            staged.append(path)
        fs_path = repo._fs_path(path)
        if not os.path.exists(fs_path):
            deleted.append(path)
            continue
        if not index.is_unchanged_on_disk(repo.root, entry):
            _mode, sha, _kind = repo.hash_worktree_file(path)
            if sha != entry.sha:
                modified.append(path)

    for relpath in repo.list_worktree_files("."):
        if relpath not in tracked:
            untracked.append(relpath)

    return {
        "staged": sorted(staged),
        "modified": sorted(modified),
        "untracked": sorted(untracked),
        "deleted": sorted(deleted),
    }


# --------------------------------------------------------------------------
# option sets reused across commands
# --------------------------------------------------------------------------

MESSAGE_OPTS = [
    Opt("-m", "--message", takes_value=True),
    Opt("--author", takes_value=True),
    Opt("--allow-empty"),
    Opt("--amend"),
    Opt("-q", "--quiet"),
    Opt("--force", "-f"),
]

LOG_OPTS = [
    Opt("--oneline"),
    Opt("--graph"),
    Opt("-n", "--max-count", takes_value=True),
    Opt("--all"),
    Opt("--no-color"),
    Opt("--reverse"),
    Opt("--since", takes_value=True),
    Opt("--until", takes_value=True),
    Opt("--author", takes_value=True),
    Opt("--session", takes_value=True),
    Opt("--stat"),
]

LIMIT_DEFAULT = 0
