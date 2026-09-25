"""`.gitpromptignore` matching, with gitignore's semantics.

Patterns are matched in file order; the last matching pattern wins, and a
leading `!` negates.  A pattern containing a slash is anchored to the repo
root, one without matches at any depth.  `**` spans directories; `*` and `?`
do not cross a slash.
"""

from __future__ import annotations

import os
import re

ALWAYS_IGNORE = {".gitprompt", ".git"}


def _translate(pattern: str) -> str:
    """One gitignore glob → a regex body (no anchors yet)."""
    out = []
    i = 0
    n = len(pattern)
    while i < n:
        ch = pattern[i]
        if ch == "*":
            if pattern[i:i + 2] == "**":
                j = i + 2
                while j < n and pattern[j] == "*":
                    j += 1
                if j < n and pattern[j] == "/":
                    out.append("(?:.*/)?")     # `**/` — zero or more directories
                    i = j + 1
                else:
                    out.append(".*")
                    i = j
            else:
                out.append("[^/]*")
                i += 1
        elif ch == "?":
            out.append("[^/]")
            i += 1
        elif ch == "[":
            end = pattern.find("]", i + 1)
            if end == -1:
                out.append(re.escape(ch))
                i += 1
            else:
                body = pattern[i + 1:end]
                if body.startswith("!"):
                    body = "^" + body[1:]
                out.append("[" + body + "]")
                i = end + 1
        elif ch == "\\" and i + 1 < n:
            out.append(re.escape(pattern[i + 1]))
            i += 2
        else:
            out.append(re.escape(ch))
            i += 1
    return "".join(out)


class IgnoreRule:
    __slots__ = ("pattern", "negate", "dir_only", "regex", "anchored", "source")

    def __init__(self, line: str, source: str = ""):
        self.source = source
        pattern = line
        self.negate = pattern.startswith("!")
        if self.negate:
            pattern = pattern[1:]
        self.dir_only = pattern.endswith("/")
        if self.dir_only:
            pattern = pattern[:-1]
        self.anchored = "/" in pattern
        if pattern.startswith("/"):
            pattern = pattern[1:]
        body = _translate(pattern)
        if self.anchored:
            self.regex = re.compile("^" + body + r"(?:/.*)?$")
        else:
            self.regex = re.compile(r"(?:^|.*/)" + body + r"(?:/.*)?$")
        self.pattern = line

    def match(self, path: str, is_dir: bool = False) -> bool:
        if self.dir_only and not is_dir:
            # a dir-only rule still hides everything below the directory
            return bool(self.regex.match(path))
        return bool(self.regex.match(path))


class IgnoreStack:
    def __init__(self):
        self.rules = []

    @classmethod
    def load(cls, root: str, extra_files=(".gitpromptignore",)):
        stack = cls()
        np = os.path.normpath
        for name in extra_files:
            path = np(os.path.join(root, name))
            if os.path.exists(path):
                stack.add_file(path, root)
        info_exclude = np(os.path.join(root, ".gitprompt", "info", "exclude"))
        if os.path.exists(info_exclude):
            stack.add_file(info_exclude, root, shared=True)
        return stack

    def add_file(self, path: str, root: str, shared: bool = False):
        base = "" if shared else _rel_dir(path, root)
        try:
            with open(path, "r", encoding="utf-8") as fh:
                for line in fh:
                    line = line.rstrip("\n").rstrip("\r")
                    if not line.strip() or line.lstrip().startswith("#"):
                        continue
                    if not shared and base:
                        line = base + "/" + line.lstrip("/") if not line.startswith("!") \
                            else "!" + base + "/" + line[1:].lstrip("/")
                    self.rules.append(IgnoreRule(line, path))
        except OSError:
            pass

    def is_ignored(self, path: str, is_dir: bool = False) -> bool:
        path = path.replace("\\", "/").strip("/")
        if not path:
            return False
        for component in path.split("/"):
            if component in ALWAYS_IGNORE:
                return True
        verdict = False
        for rule in self.rules:
            if rule.match(path, is_dir):
                verdict = not rule.negate
        return verdict

    def __len__(self):
        return len(self.rules)


def _rel_dir(config_path: str, root: str) -> str:
    rel = os.path.relpath(os.path.dirname(os.path.abspath(config_path)),
                          os.path.abspath(root))
    rel = rel.replace("\\", "/")
    return "" if rel == "." else rel.strip("/")
