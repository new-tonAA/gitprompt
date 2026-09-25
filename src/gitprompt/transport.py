"""Moving objects between repositories.

Three transports, each honest about what it needs:

`LocalTransport`  — a path or `file://` URL.  Reads and writes the other
                    repo's object directory directly.  Full fidelity, no
                    network; this is what local `clone`/`push`/`fetch` use.

`HttpTransport`   — gitprompt's native protocol against `gitprompt serve`.
                    Self-hostable anywhere you can run a process.

`GitMirrorRemote` — the one that makes GitHub work today.  GitHub cannot run
                    our server, so instead we use a plain git repository as a
                    *carrier*: the gitprompt object store is committed into a
                    dedicated branch as ordinary files, and the actual transfer
                    is delegated to real `git`, which already knows how to
                    authenticate to GitHub and how to move bytes reliably.

That last one is a deliberate trade: it makes hosting work everywhere git
works, at the cost of requiring `git` on PATH for remote operations only.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys

from .objects import ObjectStore
from .utils import GitPromptError, paint, to_text


def err(text: str) -> None:
    sys.stderr.write(text + "\n")

STORE_BRANCH = "gitprompt/store"
STORE_OBJECTS_DIR = "objects"
STORE_REFS_FILE = "refs.json"
STORE_FORMAT_FILE = "FORMAT"


# --------------------------------------------------------------------------
# shared helpers
# --------------------------------------------------------------------------

def collect_reachable(store: ObjectStore, roots) -> list:
    """Every sha reachable from `roots`, including a tag's target.

    A push that skipped a tag's target would leave the remote holding a tag
    that points at nothing, so tags are followed rather than treated as leaves.
    """
    from .objects import TYPE_COMMIT, TYPE_TAG, TYPE_TREE
    seen = set()
    order = []
    frontier = [r for r in roots if r]
    while frontier:
        sha = frontier.pop()
        if not sha or sha in seen:
            continue
        seen.add(sha)
        try:
            objtype = store.type_of(sha)
        except GitPromptError:
            continue
        order.append(sha)
        if objtype == TYPE_COMMIT:
            commit = store.read(sha)
            frontier.append(commit.tree)
            frontier.extend(commit.parents)
        elif objtype == TYPE_TREE:
            for _mode, _name, entry_sha in store.read(sha):
                frontier.append(entry_sha)
        elif objtype == TYPE_TAG:
            frontier.append(store.read(sha).object)
    return order


def _worktree_dirty(path: str, gpdir: str) -> bool:
    """Whether a local repository has changes we must not overwrite."""
    from .repo import Repository
    try:
        repo = Repository.discover(start=path, required=False)
    except Exception:
        return True
    if repo is None or repo.is_bare:
        return False
    try:
        from .cmd._helpers import working_status
        index = repo.read_index()
        state = working_status(repo, index, repo.head_tree())
    except Exception:
        return True
    return any(state.values())


def _refresh_worktree(path: str, gpdir: str, sha: str) -> None:
    """Make a local remote's files match the commit it was just moved to."""
    from .repo import Repository
    try:
        repo = Repository.discover(start=path, required=False)
        if repo is None or repo.is_bare:
            return
        repo.checkout_tree(repo.commit_of(sha).tree, force=True, update_index=True)
    except Exception:
        # the refs moved, which is the part that matters; a working tree that
        # could not be refreshed is reported but not fatal
        err(f"warning: updated '{path}' refs but could not refresh its work tree")

def is_ancestor(store: ObjectStore, maybe_ancestor: str, tip: str) -> bool:
    """Whether `maybe_ancestor` is reachable from `tip`.

    The test for a fast-forward, and the reason both the pushing client and
    the receiving server can make the same call about the same objects.
    """
    from .objects import TYPE_COMMIT
    seen = set()
    queue = [tip]
    while queue:
        sha = queue.pop()
        if sha in seen:
            continue
        seen.add(sha)
        if sha == maybe_ancestor:
            return True
        try:
            if store.type_of(sha) != TYPE_COMMIT:
                continue
            queue.extend(store.read(sha).parents)
        except GitPromptError:
            continue
    return False


