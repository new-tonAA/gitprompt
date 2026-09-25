"""Refs, HEAD, and the reflog — same layout and same rules as git.

Loose refs live in `.gitprompt/refs/…`; HEAD is a symref file; the reflog is
appended under `.gitprompt/logs/`.  `packed-refs` is read but not written —
loose-only is easier to reason about, and a `gitprompt pack-refs` can come
later without changing the reader.
"""

from __future__ import annotations

import os
import re

from .utils import (
    GitPromptError,
    format_tz,
    is_valid_hash,
    now_unix,
    tz_offset_minutes,
)

HEAD = "HEAD"
_HEADS = "refs/heads/"
_TAGS = "refs/tags/"
_REMOTES = "refs/remotes/"

# A ref name is a slash-separated path of components that cannot begin with a
# dot, cannot end in `.lock`, and cannot contain `..`, `~`, `^`, `:`, `?`, `*`,
# `[`, `\`, or whitespace.  Same rules git enforces, so the same branch names
# are legal in both tools.
_BAD_COMPONENT = re.compile(r"(^\.)|(\.\.)|[~^:?*\[\\\s]|(\.lock$)|(\.$)")
_BAD_FULL = re.compile(r"(^/)|(/$)|(//)|(@\{)|(\.\.)")


def check_ref_name(name: str) -> str:
    if not name or _BAD_FULL.search(name):
        raise GitPromptError(f"'{name}' is not a valid ref name")
    if name in (HEAD, "ORIG_HEAD", "FETCH_HEAD", "MERGE_HEAD"):
        return name
    for part in name.split("/"):
        if _BAD_COMPONENT.search(part) or part == "@":
            raise GitPromptError(f"'{name}' is not a valid ref name")
    return name


def is_branch(name: str) -> bool:
    return name.startswith(_HEADS)


def branch_name(name: str) -> str:
    return name[len(_HEADS):] if is_branch(name) else name


def full_branch_name(name: str) -> str:
    if name.startswith("refs/"):
        return name
    return _HEADS + name


