"""Configuration files in git's own format.

Layout, section syntax, subsection quoting and key lower-casing all follow
`git config`, so a `.gitprompt/config` reads like a `.git/config` and the same
habits transfer.  Scope order (later wins) is: system → global → local → env.
"""

from __future__ import annotations

import os
import re

from . import GP_DIR
from .utils import GitPromptError

SECTION_RE = re.compile(r'^\[([^\s"\]]+)(?:\s+"([^"]*)")?\]\s*$')
KV_RE = re.compile(r"^([A-Za-z][A-Za-z0-9-]*)\s*(?:=\s*(.*))?$")

# Keys that only ever hold one value; everything else can repeat.
MULTI_VALUE = {
    "remote.fetch", "remote.push", "remote.url",
}


def _parse_value(raw: str) -> str:
    raw = raw.strip()
    if not raw:
        return ""
    if raw.startswith('"') and raw.endswith('"') and len(raw) >= 2:
        body = raw[1:-1]
        return (body.replace("\\\\", "\x00")
                    .replace('\\"', '"')
                    .replace("\\n", "\n")
                    .replace("\\t", "\t")
                    .replace("\x00", "\\"))
    # Strip a trailing comment, but only when it is not escaped.
    out = []
    in_quote = False
    i = 0
    while i < len(raw):
        ch = raw[i]
        if ch == "\\" and i + 1 < len(raw):
            out.append(ch)
            out.append(raw[i + 1])
            i += 2
            continue
        if ch == '"':
            in_quote = not in_quote
        if ch in "#;" and not in_quote:
            break
        out.append(ch)
        i += 1
    return "".join(out).strip()


def _quote_value(value: str) -> str:
    if value and not re.search(r'^\s|\s$|[#;"\\]', value):
        return value
    escaped = (value.replace("\\", "\\\\")
                    .replace('"', '\\"')
                    .replace("\n", "\\n")
                    .replace("\t", "\\t"))
    return f'"{escaped}"'