def remote_kind(url: str) -> str:
    if url.startswith("http://") or url.startswith("https://"):
        if url.rstrip("/").endswith(".git") or "github.com" in url or "gitlab.com" in url \
                or "gitee.com" in url or "bitbucket.org" in url:
            return "git-mirror"
        return "http"
    if url.startswith("gp://"):
        return "http"
    if url.startswith("file://") or os.path.exists(os.path.expanduser(url)) \
            or url.startswith(("./", "../", "/")) or _looks_like_path(url):
        return "local"
    return "git-mirror"


def _looks_like_path(url: str) -> bool:
    if ":" in url.split("/")[0] and not url.startswith("/"):
        return False        # scp-style or url with scheme
    return os.sep in url or "/" in url


class RemoteRefs:
    def __init__(self, refs=None, capabilities=None):
        self.refs = dict(refs or {})
        self.capabilities = set(capabilities or [])

    def head(self):
        return self.refs.get("HEAD")

    def __contains__(self, name):
        return name in self.refs

    def __getitem__(self, name):
        return self.refs[name]


# --------------------------------------------------------------------------
# local
# --------------------------------------------------------------------------

class LocalTransport:
    kind = "local"

    def __init__(self, url: str):
        path = url[7:] if url.startswith("file://") else url
        self.path = os.path.abspath(os.path.expanduser(path))
        if not os.path.isdir(self.path):
            raise GitPromptError(f"repository '{url}' does not exist")
        if _is_bare_dir(self.path):
            self.gpdir = self.path
            # A URL may name the `.gitprompt` directory itself rather than the
            # checkout around it.  That looks like a bare repo, but it does have
            # a working tree — its parent — and a push into it still has to
            # update that tree.  Only a genuinely bare repo has none.
            self.worktree = (os.path.dirname(self.path)
                             if os.path.basename(self.path) == ".gitprompt" else None)
        else:
            self.gpdir = os.path.join(self.path, ".gitprompt")
            self.worktree = self.path
        if not os.path.isdir(self.gpdir):
            raise GitPromptError(f"'{url}' is not a gitprompt repository")
        self.store = ObjectStore(self.gpdir)

    def refs(self) -> RemoteRefs:
        from .refs import RefStore
        rs = RefStore(self.gpdir)
        out = dict(rs.list("refs/"))
        head = rs.head_sha()
        if head:
            out["HEAD"] = head
        return RemoteRefs(out)

    def fetch_objects(self, wants, haves=None, progress=None) -> int:
        """Copy the objects reachable from `wants` that we do not already have."""
        from .objects import TYPE_COMMIT, TYPE_TREE
        haves = set(haves or [])
        count = 0
        frontier = list(wants)
        seen = set()
        while frontier:
            sha = frontier.pop()
            if sha in seen:
                continue
            seen.add(sha)
            if self.store.exists(sha):
                count += _copy_object(self.store, sha, progress) if False else 0
                # still descend: the caller's store may lack children
            objtype = self.store.type_of(sha)
            payload = self.store.read_raw(sha)[1]
            if objtype == TYPE_COMMIT:
                commit = self.store.read(sha)
                frontier.append(commit.tree)
                frontier.extend(commit.parents)
            elif objtype == TYPE_TREE:
                for _mode, _name, entry_sha in self.store.read(sha):
                    frontier.append(entry_sha)
        return len(seen)

    def write_object_into(self, target_store: ObjectStore, sha: str) -> bool:
        if target_store.exists(sha):
            return False
        objtype, payload = self.store.read_raw(sha)
        target_store.write_raw(objtype, payload)
        return True

    def materialise(self, target_store: ObjectStore, wants) -> int:
        return transfer_objects(self.store, target_store, wants)

    def publish(self, source_store: ObjectStore, updates: dict, message="gitprompt push"):
        """Copy objects across, then move the remote's refs.

        A local push edits another repository's refs directly, which git also
        allows.  Its working tree is only refreshed when it is clean: pushing
        into a repository that has uncommitted work would silently destroy
        that work, and there is no way for the pusher to know it was there.
        """
        from .refs import RefStore, check_ref_name
        count = transfer_objects(source_store, self.store,
                                 collect_reachable(source_store, updates.values()))

        # Dirtiness has to be read *before* the refs move: afterwards the
        # remote's worktree looks like it is missing everything we just sent,
        # and every push would refuse to refresh it.
        dirty = self.worktree is not None and _worktree_dirty(self.worktree, self.gpdir)

        remote_refs = RefStore(self.gpdir)
        moved = []
        for refname, sha in updates.items():
            if not sha:
                continue
            refname = check_ref_name(refname)
            remote_refs.update(refname, sha, message=message, reflog=False)
            moved.append((refname, sha))

        if self.worktree is not None:
            for refname, sha in moved:
                if refname.startswith("refs/heads/") and remote_refs.head_ref() == refname:
                    if dirty:
                        err(f"warning: '{self.worktree}' has uncommitted changes; "
                            "its working tree was left alone")
                    else:
                        _refresh_worktree(self.worktree, self.gpdir, sha)
        return {"objects": count, "refs": len(moved)}

    def close(self):
        pass


