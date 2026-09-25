"""`gitprompt serve` — a small HTTP server speaking gitprompt's native protocol.

Endpoints::

    GET  /info/refs    -> {"refs": {...}, "format": "sha1"}
    POST /batch        -> {"want": [sha...]}   =>  {"objects": [{sha,type,data,next}]}
    POST /objects      -> {"wants":[...], "haves":[...]}  =>  {"objects": {...}}
    POST /receive      -> {"objects":[…], "refs":{…}}     =>  {"stored": n, "refs": m}
    GET  /health       -> {"ok": true}

Objects are sent zlib-compressed and base64-encoded, which keeps the payload
opaque to any proxy in between.  The server is read-only unless started
without `--read-only`, and it never executes anything from a request: a push
arriving over HTTP is untrusted input, so every object is re-hashed before it
is written and anything that does not hash to its claimed name is rejected.

A push carries its ref updates alongside its objects so the branch actually
moves; the server re-checks the fast-forward guard itself rather than trusting
the pusher to have run it.
"""

from __future__ import annotations

import base64
import json
import os
import sys
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .objects import TYPE_COMMIT, TYPE_TREE, hash_object
from .refs import RefStore
from .objects import ObjectStore
from .utils import GitPromptError, paint


class Handler(BaseHTTPRequestHandler):
    server_version = "gitprompt/0.1"
    protocol_version = "HTTP/1.1"

    # -- plumbing ----------------------------------------------------------

    @property
    def gpdir(self) -> str:
        return self.server.gpdir

    def _send(self, status: int, payload, content_type="application/json"):
        if isinstance(payload, (dict, list)):
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        elif isinstance(payload, str):
            body = payload.encode("utf-8")
        else:
            body = payload
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self):
        length = int(self.headers.get("Content-Length") or 0)
        if not length:
            return {}
        raw = self.rfile.read(length)
        try:
            return json.loads(raw.decode("utf-8"))
        except ValueError:
            raise GitPromptError("malformed JSON in request body")

    def log_message(self, fmt, *args):
        if self.server.verbose:
            sys.stderr.write(paint(f"[serve] {self.address_string()} {fmt % args}\n", "dim"))

    # -- routes ------------------------------------------------------------

    def do_GET(self):
        path = self.path.split("?")[0].rstrip("/") or "/"
        try:
            if path in ("/", "/health"):
                return self._send(200, {"ok": True, "service": "gitprompt",
                                        "version": "0.1",
                                        "writable": not self.server.read_only})
            if path == "/info/refs":
                return self._info_refs()
            if path.startswith("/objects/"):
                return self._get_object(path)
            return self._send(404, {"error": f"no such endpoint: {path}"})
        except GitPromptError as exc:
            return self._send(400, {"error": str(exc)})

    def do_POST(self):
        path = self.path.split("?")[0].rstrip("/") or "/"
        try:
            if path == "/batch":
                return self._batch()
            if path == "/objects":
                return self._wants()
            if path == "/receive":
                return self._receive()
            return self._send(404, {"error": f"no such endpoint: {path}"})
        except GitPromptError as exc:
            return self._send(400, {"error": str(exc)})

    # -- handlers ----------------------------------------------------------

    def _info_refs(self):
        refs = RefStore(self.gpdir)
        store = ObjectStore(self.gpdir)
        out = dict(refs.list("refs/"))
        head = refs.head_sha()
        if head:
            out["HEAD"] = head
        return self._send(200, {"refs": out, "format": store.fmt,
                                "service": "gitprompt-upload-pack"})

    def _get_object(self, path):
        sha = path[len("/objects/"):]
        store = ObjectStore(self.gpdir)
        if not store.exists(sha):
            return self._send(404, {"error": f"no such object: {sha}"})
        objtype, payload = store.read_raw(sha)
        return self._send(200, {
            "sha": sha, "type": objtype,
            "data": base64.b64encode(zlib.compress(payload, 1)).decode("ascii"),
        })

    def _batch(self):
        """Fetch a set of objects plus the names of their children.

        Sending the child names rather than the children themselves lets the
        client walk a commit graph in as many round trips as the graph is
        deep, instead of asking for objects it already has.
        """
        request = self._read_json()
        wants = request.get("want") or []
        if not isinstance(wants, list):
            raise GitPromptError("'want' must be a list")
        store = ObjectStore(self.gpdir)
        items = []
        for sha in wants[:5000]:
            if not store.exists(sha):
                items.append({"sha": sha, "missing": True})
                continue
            objtype, payload = store.read_raw(sha)
            children = []
            try:
                if objtype == TYPE_COMMIT:
                    commit = store.read(sha)
                    children = [commit.tree] + list(commit.parents)
                elif objtype == TYPE_TREE:
                    children = [entry_sha for _m, _n, entry_sha in store.read(sha)]
            except GitPromptError:
                children = []
            items.append({
                "sha": sha, "type": objtype,
                "data": base64.b64encode(zlib.compress(payload, 1)).decode("ascii"),
                "next": children,
            })
        return self._send(200, {"objects": items})

    def _wants(self):
        request = self._read_json()
        store = ObjectStore(self.gpdir)
        haves = set(request.get("haves") or [])
        result = {}
        for sha in (request.get("wants") or []):
            if sha in haves or not store.exists(sha):
                continue
            objtype, payload = store.read_raw(sha)
            result[sha] = {"type": objtype,
                           "data": base64.b64encode(zlib.compress(payload, 1)).decode("ascii")}
        return self._send(200, {"objects": result})

    def _receive(self):
        if self.server.read_only:
            return self._send(403, {"error": "this server is read-only"})
        request = self._read_json()
        items = request.get("objects") or []
        store = ObjectStore(self.gpdir)
        stored, rejected = 0, []
        for item in items:
            try:
                objtype = item["type"]
                raw = zlib.decompress(base64.b64decode(item["data"]))
            except (KeyError, ValueError, zlib.error) as exc:
                rejected.append({"reason": f"undecodable: {exc}"})
                continue
            claimed = item.get("sha")
            actual = hash_object(objtype, raw, store.fmt)
            if claimed and claimed != actual:
                rejected.append({"sha": claimed,
                                 "reason": f"content hashes to {actual}, not {claimed}"})
                continue
            store.write_raw(objtype, raw)
            stored += 1

        moved, refused = self._apply_refs(store, request.get("refs") or {},
                                          request.get("message"))
        return self._send(200, {"stored": stored, "refs": len(moved),
                                "moved": moved, "refused": refused,
                                "rejected": rejected})

    def _apply_refs(self, store, refs, message=None):
        """Move the refs a push asked for, refusing anything unsafe.

        The pusher runs the same check before sending; repeating it here means
        a client that lies about its history — or one that is not gitprompt at
        all — cannot rewind a branch just by claiming to.
        """
        from .refs import check_ref_name
        from .transport import _refresh_worktree, _worktree_dirty, is_ancestor

        ref_store = RefStore(self.gpdir)
        # Read this before any ref moves: afterwards the checkout looks like it
        # is missing everything the push just sent, and every push would decide
        # the working tree was too dirty to touch.
        worktree = (os.path.dirname(self.gpdir)
                    if os.path.basename(self.gpdir) == ".gitprompt" else None)
        dirty = bool(worktree) and _worktree_dirty(worktree, self.gpdir)

        moved, refused = [], []
        for refname, sha in refs.items():
            if not sha:
                continue
            try:
                refname = check_ref_name(refname)
            except GitPromptError as exc:
                refused.append({"ref": refname, "reason": str(exc)})
                continue
            if not store.exists(sha):
                refused.append({"ref": refname, "reason": "missing object"})
                continue
            current = ref_store.read(refname)
            if current and current != sha and not is_ancestor(store, current, sha):
                refused.append({"ref": refname, "reason": "non-fast-forward"})
                continue
            ref_store.update(refname, sha, message=message or "push", reflog=False)
            moved.append(refname)

        head_ref = ref_store.head_ref()
        if worktree and head_ref in moved:
            if dirty:
                print(f"note: {worktree} has uncommitted changes; "
                      "its working tree was left alone", file=sys.stderr)
            else:
                _refresh_worktree(worktree, self.gpdir, ref_store.read(head_ref))
        return moved, refused

    # -- misc --------------------------------------------------------------

    def do_HEAD(self):
        self.send_response(200)
        self.end_headers()


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, gpdir, read_only=False, verbose=True):
        super().__init__(addr, Handler)
        self.gpdir = gpdir
        self.read_only = read_only
        self.verbose = verbose


