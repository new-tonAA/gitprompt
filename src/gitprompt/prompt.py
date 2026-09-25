"""The on-disk prompt format: Markdown with YAML frontmatter.

A prompt file is the human-facing form of a `prompt` object:

    ---
    id: p_k3m9x2qa
    session: s_20260926_a3f2
    seq: 3
    timestamp: 2026-09-26T10:14:22+08:00
    author: new-tonAA <newtonaa@qq.com>
    model: claude-sonnet-5
    tags: [scaffold, cli]
    ---
    Build the CLI entry point so that `gitprompt push` ...

The frontmatter carries the provenance that makes replay possible; the body is
the prompt itself.  Only a focused YAML subset is supported — enough for
scalars, inline lists, block lists and block scalars, and deliberately no
more, so that a prompt file stays something a human writes by hand.
"""

from __future__ import annotations

import re

from .objects import Prompt
from .utils import GitPromptError, now_rfc3339, parse_rfc3339, short_id

DELIM = "---"

# Fields we own and serialise in a fixed order.  Anything else the user adds
# is preserved under `meta` rather than dropped.
KNOWN_ORDER = [
    "id", "session", "seq", "timestamp", "author", "model",
    "tags", "outcome", "parent_prompt", "attachments",
]
LIST_FIELDS = {"tags", "attachments"}


class PromptFileError(GitPromptError):
    pass


# --------------------------------------------------------------------------
# minimal YAML subset
# --------------------------------------------------------------------------