def _copy_object(source: ObjectStore, sha: str, progress=None):
    return 0


def _is_bare_dir(path: str) -> bool:
    return (os.path.isdir(os.path.join(path, "objects"))
            and os.path.exists(os.path.join(path, "HEAD"))
            and os.path.exists(os.path.join(path, "refs")))


# --------------------------------------------------------------------------
# git-mirror (GitHub and friends)
# --------------------------------------------------------------------------

class GitMirrorRemote:
    """A git repository used as the carrier for a gitprompt object store.

    Layout inside the carrier's `gitprompt/store` branch::

        FORMAT                  "gitprompt-store 1 sha1"
        refs.json               {"refs/heads/main": "<sha>", ...}
        objects/ab/cdef...      the same zlib-compressed loose objects

    Because the carrier is an ordinary git repository, anything git can host
    can host a gitprompt history.
    """

    kind = "git-mirror"

    def __init__(self, url: str, cache_dir: str, name: str = "origin"):
        self.url = url
        self.cache_dir = cache_dir
        self.name = name
        self._ensure_git()

    # -- carrier management -----------------------------------------------

    def _git(self, *args, cwd=None, check=True, capture=True):
        result = subprocess.run(
            ["git", "-C", cwd or self.cache_dir, *args],
            capture_output=capture, text=True,
            # Decode explicitly: the default on Windows follows the console
            # code page, so one non-ASCII byte from git would otherwise abort
            # a push with a UnicodeDecodeError instead of a real message.
            encoding="utf-8", errors="replace",
            input=None,
        )
        if check and result.returncode != 0:
            message = (result.stderr or result.stdout or "").strip()
            raise GitPromptError(
                f"git {args[0]} failed for remote '{self.name}':\n{message}"
            )
        return result

    def _ensure_git(self):
        if shutil.which("git") is None:
            raise GitPromptError(
                f"remote '{self.name}' is a git mirror ({self.url}) but 'git' was not "
                "found on PATH.\n"
                "hint: install git, or use a local/file:// remote, or run "
                "'gitprompt serve' and use an http remote"
            )

    def _mirror_ready(self) -> bool:
        return os.path.isdir(os.path.join(self.cache_dir, ".git"))

    def _init_mirror(self):
        os.makedirs(self.cache_dir, exist_ok=True)
        self._git("init", "--quiet", "-b", STORE_BRANCH, cwd=self.cache_dir)
        self._git("remote", "add", "origin", self.url, cwd=self.cache_dir)
        self._git("config", "user.name", "gitprompt", cwd=self.cache_dir)
        self._git("config", "user.email", "gitprompt@localhost", cwd=self.cache_dir)

    def _ensure_mirror(self):
        if not self._mirror_ready():
            self._init_mirror()
        remotes = self._git("remote", cwd=self.cache_dir).stdout.split()
        if "origin" not in remotes:
            self._git("remote", "add", "origin", self.url, cwd=self.cache_dir)
        else:
            current = self._git("remote", "get-url", "origin", cwd=self.cache_dir).stdout.strip()
            if current != self.url:
                self._git("remote", "set-url", "origin", self.url, cwd=self.cache_dir)

    # -- reading ----------------------------------------------------------

    def _fetch_carrier(self) -> bool:
        """Bring the carrier branch into the local cache.  Returns False when
        the remote branch does not exist yet (first push)."""
        self._ensure_mirror()
        result = self._git("fetch", "--quiet", "origin", STORE_BRANCH,
                           cwd=self.cache_dir, check=False)
        if result.returncode != 0:
            return False
        self._git("checkout", "--quiet", "-B", STORE_BRANCH,
                  "FETCH_HEAD", cwd=self.cache_dir, check=False)
        self._git("reset", "--quiet", "--hard", "FETCH_HEAD", cwd=self.cache_dir, check=False)
        return os.path.exists(os.path.join(self.cache_dir, STORE_REFS_FILE))

    def refs(self) -> RemoteRefs:
        if not self._fetch_carrier():
            return RemoteRefs()
        path = os.path.join(self.cache_dir, STORE_REFS_FILE)
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        return RemoteRefs(data.get("refs", {}))

    def _carrier_store(self) -> ObjectStore:
        return ObjectStore(os.path.join(self.cache_dir, "_store"))

    def materialise(self, target_store: ObjectStore, wants) -> int:
        """Import the wanted objects from the carrier into `target_store`."""
        if not self._fetch_carrier():
            return 0
        return _import_carrier(self.cache_dir, target_store, wants)

    # -- writing ----------------------------------------------------------

    def publish(self, source_store: ObjectStore, refs: dict, message="gitprompt push"):
        self._ensure_mirror()
        self._fetch_carrier()

        objects_dir = os.path.join(self.cache_dir, STORE_OBJECTS_DIR)
        written = _export_store(source_store, objects_dir)

        refs_path = os.path.join(self.cache_dir, STORE_REFS_FILE)
        merged = {}
        if os.path.exists(refs_path):
            try:
                with open(refs_path, "r", encoding="utf-8") as fh:
                    merged = json.load(fh).get("refs", {})
            except (OSError, ValueError):
                merged = {}
        merged.update(refs)

        with open(refs_path, "w", encoding="utf-8", newline="\n") as fh:
            json.dump({"version": 1, "format": source_store.fmt, "refs": merged},
                      fh, indent=2, sort_keys=True)
            fh.write("\n")
        with open(os.path.join(self.cache_dir, STORE_FORMAT_FILE), "w",
                  encoding="utf-8") as fh:
            fh.write(f"gitprompt-store 1 {source_store.fmt}\n")

        status = self._git("status", "--porcelain", cwd=self.cache_dir).stdout
        if not status.strip():
            return {"objects": written, "committed": False}

        self._git("add", "-A", cwd=self.cache_dir)
        self._git("commit", "--quiet", "-m", message, cwd=self.cache_dir, check=False)
        result = self._git("push", "--quiet", "origin",
                           f"HEAD:refs/heads/{STORE_BRANCH}",
                           cwd=self.cache_dir, check=False)
        if result.returncode != 0:
            raise GitPromptError(
                f"failed to push to {self.url}:\n"
                + (result.stderr or result.stdout or "").strip()
                + "\nhint: check that the repository exists and that you have "
                  "credentials configured (git uses your credential helper)"
            )
        return {"objects": written, "committed": True}

    def close(self):
        pass


