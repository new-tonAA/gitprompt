"""The object store — content-addressed, zlib-compressed, exactly like git's.

Six object types live here:

  blob      arbitrary bytes (attachments, non-prompt files)
  tree      a directory snapshot: sorted (mode, name, hash) entries
  commit    a point in history: tree + parents + authorship + session pointer
  tag       an annotated ref
  prompt    one prompt, as canonical JSON with its full provenance
  session   a grouping of prompts that were authored together in one sitting

`prompt` and `session` are the gitprompt-specific extensions.  They are what
lets `gitprompt replay` rebuild a project's *timeline* rather than just its
final file contents.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import zlib

from . import GP_VERSION
from .utils import (
    GitPromptError,
    ObjectNotFound,
    bytes_to_hex,
    hex_to_bytes,
    is_valid_hash,
    join_path,
    normalize_path,
    to_bytes,
    to_text,
)

# Object type identifiers, as they appear in the object header.
TYPE_BLOB = "blob"
TYPE_TREE = "tree"
TYPE_COMMIT = "commit"
TYPE_TAG = "tag"
TYPE_PROMPT = "prompt"
TYPE_SESSION = "session"

VALID_TYPES = (TYPE_BLOB, TYPE_TREE, TYPE_COMMIT, TYPE_TAG, TYPE_PROMPT, TYPE_SESSION)

# Tree entry modes.  The first four are git's; 100640 is ours, marking an
# entry whose object is a `prompt` rather than a `blob` so that a tree walk
# never has to guess from the file extension.
MODE_TREE = 0o040000
MODE_BLOB = 0o100644
MODE_BLOB_EXEC = 0o100755
MODE_LINK = 0o120000
MODE_PROMPT = 0o100640

MODE_NAMES = {
    MODE_TREE: "040000",
    MODE_BLOB: "100644",
    MODE_BLOB_EXEC: "100755",
    MODE_LINK: "120000",
    MODE_PROMPT: "100640",
}

_HASH_BYTES = 20  # sha1


# --------------------------------------------------------------------------
# hashing
# --------------------------------------------------------------------------

def frame(objtype: str, payload: bytes) -> bytes:
    """`"<type> <len>\\0" + payload` — the bytes that are hashed *and* stored."""
    return to_bytes(f"{objtype} {len(payload)}\0") + payload


def hash_object(objtype: str, payload: bytes, fmt: str = "sha1") -> str:
    """`sha1("<type> <len>\\0" + payload)` — byte-identical to git's scheme."""
    h = hashlib.new(fmt)
    h.update(frame(objtype, payload))
    return h.hexdigest()


# --------------------------------------------------------------------------
# blob
# --------------------------------------------------------------------------

def blob_payload(data) -> bytes:
    return to_bytes(data)


# --------------------------------------------------------------------------
# tree
# --------------------------------------------------------------------------

def encode_tree(entries) -> bytes:
    """entries: iterable of (mode:int, name:str, hexsha:str).

    Serialised in git's on-disk order: entries compare by name, except that a
    directory sorts as though its name ended in `/`.  Reproducing that rule
    exactly means our trees hash the same way git's do for the same content.
    """
    out = bytearray()
    for mode, name, sha in sorted(entries, key=lambda e: _tree_sort_key(e[1], e[0])):
        out += to_bytes(f"{MODE_NAMES.get(mode, oct(mode)[2:])} {name}\0")
        out += hex_to_bytes(sha)
    return bytes(out)


def _tree_sort_key(name: str, mode: int) -> bytes:
    raw = to_bytes(name)
    return raw + b"/" if mode == MODE_TREE else raw


def decode_tree(payload: bytes):
    """Yield (mode:int, name:str, hexsha:str) from a raw tree payload."""
    i = 0
    n = len(payload)
    while i < n:
        sp = payload.index(b" ", i)
        mode = int(payload[i:sp], 8)
        nul = payload.index(b"\0", sp)
        name = to_text(payload[sp + 1:nul])
        sha = bytes_to_hex(payload[nul + 1:nul + 1 + _HASH_BYTES])
        yield mode, name, sha
        i = nul + 1 + _HASH_BYTES


# --------------------------------------------------------------------------
# commit
# --------------------------------------------------------------------------

