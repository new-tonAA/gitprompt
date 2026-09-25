"""The staging area, in a binary format modelled on git's index.

Header `GPIX`, then a big-endian version and entry count, then one record per
staged path carrying the same stat block git records.  Keeping the stat block
matters for the same reason it does in git: `status` can then decide
"unchanged" from size/mtime alone instead of re-hashing every file.
"""

from __future__ import annotations

import os
import stat as _stat
import struct

from .utils import GitPromptError, bytes_to_hex, hex_to_bytes

MAGIC = b"GPIX"
VERSION = 2
SIGNATURE = struct.Struct(">4sII")            # magic, version, count
# git's v2 stat block: ctime(sec,nsec) mtime(sec,nsec) dev ino mode uid gid size
STAT = struct.Struct(">10I")
ENTRY_FIXED = struct.Struct(">40s20sH")       # stat block, sha, flags
FLAG_STAGE_MASK = 0x3000
FLAG_NAMELEN_MASK = 0x0FFF

# indices into the stat tuple
STAT_MODE = 6
STAT_MTIME_SEC = 2
STAT_SIZE = 9

MODE_FILE = 0o100644
MODE_EXEC = 0o100755
MODE_LINK = 0o120000
MODE_TREE = 0o040000


def git_mode_of(st_mode: int) -> int:
    """Normalise a filesystem `st_mode` to the small set of modes git records.

    The stat block carries the *git* mode (0100644, 0100755, 0120000), not the
    raw `st_mode`: 0100666 from a filesystem means nothing to a tree and would
    be written into one verbatim.
    """
    if _stat.S_ISLNK(st_mode):
        return MODE_LINK
    if _stat.S_ISDIR(st_mode):
        return MODE_TREE
    return MODE_EXEC if st_mode & 0o111 else MODE_FILE


class IndexEntry:
    __slots__ = ("path", "sha", "mode", "stage", "stat")

    def __init__(self, path, sha, mode=0o100644, stage=0, stat=None):
        self.path = path
        self.sha = sha
        self.mode = mode
        self.stage = stage
        self.stat = stat or ZERO_STAT

    def __repr__(self):
        return f"<IndexEntry {self.path} {self.mode:o} {self.sha[:8]}>"


ZERO_STAT = (0,) * 10


def stat_of(path: str):
    try:
        st = os.lstat(path)
    except OSError:
        return ZERO_STAT
    return (
        int(st.st_ctime) & 0xFFFFFFFF, int(st.st_ctime_ns % 1_000_000_000),
        int(st.st_mtime) & 0xFFFFFFFF, int(st.st_mtime_ns % 1_000_000_000),
        int(getattr(st, "st_dev", 0)) & 0xFFFFFFFF, int(getattr(st, "st_ino", 0)) & 0xFFFFFFFF,
        git_mode_of(st.st_mode), int(getattr(st, "st_uid", 0)) & 0xFFFFFFFF,
        int(getattr(st, "st_gid", 0)) & 0xFFFFFFFF, int(st.st_size) & 0xFFFFFFFF,
    )