def serve(path: str, host="127.0.0.1", port=8765, read_only=False) -> int:
    gpdir = path
    if not os.path.isdir(os.path.join(gpdir, "objects")):
        candidate = os.path.join(path, ".gitprompt")
        if os.path.isdir(candidate):
            gpdir = candidate
        else:
            raise GitPromptError(
                f"'{path}' is not a gitprompt repository to serve\n"
                "hint: run from inside a repository, or pass --path <repo>"
            )

    try:
        server = Server((host, port), gpdir, read_only=read_only)
    except OSError as exc:
        raise GitPromptError(f"cannot bind {host}:{port}: {exc}")

    refs = RefStore(gpdir)
    branches = refs.branches()
    sys.stderr.write(paint(f"Serving {gpdir}\n", "bold"))
    sys.stderr.write(f"  http://{host}:{port}/\n")
    sys.stderr.write(f"  {len(branches)} branch(es): "
                     f"{', '.join(sorted(branches)) or '(none)'}\n")
    sys.stderr.write(f"  mode: {'read-only' if read_only else 'read-write'}\n")
    sys.stderr.write(paint("  clone with: gitprompt clone "
                           f"gp://{host}:{port}/ <dir>\n", "dim"))
    sys.stderr.write(paint("  ctrl-c to stop\n", "dim"))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        sys.stderr.write("\nstopped\n")
    finally:
        server.server_close()
    return 0