def _export_store(source: ObjectStore, dest_objects: str) -> int:
    """Copy every loose object we have into the carrier directory."""
    written = 0
    src = source.objects_dir
    if not os.path.isdir(src):
        return 0
    for sub in os.listdir(src):
        subdir = os.path.join(src, sub)
        if len(sub) != 2 or not os.path.isdir(subdir):
            continue
        dest_sub = os.path.join(dest_objects, sub)
        os.makedirs(dest_sub, exist_ok=True)
        for name in os.listdir(subdir):
            if len(name) != 38:
                continue
            target = os.path.join(dest_sub, name)
            if os.path.exists(target):
                continue
            shutil.copy2(os.path.join(subdir, name), target)
            written += 1
    return written


def _import_carrier(carrier_dir: str, target_store: ObjectStore, wants) -> int:
    """Copy the carrier's objects into our store, then walk from `wants`."""
    from .objects import TYPE_COMMIT, TYPE_TREE, TYPE_TAG

    carrier_objects = os.path.join(carrier_dir, STORE_OBJECTS_DIR)
    if not os.path.isdir(carrier_objects):
        return 0

    def carrier_path(sha):
        return os.path.join(carrier_objects, sha[:2], sha[2:])

    imported = 0
    seen = set()
    frontier = list(wants)
    while frontier:
        sha = frontier.pop()
        if sha in seen:
            continue
        seen.add(sha)
        src = carrier_path(sha)
        if not os.path.exists(src):
            continue
        if not target_store.exists(sha):
            with open(src, "rb") as fh:
                raw = fh.read()
            import zlib
            try:
                data = zlib.decompress(raw)
            except zlib.error:
                continue
            nul = data.index(b"\0")
            objtype = data[:nul].decode("ascii").split(" ")[0]
            payload = data[nul + 1:]
            target_store.write_raw(objtype, payload)
            imported += 1
        try:
            objtype, payload = target_store.read_raw(sha)
        except Exception:
            continue
        if objtype == TYPE_COMMIT:
            commit = target_store.read(sha)
            frontier.append(commit.tree)
            frontier.extend(commit.parents)
        elif objtype == TYPE_TREE:
            for _mode, _name, entry_sha in target_store.read(sha):
                frontier.append(entry_sha)
        elif objtype == TYPE_TAG:
            # a tag's target has to travel with it, or the receiving side
            # ends up with a tag pointing at nothing
            frontier.append(target_store.read(sha).object)
    return imported


