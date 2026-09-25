"""Small shared helpers: hashing, time, bytes, paths, colored output."""

from __future__ import annotations

import os
import re
import sys
import time
from datetime import datetime, timezone, timedelta


# --------------------------------------------------------------------------
# errors
# --------------------------------------------------------------------------

class GitPromptError(Exception):
    """Any user-facing failure. The CLI turns this into `gitprompt: <msg>`."""

    exit_code = 1


class ObjectNotFound(GitPromptError):
    pass


class RepoNotFound(GitPromptError):
    """Raised when we walk up from cwd and find no .gitprompt directory."""

    def __init__(self, start=None):
        super().__init__(
            "not a gitprompt repository (or any of the parent directories): .gitprompt"
        )


# --------------------------------------------------------------------------
# time
# --------------------------------------------------------------------------

def tz_offset_minutes() -> int:
    """Local UTC offset in minutes, DST-aware, measured the way git measures it."""
    delta = datetime.now(timezone.utc).astimezone().utcoffset() or timedelta(0)
    return int(delta.total_seconds() // 60)


def format_tz(minutes: int) -> str:
    """git's `+0800` style timezone string."""
    sign = "+" if minutes >= 0 else "-"
    minutes = abs(minutes)
    return f"{sign}{minutes // 60:02d}{minutes % 60:02d}"


def now_rfc3339() -> str:
    """ISO-8601 with the local offset, e.g. 2026-09-26T10:14:22+08:00.

    This is the canonical timestamp form stored on prompts. It keeps the wall
    clock the prompt was actually written at, which is what `replay` needs to
    reconstruct the author's real timeline across sessions and machines.
    """
    off = tz_offset_minutes()
    tz = timezone(timedelta(minutes=off))
    return datetime.now(tz).replace(microsecond=0).isoformat()


def parse_rfc3339(value: str) -> datetime:
    """Parse the timestamp forms we and users may write.

    Accepts `Z`, `+08:00`, `+0800`, and a bare `YYYY-MM-DD HH:MM:SS`.
    Naive input is assumed to be local time.
    """
    s = value.strip()
    if s.endswith("Z"):
        s = s[:-1] + "+00:00"
    s = re.sub(r"([+-]\d{2})(\d{2})$", r"\1:\2", s)
    for fmt in ("%Y-%m-%dT%H:%M:%S%z", "%Y-%m-%d %H:%M:%S%z", "%Y-%m-%dT%H:%M:%S", "%Y-%m-%d %H:%M:%S", "%Y-%m-%d"):
        try:
            dt = datetime.strptime(s, fmt)
            return dt if dt.tzinfo else dt.astimezone()
        except ValueError:
            continue
    raise GitPromptError(f"cannot parse timestamp: {value!r}")


def now_unix() -> int:
    return int(time.time())


def today_stamp() -> str:
    return datetime.now().strftime("%Y%m%d")


# --------------------------------------------------------------------------
# bytes / text
# --------------------------------------------------------------------------

def to_bytes(value) -> bytes:
    if isinstance(value, bytes):
        return value
    return str(value).encode("utf-8")


def to_text(value) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value


def hex_to_bytes(hexstr: str) -> bytes:
    return bytes.fromhex(hexstr)


def bytes_to_hex(raw: bytes) -> str:
    return raw.hex()


# --------------------------------------------------------------------------
# paths
# --------------------------------------------------------------------------

def normalize_path(path: str) -> str:
    """Repo-relative, forward-slashed, no leading `./`. Mirrors git's rules."""
    p = path.replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]
    p = re.sub(r"/+", "/", p)
    if p.endswith("/"):
        p = p.rstrip("/")
    if p in ("", "."):
        return "."
    if p.startswith("../") or p == "..":
        raise GitPromptError(f"path '{path}' is outside the repository")
    return p


def join_path(*parts: str) -> str:
    return "/".join(p.strip("/") for p in parts if p not in (None, "", "."))


def path_join_fs(*parts: str) -> str:
    return os.path.join(*[p for p in parts if p not in (None, "")])


# --------------------------------------------------------------------------
# terminal
# --------------------------------------------------------------------------

_COLOR = None


def color_enabled(stream=None) -> bool:
    global _COLOR
    if _COLOR is None:
        mode = os.environ.get("GITPROMPT_COLOR") or os.environ.get("NO_COLOR")
        if mode in ("0", "never") or os.environ.get("NO_COLOR") is not None:
            _COLOR = False
        else:
            _COLOR = (stream or sys.stdout).isatty()
    return _COLOR


def paint(text: str, color: str) -> str:
    if not color_enabled():
        return text
    codes = {
        "red": "31", "green": "32", "yellow": "33",
        "blue": "34", "magenta": "35", "cyan": "36",
        "bold": "1", "dim": "2", "reset": "0",
    }
    return f"\033[{codes.get(color, '0')}m{text}\033[0m"


def warn(msg: str) -> None:
    sys.stderr.write(paint(f"warning: {msg}", "yellow") + "\n")


def die(msg: str, code: int = 1) -> None:
    raise GitPromptError(msg)


# --------------------------------------------------------------------------
# ids
# --------------------------------------------------------------------------

_B32 = "0123456789abcdefghjkmnpqrstvwxyz"  # no i/l/o/u — readable, no lookalikes

_HEX40 = re.compile(r"[0-9a-f]{40}")


def is_valid_hash(value: str) -> bool:
    return bool(_HEX40.fullmatch(value or ""))


def short_id(n: int = 8) -> str:
    """Short, sortable-ish, human-transcribable identifier suffix."""
    raw = os.urandom(n)
    return "".join(_B32[b % 32] for b in raw)