class Index:
    """Sorted-by-path set of staged entries.  `index.add()` and `remove()` are
    in-memory; call `write()` to persist."""

    def __init__(self, path: str = None):
        self.path = path
        self.entries = {}       # path -> IndexEntry
        self.version = VERSION
        self._parsed_stat = {}  # path -> stat tuple at read time, for change detection

    # -- IO ----------------------------------------------------------------

    @classmethod
    def read(cls, path: str) -> "Index":
        idx = cls(path)
        if not path or not os.path.exists(path):
            return idx
        with open(path, "rb") as fh:
            data = fh.read()
        if len(data) < SIGNATURE.size:
            return idx
        magic, version, count = SIGNATURE.unpack_from(data, 0)
        if magic != MAGIC:
            raise GitPromptError(f"index file corrupt: bad signature {magic!r}")
        if version not in (1, 2, 3):
            raise GitPromptError(f"index file corrupt: unsupported version {version}")
        idx.version = version
        off = SIGNATURE.size
        for _ in range(count):
            if off + ENTRY_FIXED.size > len(data):
                raise GitPromptError("index file corrupt: truncated entry")
            statblob, shabin, flags = ENTRY_FIXED.unpack_from(data, off)
            off += ENTRY_FIXED.size
            statblock = STAT.unpack(statblob)
            namelen = flags & FLAG_NAMELEN_MASK
            # v2+ pads entries to a multiple of 8 bytes; when the name is long
            # enough that it no longer fits in 12 bits, it is stored as NUL.
            if namelen == FLAG_NAMELEN_MASK:
                end = data.index(b"\0", off)
                name = data[off:end]
            else:
                name = data[off:off + namelen]
                end = off + namelen
            off = end + 1
            while (off - SIGNATURE.size) % 8:
                off += 1
            entry = IndexEntry(
                path=name.decode("utf-8"),
                sha=bytes_to_hex(shabin),
                mode=statblock[6],
                stage=(flags & FLAG_STAGE_MASK) >> 12,
                stat=statblock,
            )
            idx.entries[entry.path] = entry
            idx._parsed_stat[entry.path] = statblock
        return idx

    def write(self, path: str = None) -> None:
        path = path or self.path
        if not path:
            raise GitPromptError("index has no path")
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        out = bytearray()
        out += SIGNATURE.pack(MAGIC, self.version, len(self.entries))
        for entry in self.sorted_entries():
            name = entry.path.encode("utf-8")
            namelen = min(len(name), FLAG_NAMELEN_MASK)
            flags = ((entry.stage << 12) & FLAG_STAGE_MASK) | namelen
            # The stat block's mode field is where the git mode lives, so the
            # entry's own mode wins over whatever the filesystem reported.
            block = list(entry.stat or ZERO_STAT)
            block[STAT_MODE] = entry.mode
            out += ENTRY_FIXED.pack(STAT.pack(*block), hex_to_bytes(entry.sha), flags)
            out += name + b"\0"
            while (len(out) - SIGNATURE.size) % 8:
                out += b"\0"
        tmp = path + f".lock{os.getpid()}"
        with open(tmp, "wb") as fh:
            fh.write(bytes(out))
        os.replace(tmp, path)

    # -- access ------------------------------------------------------------

    def sorted_entries(self):
        return [self.entries[p] for p in sorted(self.entries)]

    def get(self, path: str):
        return self.entries.get(path)

    def __contains__(self, path: str) -> bool:
        return path in self.entries

    def __len__(self) -> int:
        return len(self.entries)

    def __iter__(self):
        return iter(self.sorted_entries())

    def paths(self):
        return set(self.entries)

    # -- mutation ----------------------------------------------------------

    def add(self, path: str, sha: str, mode: int = 0o100644, stat=None, stage: int = 0):
        self.entries[path] = IndexEntry(path, sha, mode, stage, stat)

    def remove(self, path: str) -> bool:
        return self.entries.pop(path, None) is not None

    def remove_dir(self, prefix: str) -> int:
        prefix = prefix.rstrip("/") + "/"
        doomed = [p for p in self.entries if p.startswith(prefix)]
        for p in doomed:
            del self.entries[p]
        return len(doomed)

    # -- change detection --------------------------------------------------

    def is_unchanged_on_disk(self, repo_root: str, entry: IndexEntry) -> bool:
        """True when size and mtime both match what was recorded at add time.

        This is git's stat cache: a cheap "probably identical" answer.  Callers
        that need certainty call `repo.hash_worktree_file` instead.
        """
        if entry.stat == ZERO_STAT:
            return False
        try:
            st = os.lstat(os.path.join(repo_root, *entry.path.split("/")))
        except OSError:
            return False
        return (int(st.st_size) == entry.stat[STAT_SIZE]
                and int(st.st_mtime) == entry.stat[STAT_MTIME_SEC])