class Config:
    """A layered view over the config files.

    `Config.load(repo)` merges system, global and local files.  Writes go to
    whichever scope was asked for, never to a merged blob, so `gitprompt
    config --global` cannot accidentally rewrite a repository's local config.
    """

    def __init__(self, entries=None):
        # ordered list of (fullkey, value); order is preserved so repeated
        # keys (remote.*.fetch) come back the way they were written.
        self._entries = list(entries or [])
        self._files = {}          # scope -> path

    # -- construction ------------------------------------------------------

    SYSTEM_PATHS = [
        "/etc/gitpromptconfig",
        os.path.join(os.environ.get("PROGRAMDATA", "C:\\ProgramData"), "gitprompt", "config"),
    ]

    @staticmethod
    def global_path() -> str:
        return os.path.join(os.path.expanduser("~"), ".gitpromptconfig")

    @staticmethod
    def local_path(gpdir: str) -> str:
        return os.path.join(gpdir, "config")

    @classmethod
    def global_config(cls) -> "Config":
        cfg = cls()
        path = cls.global_path()
        cfg._files["global"] = path
        cfg._read_file(path)
        return cfg

    @classmethod
    def load(cls, repo=None) -> "Config":
        cfg = cls()
        for path in cls.SYSTEM_PATHS:
            if os.path.exists(path):
                cfg._files.setdefault("system", path)
                cfg._read_file(path)
        gpath = cls.global_path()
        cfg._files["global"] = gpath
        cfg._read_file(gpath)
        if repo is not None:
            lpath = cls.local_path(repo.gpdir)
            cfg._files["local"] = lpath
            cfg._read_file(lpath)
            # a worktree-specific file, if one ever exists
            for extra in ("config.worktree",):
                p = os.path.join(repo.gpdir, extra)
                if os.path.exists(p):
                    cfg._read_file(p)
        cfg._apply_env()
        return cfg

    def _apply_env(self):
        for key, value in os.environ.items():
            if key.startswith("GITPROMPT_CONFIG_"):
                name = key[len("GITPROMPT_CONFIG_"):].lower().replace("_", ".")
                self._entries.append((name, value))

    # -- file IO -----------------------------------------------------------

    def _read_file(self, path: str) -> None:
        if not path or not os.path.exists(path):
            return
        section = ""
        with open(path, "r", encoding="utf-8") as fh:
            for raw in fh:
                line = raw.rstrip("\n")
                stripped = line.strip()
                if not stripped or stripped[0] in "#;":
                    continue
                m = SECTION_RE.match(stripped)
                if m:
                    name, sub = m.group(1), m.group(2)
                    section = f'{name.lower()}.{sub}' if sub else name.lower()
                    continue
                if stripped.startswith("["):   # malformed section header
                    raise GitPromptError(f"bad config line in {path}: {line!r}")
                m = KV_RE.match(stripped)
                if not m:
                    raise GitPromptError(f"bad config line in {path}: {line!r}")
                key = m.group(1).lower()
                value = _parse_value(m.group(2) or "")
                full = f"{section}.{key}" if section else key
                self._entries.append((full, value))

    @staticmethod
    def _write_entries(path: str, entries) -> None:
        lines = ["# gitprompt configuration file\n"]
        current = None
        for full, value in entries:
            section, _, key = full.rpartition(".")
            if not section:
                section, key = "", full
            if section != current:
                if current is not None:
                    lines.append("")
                for scope in ("remote", "branch"):
                    if section.startswith(scope + "."):
                        sub = section[len(scope) + 1:]
                        lines.append(f'[{scope} "{sub}"]')
                        break
                else:
                    lines.append(f"[{section}]")
                current = section
            lines.append(f"\t{key} = {_quote_value(value)}")
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        tmp = path + f".lock{os.getpid()}"
        with open(tmp, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
        os.replace(tmp, path)

    def _entries_from_file(self, path: str):
        sub = Config()
        sub._read_file(path)
        return sub._entries

    def _scope_path(self, scope: str) -> str:
        if scope in self._files:
            return self._files[scope]
        if scope == "global":
            return self.global_path()
        raise GitPromptError(f"unknown config scope '{scope}'")

    # -- reading -----------------------------------------------------------

    def get(self, key: str, default=None) -> str:
        key = key.lower()
        for full, value in reversed(self._entries):
            if full == key:
                return value
        return default

    def get_all(self, key: str):
        key = key.lower()
        return [v for k, v in self._entries if k == key]

    def get_bool(self, key: str, default=False) -> bool:
        value = self.get(key)
        if value is None:
            return default
        return value.strip().lower() in ("true", "yes", "on", "1", "")

    def get_int(self, key: str, default=0) -> int:
        value = self.get(key)
        if value is None:
            return default
        try:
            return int(value)
        except ValueError:
            raise GitPromptError(f"bad numeric config value for '{key}': {value!r}")

    # -- writing -----------------------------------------------------------

    def set(self, key: str, value: str, scope: str = "local") -> None:
        path = self._scope_path(scope)
        entries = self._entries_from_file(path)
        key = key.lower()
        # replace the last occurrence in place, preserving file order
        for i in range(len(entries) - 1, -1, -1):
            if entries[i][0] == key:
                entries[i] = (key, value)
                break
        else:
            entries.append((key, value))
        self._write_entries(path, entries)
        self._entries.append((key, value))

    def unset(self, key: str, scope: str = "local") -> bool:
        path = self._scope_path(scope)
        entries = self._entries_from_file(path)
        key = key.lower()
        kept = [e for e in entries if e[0] != key]
        if len(kept) == len(entries):
            return False
        self._write_entries(path, kept)
        return True

    def unset_section(self, prefix: str, scope: str = "local") -> int:
        path = self._scope_path(scope)
        entries = self._entries_from_file(path)
        prefix = prefix.lower()
        kept = [e for e in entries if not (e[0] == prefix or e[0].startswith(prefix + "."))]
        removed = len(entries) - len(kept)
        if removed:
            self._write_entries(path, kept)
        return removed

    def items(self, prefix: str = ""):
        prefix = prefix.lower()
        for key, value in self._entries:
            if key.startswith(prefix):
                yield key, value

    def paths(self) -> dict:
        return dict(self._files)


# --------------------------------------------------------------------------
# convenience accessors used across the codebase
# --------------------------------------------------------------------------

def user_identity(cfg: "Config"):
    name = cfg.get("user.name")
    email = cfg.get("user.email")
    if not name or not email:
        raise GitPromptError(
            "author identity unknown\n\n"
            "*** Please tell me who you are.\n\n"
            "Run\n\n"
            '  gitprompt config --global user.email "you@example.com"\n'
            '  gitprompt config --global user.name "Your Name"\n\n'
            "to set your account's default identity."
        )
    return name, email


def default_branch(cfg: "Config") -> str:
    return cfg.get("init.defaultBranch") or "main"


def prompt_dir(cfg: "Config") -> str:
    return (cfg.get("gitprompt.promptDir") or "prompts").strip("/") or "prompts"


def default_model(cfg: "Config"):
    return cfg.get("gitprompt.model")
