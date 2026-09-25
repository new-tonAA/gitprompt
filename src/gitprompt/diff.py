"""Unified diffs in git's output format.

The line-matching itself uses `difflib.SequenceMatcher` rather than a hand-
rolled Myers implementation: it is the same algorithm family, it is in the
standard library, and it is far better tested than anything written here
would be.  What this module owns is git's *presentation* — the `diff
--git` header, hunk ranges, `+`/`-` prefixes and context handling.
"""

from __future__ import annotations

import difflib

from .objects import TYPE_BLOB, TYPE_PROMPT

CONTEXT = 3


class FileDiff:
    __slots__ = ("path", "old_path", "old_mode", "new_mode", "old_sha", "new_sha",
                 "old_text", "new_text", "status", "binary", "prompt")

    def __init__(self, path, old_mode=None, new_mode=None, old_sha=None, new_sha=None,
                 old_text="", new_text="", status="modified", binary=False, prompt=None):
        self.path = path
        self.old_path = path
        self.old_mode = old_mode
        self.new_mode = new_mode
        self.old_sha = old_sha
        self.new_sha = new_sha
        self.old_text = old_text
        self.new_text = new_text
        self.status = status
        self.binary = binary
        self.prompt = prompt          # Prompt object when the change is to a prompt

    @property
    def added(self) -> int:
        return sum(1 for l in self._hunks() if l.startswith("+") and not l.startswith("+++"))

    @property
    def removed(self) -> int:
        return sum(1 for l in self._hunks() if l.startswith("-") and not l.startswith("---"))

    def _hunks(self):
        old = self.old_text.splitlines(keepends=True)
        new = self.new_text.splitlines(keepends=True)
        return list(difflib.unified_diff(old, new, n=CONTEXT, lineterm="\n"))

    def render(self, color: bool = False, stat_only: bool = False) -> str:
        if stat_only:
            if self.binary:
                return f" {self.path} | Bin"
            total = self.added + self.removed
            bar = "+" * min(self.added, 40) + "-" * min(self.removed, 40)
            return f" {self.path} | {total} {bar}"
        if self.status == "deleted":
            return self._header(deleted=True)
        if self.binary:
            return self._header() + f"Binary files a/{self.path} and b/{self.path} differ\n"
        body = "".join(self._hunks())
        if not body:
            return ""
        return self._header() + body

    def _header(self, deleted: bool = False) -> str:
        lines = [f"diff --git a/{self.old_path} b/{self.path}"]
        if self.status == "added":
            lines.append("new file mode " + _mode(self.new_mode))
            lines.append("index " + "0" * 8 + f"..{_short(self.new_sha)}")
        elif deleted:
            lines.append("deleted file mode " + _mode(self.old_mode))
            lines.append("index " + f"{_short(self.old_sha)}.." + "0" * 8)
        else:
            if self.old_mode != self.new_mode:
                lines.append(f"old mode {_mode(self.old_mode)}")
                lines.append(f"new mode {_mode(self.new_mode)}")
            lines.append(f"index {_short(self.old_sha)}..{_short(self.new_sha)} "
                         + _mode(self.new_mode))
        lines.append(f"--- {'/dev/null' if self.status == 'added' else 'a/' + self.old_path}")
        lines.append(f"+++ {'/dev/null' if deleted else 'b/' + self.path}")
        return "\n".join(lines) + "\n"


def _mode(mode) -> str:
    return f"{mode:06o}" if mode else "100644"


def _short(sha) -> str:
    return (sha or "0" * 40)[:7]


def diff_trees(repo, old_tree: dict, new_tree: dict, paths=None, context=CONTEXT):
    """Yield FileDiff for every path that differs between two flattened trees."""
    paths = set(paths) if paths else None
    for path in sorted(set(old_tree) | set(new_tree)):
        if paths and path not in paths:
            continue
        old = old_tree.get(path)
        new = new_tree.get(path)
        if old and new and old[1] == new[1]:
            continue
        yield _make_diff(repo, path, old, new)


def _make_diff(repo, path, old, new) -> FileDiff:
    old_mode, old_sha, old_type = old if old else (None, None, None)
    new_mode, new_sha, new_type = new if new else (None, None, None)

    if not old:
        status = "added"
    elif not new:
        status = "deleted"
    else:
        status = "modified"

    old_text, old_binary = _text_of(repo, old_sha, old_type)
    new_text, new_binary = _text_of(repo, new_sha, new_type)

    return FileDiff(
        path=path,
        old_mode=old_mode, new_mode=new_mode,
        old_sha=old_sha, new_sha=new_sha,
        old_text=old_text, new_text=new_text,
        status=status,
        binary=old_binary or new_binary,
        prompt=_prompt_of(repo, new_sha, new_type),
    )


def _text_of(repo, sha, objtype):
    if not sha:
        return "", False
    raw = repo.store.read(sha)
    if not isinstance(raw, bytes):
        # prompt (and session) objects deserialise into objects rather than
        # payload bytes; render the prompt back to its file form for diffing.
        if objtype == TYPE_PROMPT or hasattr(raw, "body"):
            from .prompt import prompt_to_file
            return prompt_to_file(raw), False
        return "", True
    if b"\0" in raw[:8000]:
        return "", True
    try:
        return raw.decode("utf-8"), False
    except UnicodeDecodeError:
        return "", True


def _prompt_of(repo, sha, objtype):
    if objtype == TYPE_PROMPT and sha:
        obj = repo.store.read(sha)
        obj.sha = sha
        return obj
    return None


def render_diff(diffs, color: bool = False, stat_only: bool = False) -> str:
    if stat_only:
        rows = [d.render(color, stat_only=True) for d in diffs]
        rows = [r for r in rows if r]
        if not rows:
            return ""
        changed = len(rows)
        adds = sum(d.added for d in diffs)
        dels = sum(d.removed for d in diffs)
        return ("\n".join(rows)
                + f"\n {changed} file{'s' if changed != 1 else ''} changed"
                + (f", {adds} insertion{'s' if adds != 1 else ''}(+)" if adds else "")
                + (f", {dels} deletion{'s' if dels != 1 else ''}(-)" if dels else "")
                + "\n")
    return "".join(d.render(color) for d in diffs)