class Commit:
    __slots__ = ("tree", "parents", "author", "committer", "session",
                 "session_seq", "model", "extra", "message", "sha")

    def __init__(self, tree, parents=None, author="", committer="",
                 session=None, session_seq=None, model=None,
                 extra=None, message="", sha=None):
        self.tree = tree
        self.parents = list(parents or [])
        self.author = author
        self.committer = committer
        self.session = session
        self.session_seq = session_seq
        self.model = model
        self.extra = dict(extra or {})
        self.message = message
        self.sha = sha

    def encode(self) -> bytes:
        lines = [f"tree {self.tree}"]
        for p in self.parents:
            lines.append(f"parent {p}")
        lines.append(f"author {self.author}")
        lines.append(f"committer {self.committer}")
        if self.session:
            lines.append(f"session {self.session}")
        if self.session_seq is not None:
            lines.append(f"session-seq {self.session_seq}")
        if self.model:
            lines.append(f"model {self.model}")
        lines.append(f"gitprompt-version {GP_VERSION}")
        for k, v in sorted(self.extra.items()):
            lines.append(f"{k} {v}")
        body = "\n".join(lines) + "\n\n" + self.message
        if not body.endswith("\n"):
            body += "\n"
        return to_bytes(body)

    @classmethod
    def decode(cls, payload, sha=None):
        text = to_text(payload)
        head, _, message = text.partition("\n\n")
        tree = ""
        parents, extra = [], {}
        author = committer = ""
        session = model = None
        seq = None
        for line in head.split("\n"):
            if not line:
                continue
            key, _, value = line.partition(" ")
            if key == "tree":
                tree = value
            elif key == "parent":
                parents.append(value)
            elif key == "author":
                author = value
            elif key == "committer":
                committer = value
            elif key == "session":
                session = value
            elif key == "session-seq":
                seq = int(value)
            elif key == "model":
                model = value
            elif key in ("gitprompt-version", "gp-version"):
                extra[key] = value
            else:
                extra[key] = value
        return cls(tree, parents, author, committer, session, seq, model,
                   extra, message, sha)


# --------------------------------------------------------------------------
# prompt
# --------------------------------------------------------------------------

class Prompt:
    """One prompt plus everything needed to place it on a timeline.

    `session` + `seq` fix its position within a sitting; `timestamp` fixes its
    wall-clock position across sittings.  `parent_prompt` records which prompt
    this one was written in reaction to, which is how branching conversations
    stay reconstructable.
    """

    __slots__ = ("id", "body", "session", "seq", "timestamp", "author",
                 "model", "tags", "outcome", "attachments", "parent_prompt",
                 "meta", "sha")

    def __init__(self, body, id=None, session=None, seq=None, timestamp=None,
                 author=None, model=None, tags=None, outcome=None,
                 attachments=None, parent_prompt=None, meta=None, sha=None):
        self.id = id or ""
        self.body = body
        self.session = session
        self.seq = seq
        self.timestamp = timestamp
        self.author = author or {}
        self.model = model
        self.tags = list(tags or [])
        self.outcome = outcome
        self.attachments = list(attachments or [])
        self.parent_prompt = parent_prompt
        self.meta = dict(meta or {})
        self.sha = sha

    def to_dict(self) -> dict:
        d = {
            "v": GP_VERSION,
            "id": self.id,
            "body": self.body,
        }
        if self.session:
            d["session"] = self.session
        if self.seq is not None:
            d["seq"] = self.seq
        if self.timestamp:
            d["timestamp"] = self.timestamp
        if self.author:
            d["author"] = self.author
        if self.model:
            d["model"] = self.model
        if self.tags:
            d["tags"] = self.tags
        if self.outcome:
            d["outcome"] = self.outcome
        if self.attachments:
            d["attachments"] = self.attachments
        if self.parent_prompt:
            d["parent_prompt"] = self.parent_prompt
        if self.meta:
            d["meta"] = self.meta
        return d

    def encode(self) -> bytes:
        # Canonical JSON: sorted keys, tight separators, UTF-8 preserved.
        # Determinism here is what makes the content hash meaningful.
        return json.dumps(
            self.to_dict(), sort_keys=True, separators=(",", ":"),
            ensure_ascii=False,
        ).encode("utf-8")

    @classmethod
    def decode(cls, payload, sha=None):
        d = json.loads(to_text(payload))
        return cls(
            body=d.get("body", ""),
            id=d.get("id", ""),
            session=d.get("session"),
            seq=d.get("seq"),
            timestamp=d.get("timestamp"),
            author=d.get("author") or {},
            model=d.get("model"),
            tags=d.get("tags"),
            outcome=d.get("outcome"),
            attachments=d.get("attachments"),
            parent_prompt=d.get("parent_prompt"),
            meta=d.get("meta"),
            sha=sha,
        )

    def sort_key(self):
        """Chronological key.  Timestamp leads because sessions can overlap
        across machines; seq breaks ties inside one sitting."""
        return (self.timestamp or "", self.session or "", self.seq or 0, self.id)


# --------------------------------------------------------------------------
# tag
# --------------------------------------------------------------------------