def _unquote(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        inner = value[1:-1]
        if value[0] == '"':
            return (inner.replace("\\n", "\n").replace("\\t", "\t")
                         .replace('\\"', '"').replace("\\\\", "\\"))
        return inner
    return value


def _parse_inline_list(value: str):
    inner = value.strip()[1:-1].strip()
    if not inner:
        return []
    out, buf, quote = [], "", None
    for ch in inner:
        if quote:
            buf += ch
            if ch == quote:
                quote = None
            continue
        if ch in "\"'":
            quote = ch
            buf += ch
        elif ch == ",":
            out.append(_unquote(buf))
            buf = ""
        else:
            buf += ch
    if buf.strip():
        out.append(_unquote(buf))
    return [x for x in out if x != ""]


def parse_frontmatter(text: str):
    """Return (metadata_dict, body_text).  Raises when the file is malformed."""
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    lines = text.split("\n")
    if not lines or lines[0].strip() != DELIM:
        return {}, text
    end = None
    for i in range(1, len(lines)):
        if lines[i].strip() == DELIM:
            end = i
            break
    if end is None:
        raise PromptFileError("frontmatter is opened with --- but never closed")
    meta = _parse_block(lines[1:end])
    body = "\n".join(lines[end + 1:])
    if body.startswith("\n"):
        body = body[1:]
    return meta, body


def _parse_block(lines):
    meta = {}
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line.strip() or line.lstrip().startswith("#"):
            i += 1
            continue
        if line.startswith((" ", "\t")):
            raise PromptFileError(f"unexpected indentation in frontmatter: {line!r}")
        if ":" not in line:
            raise PromptFileError(f"frontmatter line is not 'key: value': {line!r}")
        key, _, rest = line.partition(":")
        key = key.strip()
        rest = rest.strip()
        if rest == "|" or rest == ">":
            folded = rest == ">"
            block, i = _read_block_scalar(lines, i + 1)
            meta[key] = " ".join(block.split()) if folded else "\n".join(block)
            continue
        if rest == "":
            # could be a block list, or an empty value
            items, i = _read_block_list(lines, i + 1)
            meta[key] = items if items else ""
            continue
        if rest.startswith("[") and rest.endswith("]"):
            meta[key] = _parse_inline_list(rest)
        else:
            meta[key] = _unquote(rest)
        i += 1
    return meta


def _read_block_scalar(lines, i):
    out = []
    while i < len(lines):
        line = lines[i]
        if line.strip() == "" :
            out.append("")
            i += 1
            continue
        if not line.startswith((" ", "\t")):
            break
        out.append(line[2:] if line[:2] in ("  ", "\t") else line.lstrip())
        i += 1
    while out and out[-1] == "":
        out.pop()
    return out, i


def _read_block_list(lines, i):
    out = []
    while i < len(lines):
        line = lines[i]
        if not line.strip():
            i += 1
            continue
        stripped = line.lstrip()
        if not stripped.startswith("- "):
            break
        out.append(_unquote(stripped[2:]))
        i += 1
    return out, i


def _render_value(key, value) -> list:
    if isinstance(value, list):
        return [f"{key}: [{', '.join(str(v) for v in value)}]"] if value else []
    if isinstance(value, str) and "\n" in value:
        return [f"{key}: |"] + ["  " + l for l in value.split("\n")]
    return [f"{key}: {value}"]


def render_frontmatter(meta: dict) -> str:
    lines = [DELIM]
    seen = set()
    for key in KNOWN_ORDER:
        if key in meta and meta[key] not in (None, "", []):
            lines += _render_value(key, meta[key])
            seen.add(key)
    for key in sorted(meta):
        if key in seen or key in ("v", "body") or meta[key] in (None, "", []):
            continue
        lines += _render_value(key, meta[key])
    lines.append(DELIM)
    return "\n".join(lines)


# --------------------------------------------------------------------------
# file <-> object
# --------------------------------------------------------------------------

def parse_author(raw):
    """`Name <email>` → dict.  A bare string is treated as a name."""
    if not raw:
        return {}
    if isinstance(raw, dict):
        return raw
    m = re.match(r"^(.*?)\s*<([^>]*)>\s*$", str(raw))
    if m:
        return {"name": m.group(1).strip(), "email": m.group(2).strip()}
    return {"name": str(raw).strip()}


def format_author(author) -> str:
    if not author:
        return ""
    if isinstance(author, str):
        return author
    name, email = author.get("name", ""), author.get("email", "")
    return f"{name} <{email}>" if email else name


def file_to_prompt(text: str, path: str = None, fallback_author=None,
                   fallback_session=None, fallback_seq=None) -> Prompt:
    """Parse a prompt file into a Prompt object.

    Anything the file omits is filled from the caller's context, so a
    hand-written two-line prompt file still lands on the timeline with a real
    timestamp and session instead of a null one.
    """
    meta, body = parse_frontmatter(text)
    if not body.strip():
        raise PromptFileError(
            f"{path or 'prompt'}: the prompt body is empty "
            "(frontmatter is present but there is no prompt text)"
        )
    tags = meta.get("tags") or []
    if isinstance(tags, str):
        tags = [t.strip() for t in tags.split(",") if t.strip()]
    attachments = meta.get("attachments") or []
    if isinstance(attachments, str):
        attachments = [attachments]
    extra = {
        k: v for k, v in meta.items()
        if k not in KNOWN_ORDER and k not in ("v", "body")
    }
    timestamp = meta.get("timestamp") or None
    if timestamp:
        parse_rfc3339(timestamp)      # validate early, fail with a clear message
    return Prompt(
        body=body.rstrip("\n") + "\n",
        id=meta.get("id") or f"p_{short_id(8)}",
        session=meta.get("session") or fallback_session,
        seq=int(meta["seq"]) if str(meta.get("seq") or "").isdigit() else fallback_seq,
        timestamp=timestamp or now_rfc3339(),
        author=parse_author(meta.get("author")) or dict(fallback_author or {}),
        model=meta.get("model") or None,
        tags=list(tags),
        outcome=meta.get("outcome") or None,
        attachments=list(attachments),
        parent_prompt=meta.get("parent_prompt") or None,
        meta=extra,
    )


def prompt_to_file(prompt: Prompt) -> str:
    """Serialise a Prompt back to file text.

    Round-trips exactly: `prompt_to_file(file_to_prompt(t)) == t` for any file
    this module wrote, which is what lets `checkout` restore the working tree
    byte-for-byte from objects alone.
    """
    meta = dict(prompt.meta)
    meta.update({
        "id": prompt.id,
        "session": prompt.session,
        "seq": prompt.seq,
        "timestamp": prompt.timestamp,
        "author": format_author(prompt.author) or None,
        "model": prompt.model,
        "tags": prompt.tags,
        "outcome": prompt.outcome,
        "parent_prompt": prompt.parent_prompt,
        "attachments": prompt.attachments,
    })
    fm = render_frontmatter(meta)
    body = prompt.body if prompt.body.endswith("\n") else prompt.body + "\n"
    return fm + "\n" + body


# --------------------------------------------------------------------------
# naming
# --------------------------------------------------------------------------

_SLUG_STRIP = re.compile(r"[^a-z0-9]+")


def slugify(text: str, maxlen: int = 40) -> str:
    """First meaningful words of a prompt → a filename-safe slug."""
    first = text.strip().split("\n", 1)[0]
    slug = _SLUG_STRIP.sub("-", first.lower()).strip("-")
    return (slug[:maxlen].rstrip("-")) or "prompt"


def prompt_filename(seq, slug: str) -> str:
    """`003-fix-the-parser.md` — numeric prefix so a directory listing is
    already in reading order even without frontmatter."""
    return f"{int(seq):03d}-{slug}.md"
