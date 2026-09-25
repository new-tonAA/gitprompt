"""Revision syntax: the same shorthand git accepts for naming a commit.

Supported forms::

    <sha>            full or abbreviated
    HEAD             the current commit
    main             a branch
    v1.0             a tag
    origin/main      a remote-tracking branch
    HEAD~3           three first-parents back
    HEAD^2           second parent
    main@{2}         where the branch was two moves ago
    @                an alias for HEAD
"""

from __future__ import annotations

import re

from .objects import Commit, is_valid_hash, resolve_prefix
from .refs import HEAD, _HEADS, _REMOTES, _TAGS
from .utils import GitPromptError

_SUFFIX_RE = re.compile(r"([~^])(\d*)$")
_AT_RE = re.compile(r"^(?P<base>.+?)@\{(?P<n>\d+)\}$")
_AT_DATE_RE = re.compile(r"^(?P<base>.+?)@\{(?P<when>[^}]+)\}$")


def resolve_revision(repo, name: str) -> str:
    if not name:
        raise GitPromptError("empty revision")
    if name == "@":
        name = HEAD

    m = _AT_RE.match(name)
    if m:
        return _from_reflog(repo, m.group("base"), int(m.group("n")))

    m = _AT_DATE_RE.match(name)
    if m:
        return _from_reflog_date(repo, m.group("base"), m.group("when"))

    base, ops = _split_suffixes(name)
    sha = _resolve_base(repo, base)
    for op, num in ops:
        sha = _apply_op(repo, sha, op, num, name)
    return sha


def _split_suffixes(name: str):
    ops = []
    while True:
        m = _SUFFIX_RE.search(name)
        if not m:
            break
        num = int(m.group(2)) if m.group(2) else None
        ops.insert(0, (m.group(1), num))
        name = name[:m.start()]
        if not name:
            raise GitPromptError("invalid revision syntax")
    return name, ops


def _resolve_base(repo, base: str) -> str:
    if base == "":
        raise GitPromptError("empty revision")
    if base == HEAD:
        sha = repo.refs.head_sha()
        if not sha:
            raise GitPromptError("HEAD does not point at any commit yet")
        return sha

    for candidate in _ref_candidates(base):
        sha = repo.refs.read(candidate)
        if sha:
            # peel from the sha we just read, not from the ref name, so the
            # lookup above is not repeated
            return repo.peel_sha(sha, base)

    if is_valid_hash(base) or len(base) >= 4:
        try:
            return repo.peel_sha(resolve_prefix(repo.store, base), base)
        except GitPromptError:
            # not an object here either — fall through to the real error
            pass
    raise GitPromptError(f"unknown revision or path: {base}")


def _ref_candidates(name: str):
    """The ref names a bare word could mean, in git's lookup order."""
    if name.startswith("refs/"):
        return [name]
    # `origin/main` is already covered by _REMOTES; sessions are ambiguous
    # with branches, so they are tried last.
    return [c for c in (
        HEAD if name == "HEAD" else None,
        _HEADS + name,
        _TAGS + name,
        _REMOTES + name,
        "refs/" + name,
        "refs/sessions/" + name,
    ) if c]


def _apply_op(repo, sha: str, op: str, num, original: str) -> str:
    if op == "~":
        steps = num if num is not None else 1
        for _ in range(steps):
            commit = repo.store.read(sha)
            if not isinstance(commit, Commit):
                raise GitPromptError(f"{original}: not a commit")
            if not commit.parents:
                raise GitPromptError(f"{original}: no such ancestor (root commit)")
            sha = commit.parents[0]
        return sha
    if op == "^":
        idx = 1 if num is None else num
        commit = repo.store.read(sha)
        if not isinstance(commit, Commit):
            raise GitPromptError(f"{original}: not a commit")
        if idx == 0:
            return sha
        if len(commit.parents) < idx:
            raise GitPromptError(
                f"{original}: commit has only {len(commit.parents)} parent(s)"
            )
        return commit.parents[idx - 1]
    raise GitPromptError(f"unsupported revision operator '{op}'")


def _from_reflog(repo, base: str, n: int) -> str:
    refname = _ref_for(repo, base)
    entries = repo.refs.read_log(refname)
    if not entries:
        entries = repo.refs.read_log(HEAD)
    if n >= len(entries):
        raise GitPromptError(f"{base}@{{{n}}}: reflog has only {len(entries)} entries")
    return entries[n]["new"]


def _from_reflog_date(repo, base: str, when: str) -> str:
    from .utils import parse_rfc3339
    from datetime import datetime
    if when.lower() == "yesterday":
        import time as _t
        target = datetime.now().timestamp() - 86400
    elif when.lower() == "now":
        return resolve_revision(repo, base)
    else:
        target = parse_rfc3339(when).timestamp()
    refname = _ref_for(repo, base)
    for entry in repo.refs.read_log(refname) or repo.refs.read_log(HEAD):
        if int(entry["ts"]) <= target:
            return entry["new"]
    raise GitPromptError(f"{base}@{{{when}}}: no reflog entry that old")


def _ref_for(repo, base: str) -> str:
    if base in ("", HEAD, "@"):
        return HEAD
    for candidate in _ref_candidates(base):
        if candidate and repo.refs.read(candidate):
            return candidate
    return HEAD


def short_sha(repo, sha: str, minlen: int = 7) -> str:
    """Shortest unambiguous abbreviation of `sha`."""
    if not sha:
        return ""
    length = minlen
    while length < len(sha):
        if not any(other.startswith(sha[:length])
                   for other in repo.store.iter_loose() if other != sha):
            return sha[:length]
        length += 1
    return sha[:length]