class RefStore:
    def __init__(self, gpdir: str):
        self.gpdir = gpdir
        self.refs_dir = os.path.join(gpdir, "refs")
        self.logs_dir = os.path.join(gpdir, "logs")

    # -- raw file access ---------------------------------------------------

    def _path(self, refname: str) -> str:
        return os.path.join(self.gpdir, *refname.split("/"))

    def read(self, refname: str):
        """Return the sha a ref points at, or None.  Body of a symref is a sha."""
        path = self._path(refname)
        if not os.path.exists(path):
            return self._read_packed(refname)
        with open(path, "r", encoding="utf-8") as fh:
            value = fh.read().strip()
        if value.startswith("ref:"):
            return self.read(value[4:].strip())
        return value or None

    def read_symbolic(self, refname: str):
        """Return the ref name a symref targets, else None."""
        path = self._path(refname)
        if not os.path.exists(path):
            return None
        with open(path, "r", encoding="utf-8") as fh:
            value = fh.read().strip()
        return value[4:].strip() if value.startswith("ref:") else None

    def write_symbolic(self, refname: str, target: str) -> None:
        path = self._path(refname)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(f"ref: {check_ref_name(target)}\n")

    def update(self, refname: str, new_sha: str, old_sha=None,
               message: str = None, reflog: bool = True) -> None:
        """Move a ref, with git's compare-and-swap semantics.

        Passing `old_sha` turns this into a guarded update: if the ref has
        moved since the caller looked, the write is refused rather than
        clobbering concurrent work.
        """
        refname = check_ref_name(refname)
        current = self.read(refname)
        if old_sha is not None and current != old_sha:
            raise GitPromptError(
                f"cannot update {refname}: expected {_short(old_sha)}, found {_short(current)}"
            )
        if new_sha is not None and not is_valid_hash(new_sha):
            raise GitPromptError(f"refusing to point {refname} at invalid object {new_sha!r}")
        path = self._path(refname)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + f".lock{os.getpid()}"
        with open(tmp, "w", encoding="utf-8") as fh:
            if new_sha is None:
                fh.write("\0" * 40 + "\n")   # git's "null sha" = deleted ref
            else:
                fh.write(new_sha + "\n")
        os.replace(tmp, path)
        if reflog:
            self.append_log(refname, current, new_sha, message)

    def delete(self, refname: str, reflog: bool = True) -> None:
        path = self._path(refname)
        current = self.read(refname)
        if os.path.exists(path):
            os.remove(path)
        if reflog:
            self.append_log(refname, current, None, "deleted")

    def list(self, prefix: str = "refs/") -> dict:
        """Return {refname: sha} for every ref under `prefix`."""
        out = {}
        base = self._path(prefix.rstrip("/"))
        if os.path.isdir(base):
            for root, _dirs, files in os.walk(base):
                for name in files:
                    if name.endswith(".lock"):
                        continue
                    full = os.path.join(root, name)
                    rel = os.path.relpath(full, self.gpdir).replace("\\", "/")
                    sha = self.read(rel)
                    if sha:
                        out[rel] = sha
        out.update(self._packed_under(prefix))
        return out

    def branches(self) -> dict:
        return {branch_name(k): v for k, v in self.list(_HEADS).items()}

    def tags(self) -> dict:
        return {k[len(_TAGS):]: v for k, v in self.list(_TAGS).items()}

    def remotes(self) -> dict:
        """`other/main` rather than `refs/remotes/other/main`, so branch
        listings read the way git's do."""
        return {k[len(_REMOTES):]: v for k, v in self.list(_REMOTES).items()}

    # -- packed-refs (read-only) ------------------------------------------

    def _iter_packed(self):
        path = os.path.join(self.gpdir, "packed-refs")
        if not os.path.exists(path):
            return
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("#") or line.startswith("^"):
                    continue
                parts = line.split()
                if len(parts) == 2:
                    yield parts[1], parts[0]

    def _read_packed(self, refname: str):
        for name, sha in self._iter_packed():
            if name == refname:
                return sha
        return None

    def _packed_under(self, prefix: str) -> dict:
        return {n: s for n, s in self._iter_packed() if n.startswith(prefix)}

    # -- HEAD --------------------------------------------------------------

    def head_ref(self):
        """The branch HEAD points at, or None when detached."""
        return self.read_symbolic(HEAD)

    def head_sha(self):
        return self.read(HEAD)

    def is_detached(self) -> bool:
        return self.head_ref() is None and self.head_sha() is not None

    def set_head(self, refname: str) -> None:
        self.write_symbolic(HEAD, refname)

    def detach_head(self, sha: str) -> None:
        path = self._path(HEAD)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(sha + "\n")

    def current_branch(self):
        ref = self.head_ref()
        return branch_name(ref) if ref else None

    # -- reflog ------------------------------------------------------------

    def append_log(self, refname: str, old_sha, new_sha, message: str = None) -> None:
        """Append one reflog line.  The reflog is the safety net that makes
        every history rewrite reversible, so it is written on every ref move."""
        path = os.path.join(self.logs_dir, *refname.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        name, email = _who()
        line = "{old} {new} {who} {ts} {tz}\t{msg}\n".format(
            old=old_sha or ("\0" * 40),
            new=new_sha or ("\0" * 40),
            who=f"{name} <{email}>",
            ts=now_unix(),
            tz=format_tz(tz_offset_minutes()),
            msg=(message or "").replace("\n", " "),
        )
        with open(path, "a", encoding="utf-8") as fh:
            fh.write(line)

    def read_log(self, refname: str = HEAD, limit: int = None):
        path = os.path.join(self.logs_dir, *refname.split("/"))
        if not os.path.exists(path):
            return []
        out = []
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.rstrip("\n")
                if not line:
                    continue
                info, _, msg = line.partition("\t")
                parts = info.split()
                if len(parts) >= 2:
                    out.append({
                        "old": parts[0], "new": parts[1],
                        "who": " ".join(parts[2:-2]),
                        "ts": parts[-2], "tz": parts[-1], "message": msg,
                    })
        out.reverse()
        return out[:limit] if limit else out


def _who():
    from .config import Config
    try:
        cfg = Config.global_config()
        return (cfg.get("user.name") or os.environ.get("USERNAME")
                or os.environ.get("USER") or "unknown",
                cfg.get("user.email") or f"{os.environ.get('USERNAME', 'unknown')}@localhost")
    except Exception:
        return ("unknown", "unknown@localhost")


def _short(sha):
    if not sha or sha == "\0" * 40:
        return "(none)"
    return sha[:7]