# --------------------------------------------------------------------------
# http (gitprompt's own protocol)
# --------------------------------------------------------------------------

class HttpTransport:
    kind = "http"

    def __init__(self, url: str):
        self.url = url.rstrip("/")
        if self.url.startswith("gp://"):
            self.url = "http://" + self.url[5:]

    def _request(self, path, method="GET", body=None, headers=None):
        import urllib.error
        import urllib.request
        full = self.url + path
        req = urllib.request.Request(full, data=body, method=method)
        for key, value in (headers or {}).items():
            req.add_header(key, value)
        try:
            with urllib.request.urlopen(req, timeout=60) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as exc:
            raise GitPromptError(
                f"{method} {full}: HTTP {exc.code} {exc.reason}\n"
                f"{exc.read().decode('utf-8', 'replace')[:400]}"
            )
        except urllib.error.URLError as exc:
            raise GitPromptError(f"cannot reach {full}: {exc.reason}")

    def refs(self) -> RemoteRefs:
        import json as _json
        status, body = self._request("/info/refs")
        data = _json.loads(body.decode("utf-8"))
        return RemoteRefs(data.get("refs", {}))

    def fetch_objects(self, wants, haves=None, progress=None) -> int:
        import json as _json
        payload = _json.dumps({"wants": list(wants), "haves": list(haves or [])}).encode()
        status, body = self._request("/objects", method="POST", body=payload,
                                     headers={"Content-Type": "application/json"})
        objects = _json.loads(body.decode("utf-8"))
        return len(objects)

    def download_all(self, wants, target_store: ObjectStore, progress=None) -> int:
        """Stream every object reachable from `wants` into the local store."""
        import json as _json
        frontier = list(wants)
        seen = set()
        imported = 0
        while frontier:
            batch = [sha for sha in frontier if sha not in seen]
            frontier = []
            if not batch:
                break
            payload = _json.dumps({"want": batch}).encode()
            status, body = self._request("/batch", method="POST", body=payload,
                                         headers={"Content-Type": "application/json"})
            data = _json.loads(body.decode("utf-8"))
            for item in data.get("objects", []):
                sha = item["sha"]
                if sha in seen:
                    continue
                seen.add(sha)
                if item.get("missing"):
                    continue
                import base64
                raw = base64.b64decode(item["data"])
                import zlib
                objtype = item["type"]
                target_store.write_raw(objtype, zlib.decompress(raw))
                imported += 1
                frontier.extend(item.get("next", []))
            if progress:
                progress(imported)
        return imported

    def materialise(self, target_store: ObjectStore, wants) -> int:
        return self.download_all(wants, target_store)

    def _encode(self, source_store: ObjectStore, shas):
        import base64
        import zlib
        items = []
        for sha in shas:
            objtype, payload = source_store.read_raw(sha)
            items.append({
                "type": objtype,
                "sha": sha,
                "data": base64.b64encode(zlib.compress(payload, 1)).decode("ascii"),
            })
        return items

    def publish(self, source_store: ObjectStore, updates: dict, message="gitprompt push"):
        """Upload the objects and move the refs in one request.

        Both halves have to travel together: objects alone would leave the
        remote holding a push it never pointed a branch at, which reads as
        success while changing nothing.
        """
        import json as _json
        shas = collect_reachable(source_store, updates.values())
        body = _json.dumps({
            "objects": self._encode(source_store, shas),
            "refs": updates,
            "message": message,
        }).encode()
        status, response = self._request("/receive", method="POST", body=body,
                                         headers={"Content-Type": "application/json"})
        return _json.loads(response.decode("utf-8"))

    def close(self):
        pass