class Tag:
    """An annotated ref: a name, a message, and the object it points at.

    A tag is a real object rather than a bare pointer so it can carry a
    message and a tagger, which is what makes `describe` able to report a
    human name for a commit.
    """

    __slots__ = ("object", "type", "tag", "tagger", "message", "sha")

    def __init__(self, object, type=TYPE_COMMIT, tag="", tagger="",
                 message="", sha=None):
        self.object = object
        self.type = type
        self.tag = tag
        self.tagger = tagger
        self.message = message
        self.sha = sha

    def to_dict(self) -> dict:
        d = {"v": GP_VERSION, "object": self.object, "type": self.type,
             "tag": self.tag}
        if self.tagger:
            d["tagger"] = self.tagger
        if self.message:
            d["message"] = self.message
        return d

    def encode(self) -> bytes:
        return json.dumps(
            self.to_dict(), sort_keys=True, separators=(",", ":"),
            ensure_ascii=False,
        ).encode("utf-8")

    @classmethod
    def decode(cls, payload, sha=None):
        d = json.loads(to_text(payload))
        return cls(
            object=d.get("object", ""),
            type=d.get("type", TYPE_COMMIT),
            tag=d.get("tag", ""),
            tagger=d.get("tagger", ""),
            message=d.get("message", ""),
            sha=sha,
        )


# --------------------------------------------------------------------------
# session
# --------------------------------------------------------------------------

class Session:
    """A contiguous stretch of prompting — one sitting, one conversation.

    Sessions are what make cross-session reconstruction possible: they mark
    where an agent's context was reset, so a replay can re-establish state at
    exactly the same boundaries the original work had.
    """

    __slots__ = ("id", "title", "started_at", "ended_at", "author", "model",
                 "prompts", "tags", "notes", "meta", "sha")

    def __init__(self, id, title="", started_at=None, ended_at=None,
                 author=None, model=None, prompts=None, tags=None,
                 notes=None, meta=None, sha=None):
        self.id = id
        self.title = title
        self.started_at = started_at
        self.ended_at = ended_at
        self.author = author or {}
        self.model = model
        self.prompts = list(prompts or [])   # prompt object hashes, in order
        self.tags = list(tags or [])
        self.notes = notes
        self.meta = dict(meta or {})
        self.sha = sha

    def to_dict(self) -> dict:
        d = {
            "v": GP_VERSION,
            "id": self.id,
            "title": self.title,
            "started_at": self.started_at,
        }
        if self.ended_at:
            d["ended_at"] = self.ended_at
        if self.author:
            d["author"] = self.author
        if self.model:
            d["model"] = self.model
        if self.prompts:
            d["prompts"] = self.prompts
        if self.tags:
            d["tags"] = self.tags
        if self.notes:
            d["notes"] = self.notes
        if self.meta:
            d["meta"] = self.meta
        return d

    def encode(self) -> bytes:
        return json.dumps(
            self.to_dict(), sort_keys=True, separators=(",", ":"),
            ensure_ascii=False,
        ).encode("utf-8")

    @classmethod
    def decode(cls, payload, sha=None):
        d = json.loads(to_text(payload))
        return cls(
            id=d.get("id", ""),
            title=d.get("title", ""),
            started_at=d.get("started_at"),
            ended_at=d.get("ended_at"),
            author=d.get("author") or {},
            model=d.get("model"),
            prompts=d.get("prompts"),
            tags=d.get("tags"),
            notes=d.get("notes"),
            meta=d.get("meta"),
            sha=sha,
        )


# --------------------------------------------------------------------------
# serialisation dispatch
# --------------------------------------------------------------------------

def serialize(objtype: str, obj) -> bytes:
    if objtype == TYPE_BLOB:
        return blob_payload(obj)
    if objtype == TYPE_TREE:
        return encode_tree(obj)
    if objtype == TYPE_COMMIT:
        return obj.encode()
    if objtype == TYPE_PROMPT:
        return obj.encode()
    if objtype == TYPE_SESSION:
        return obj.encode()
    if objtype == TYPE_TAG:
        return obj.encode() if hasattr(obj, "encode") else to_bytes(obj)
    raise GitPromptError(f"unknown object type {objtype!r}")


def make_tag(target_sha, name, tagger="", message="", target_type=TYPE_COMMIT):
    return Tag(target_sha, target_type, name, tagger, message)


def deserialize(objtype: str, payload: bytes, sha: str = None):
    if objtype == TYPE_BLOB:
        return payload
    if objtype == TYPE_TREE:
        return list(decode_tree(payload))
    if objtype == TYPE_COMMIT:
        return Commit.decode(payload, sha)
    if objtype == TYPE_PROMPT:
        return Prompt.decode(payload, sha)
    if objtype == TYPE_SESSION:
        return Session.decode(payload, sha)
    if objtype == TYPE_TAG:
        return Tag.decode(payload, sha)
    raise GitPromptError(f"unknown object type {objtype!r}")