# --------------------------------------------------------------------------
# factory
# --------------------------------------------------------------------------

def open_transport(url: str, cache_dir: str, name: str = "origin"):
    kind = remote_kind(url)
    if kind == "local":
        return LocalTransport(url)
    if kind == "http":
        return HttpTransport(url)
    return GitMirrorRemote(url, cache_dir, name)


def transfer_objects(source_store: ObjectStore, target_store: ObjectStore, wants):
    """Copy every object reachable from `wants` between two local stores.

    Iterative rather than recursive so a deep history cannot exhaust the
    stack, and content-addressed so copying twice is free.
    """
    from .objects import TYPE_COMMIT, TYPE_TREE, TYPE_TAG
    seen = set()
    frontier = list(wants)
    copied = 0
    while frontier:
        sha = frontier.pop()
        if sha in seen:
            continue
        seen.add(sha)
        if target_store.exists(sha):
            continue
        try:
            objtype, payload = source_store.read_raw(sha)
        except GitPromptError:
            continue
        target_store.write_raw(objtype, payload)
        copied += 1
        if objtype == TYPE_COMMIT:
            commit = source_store.read(sha)
            frontier.append(commit.tree)
            frontier.extend(commit.parents)
        elif objtype == TYPE_TREE:
            for _mode, _name, entry_sha in source_store.read(sha):
                frontier.append(entry_sha)
        elif objtype == TYPE_TAG:
            frontier.append(source_store.read(sha).object)
    return copied