# --------------------------------------------------------------------------
# the store
# --------------------------------------------------------------------------

class ObjectStore:
    """Reads and writes loose objects under `<gpdir>/objects/`."""

    def __init__(self, gpdir: str, fmt: str = "sha1"):
        self.gpdir = gpdir
        self.fmt = fmt
        self.objects_dir = os.path.join(gpdir, "objects")
        self.alternates = self._read_alternates()

    # -- layout ------------------------------------------------------------

    def _read_alternates(self):
        """`objects/info/alternates` — lets a clone borrow objects instead of
        copying them, same trick git uses for local clones."""
        path = os.path.join(self.objects_dir, "info", "alternates")
        if not os.path.exists(path):
            return []
        out = []
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if line and not line.startswith("#"):
                    out.append(os.path.normpath(os.path.join(self.objects_dir, line)))
        return out

    def object_path(self, sha: str) -> str:
        return os.path.join(self.objects_dir, sha[:2], sha[2:])

    def _candidate_dirs(self):
        yield self.objects_dir
        for alt in self.alternates:
            yield alt

    # -- write -------------------------------------------------------------

    def write(self, objtype: str, obj) -> str:
        """Serialise, hash and persist.  Returns the hex sha.

        Idempotent by construction: identical content is already on disk and
        is not rewritten, which is what lets two people arrive at the same
        prompt history and get the same hashes.
        """
        return self.write_raw(objtype, serialize(objtype, obj))

    def write_raw(self, objtype: str, payload: bytes) -> str:
        sha = hash_object(objtype, payload, self.fmt)
        path = self.object_path(sha)
        if os.path.exists(path):
            return sha
        os.makedirs(os.path.dirname(path), exist_ok=True)
        # Write to a temp file and rename, so a crash mid-write can never
        # leave a half-written object under a name that claims it is complete.
        tmp = path + f".tmp{os.getpid()}"
        with open(tmp, "wb") as fh:
            fh.write(zlib.compress(frame(objtype, payload), 1))
        os.replace(tmp, path)
        return sha

    # -- read --------------------------------------------------------------

    def read_raw(self, sha: str):
        """Return (objtype, payload) or raise ObjectNotFound."""
        for base in self._candidate_dirs():
            path = os.path.join(base, sha[:2], sha[2:])
            if os.path.exists(path):
                with open(path, "rb") as fh:
                    data = zlib.decompress(fh.read())
                nul = data.index(b"\0")
                header = data[:nul].decode("ascii")
                objtype, _, _size = header.partition(" ")
                return objtype, data[nul + 1:]
        raise ObjectNotFound(f"object {sha} not found")

    def read(self, sha: str):
        objtype, payload = self.read_raw(sha)
        return deserialize(objtype, payload, sha)

    def type_of(self, sha: str) -> str:
        return self.read_raw(sha)[0]

    def exists(self, sha: str) -> bool:
        for base in self._candidate_dirs():
            if os.path.exists(os.path.join(base, sha[:2], sha[2:])):
                return True
        return False

    # -- enumeration -------------------------------------------------------

    def iter_loose(self):
        """Yield every loose object sha in this store (not alternates)."""
        for sub in os.listdir(self.objects_dir) if os.path.isdir(self.objects_dir) else []:
            if len(sub) != 2:
                continue
            subdir = os.path.join(self.objects_dir, sub)
            if not os.path.isdir(subdir):
                continue
            for name in os.listdir(subdir):
                if len(name) == 38:
                    yield sub + name

    def count(self) -> int:
        return sum(1 for _ in self.iter_loose())


# --------------------------------------------------------------------------
# resolvers
# --------------------------------------------------------------------------

HEX_RE = re.compile(r"^[0-9a-f]{4,64}$")


def resolve_prefix(store: ObjectStore, prefix: str) -> str:
    """Expand an abbreviated sha, the way `git rev-parse` does.

    Ambiguity is an error rather than a silent pick — guessing here would let
    a replay silently diverge from the history it claims to reproduce.
    """
    if len(prefix) >= 40 and store.exists(prefix):
        return prefix
    if not HEX_RE.match(prefix):
        raise GitPromptError(f"not a valid object name: {prefix}")
    matches = [sha for sha in store.iter_loose() if sha.startswith(prefix)]
    if not matches:
        raise ObjectNotFound(f"object {prefix} not found")
    if len(matches) > 1:
        raise GitPromptError(f"short object ID {prefix} is ambiguous")
    return matches[0]
