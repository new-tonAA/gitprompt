"""Remote commands: remote, push, fetch, pull, serve."""

from __future__ import annotations

import os
import sys

from ..argparser import Args, Opt
from ..objects import TYPE_COMMIT, TYPE_TREE
from ..transport import is_ancestor, open_transport, remote_kind, transfer_objects
from ..utils import GitPromptError, paint
from ._helpers import err, out


# --------------------------------------------------------------------------

def cmd_remote(repo, argv):
    args = Args.parse([Opt("-v", "--verbose"), Opt("-d", "--delete"),
                       Opt("--set-url", takes_value=True), Opt("--show-url"),
                       Opt("--prune")], argv)

    if not argv:
        return _list_remotes(repo, verbose=False)

    sub = args.positional[0] if args.positional else None

    if sub in ("add", "set-url", "remove", "rm", "show", "get-url", "rename"):
        name = args.arg(1)
        if not name:
            raise GitPromptError(f"remote {sub}: a remote name is required")
        if sub == "add":
            url = args.arg(2)
            if not url:
                raise GitPromptError("remote add: a URL is required")
            if repo.config.get(f"remote.{name}.url"):
                raise GitPromptError(f"remote '{name}' already exists")
            repo.config.set(f"remote.{name}.url", url)
            repo.config.set(f"remote.{name}.fetch", f"+refs/heads/*:refs/remotes/{name}/*")
            repo.reload_config()
            return 0
        if sub in ("remove", "rm"):
            removed = repo.config.unset_section(f"remote.{name}")
            for refname in list(repo.refs.list(f"refs/remotes/{name}/")):
                repo.refs.delete(refname, reflog=False)
            if not removed:
                raise GitPromptError(f"no such remote: '{name}'")
            repo.reload_config()
            return 0
        if sub in ("show-url", "get-url"):
            url = repo.config.get(f"remote.{name}.url")
            if not url:
                raise GitPromptError(f"no such remote: '{name}'")
            out(url)
            return 0
        if sub == "set-url":
            url = args.arg(2)
            if not url:
                raise GitPromptError("remote set-url: a URL is required")
            repo.config.set(f"remote.{name}.url", url)
            repo.reload_config()
            return 0
        if sub == "rename":
            new = args.arg(2)
            if not new:
                raise GitPromptError("remote rename: a new name is required")
            url = repo.config.get(f"remote.{name}.url")
            if not url:
                raise GitPromptError(f"no such remote: '{name}'")
            fetch = repo.config.get(f"remote.{name}.fetch")
            repo.config.unset_section(f"remote.{name}")
            repo.config.set(f"remote.{new}.url", url)
            if fetch:
                repo.config.set(f"remote.{new}.fetch", fetch.replace(f"{name}/", f"{new}/"))
            repo.reload_config()
            return 0
        if sub == "show":
            url = repo.config.get(f"remote.{name}.url")
            if not url:
                raise GitPromptError(f"no such remote: '{name}'")
            out(f"* remote {name}")
            out(f"  Fetch URL: {url}")
            out(f"  Push  URL: {url}")
            return 0

    if sub in ("prune",):
        return 0

    if args.flag("show_url"):
        name = args.arg(0)
        if not name:
            raise GitPromptError("remote --show-url: a remote name is required")
        out(repo.config.get(f"remote.{name}.url") or "")
        return 0

    return _list_remotes(repo, verbose=args.flag("v", "verbose"))


def _list_remotes(repo, verbose=False) -> int:
    names = sorted({k.split(".")[1] for k, _v in repo.config.items("remote.")
                    if k.startswith("remote.") and k.count(".") >= 2})
    for name in names:
        url = repo.config.get(f"remote.{name}.url") or ""
        if verbose:
            kind = remote_kind(url) if url else "?"
            out(f"{name}\t{url} (fetch)")
            out(f"{name}\t{url} (push)   [{kind}]")
        else:
            out(name)
    return 0


# --------------------------------------------------------------------------

def _get_transport(repo, name=None):
    name = name or "origin"
    url = repo.config.get(f"remote.{name}.url")
    if not url:
        raise GitPromptError(
            f"no such remote: '{name}'\n"
            f"hint: gitprompt remote add {name} <url>"
        )
    cache = os.path.join(repo.gpdir, "remotes", name, "mirror")
    return name, url, open_transport(url, cache, name)


def cmd_fetch(repo, argv):
    args = Args.parse([
        Opt("--all"), Opt("-q", "--quiet"), Opt("--prune"),
        Opt("--tags"), Opt("-f", "--force"),
    ], argv)
    names = []
    if args.flag("all"):
        names = sorted({k.split(".")[1] for k, _v in repo.config.items("remote.")
                        if k.startswith("remote.") and k.count(".") >= 2})
        if not names:
            raise GitPromptError("no remotes configured")
    elif args.positional:
        names = [args.positional[0]]
    else:
        names = ["origin"]

    total = 0
    for name in names:
        name, url, transport = _get_transport(repo, name)
        try:
            remote_refs = transport.refs().refs
            if not remote_refs:
                if not args.flag("q"):
                    err(f"warning: remote '{name}' has no refs (is it an empty gitprompt repo?)")
                continue
            wants = list(remote_refs.values())
            copied = _transfer_in(repo, transport, wants)
            total += copied
            _store_remote_refs(repo, name, remote_refs, args.flag("force"))
            if not args.flag("q"):
                branches = sum(1 for k in remote_refs if k.startswith("refs/heads/"))
                err(f"From {url}\n  fetched {copied} object(s), "
                    f"{len(remote_refs)} ref(s) ({branches} branch(es))")
        finally:
            transport.close()
    return 0


def _transfer_in(repo, transport, wants) -> int:
    """Pull objects into our store, whichever transport produced them."""
    if hasattr(transport, "materialise"):
        return transport.materialise(repo.store, wants)
    if hasattr(transport, "download_all"):
        return transport.download_all(wants, repo.store)
    return transfer_objects(transport.store, repo.store, wants)


def _store_remote_refs(repo, name, remote_refs, force=False):
    for refname, sha in remote_refs.items():
        if refname == "HEAD":
            continue
        if refname.startswith("refs/heads/"):
            local = f"refs/remotes/{name}/{refname[len('refs/heads/'):]}"
        elif refname.startswith("refs/tags/") or refname.startswith("refs/sessions/"):
            local = refname
        else:
            continue
        repo.refs.update(local, sha, message=f"fetch {name}", reflog=False)
    head = remote_refs.get("HEAD")
    if head:
        repo.refs.update(f"refs/remotes/{name}/HEAD", head, reflog=False)


# --------------------------------------------------------------------------

def cmd_push(repo, argv):
    args = Args.parse([
        Opt("-u", "--set-upstream"), Opt("-f", "--force"),
        Opt("--all"), Opt("--tags"), Opt("--dry-run"),
        Opt("-q", "--quiet"), Opt("--delete"),
    ], argv)

    name = None
    refspecs = list(args.positional)
    if refspecs and not _looks_like_refspec(refspecs[0]):
        name = refspecs.pop(0)
    all_remotes = {k.split(".")[1] for k, _v in repo.config.items("remote.")
                   if k.startswith("remote.") and k.count(".") >= 2}

    if name is None:
        current = repo.refs.current_branch()
        name = repo.config.get(f"branch.{current}.remote") if current else None
        name = name or ("origin" if "origin" in all_remotes else
                        (sorted(all_remotes)[0] if len(all_remotes) == 1 else None))
    if not name:
        raise GitPromptError(
            "no remote configured to push to\n"
            "hint: gitprompt remote add origin <url>"
        )

    name, url, transport = _get_transport(repo, name)
    refs = _resolve_push_refspecs(repo, refspecs, args)
    if not refs:
        raise GitPromptError("nothing to push (no refs selected)")

    if args.flag("dry_run"):
        for local, remote in refs.items():
            err(f"  would push {local} -> {remote}")
        return 0

    updates = _resolve_updates(repo, refs)
    if not updates:
        raise GitPromptError("nothing to push (no refs resolved to a commit)")

    try:
        _check_fast_forward(repo, transport, updates, args.flag("force"))
        result = transport.publish(
            repo.store, updates,
            message=f"gitprompt push from {repo.root or repo.gpdir}")
        refused = result.get("refused") or []
        if refused:
            # the objects are on the remote but the branch did not move;
            # saying "done" here would be a lie the user acts on
            detail = "; ".join(f"{r['ref']}: {r['reason']}" for r in refused)
            raise GitPromptError(f"the remote refused the update — {detail}")
        if not args.flag("q"):
            uploaded = result.get("objects", result.get("stored", 0))
            err(f"To {url}")
            for local, remote in refs.items():
                sha = updates.get(remote, "")
                err(f"   {local} -> {remote}  ({sha[:7]})")
            err(f"  {uploaded} new object(s) uploaded")
        _update_push_tracking(repo, name, refs, args.flag("set_upstream"))
    finally:
        transport.close()
    return 0


def _resolve_updates(repo, refs) -> dict:
    """`{remote ref: sha}` — what the remote should be left pointing at.

    Transports all want the destination name paired with the sha, whereas
    refspecs are local→remote name pairs; converting once here keeps the
    transports from each having to know how to read our ref store.
    """
    updates = {}
    for local, remote in refs.items():
        if local == "HEAD":
            sha = repo.refs.head_sha()
        else:
            sha = repo.refs.read(local)
        if sha:
            updates[remote] = sha
    return updates


def _looks_like_refspec(value: str) -> bool:
    return (":" in value or value.startswith("refs/") or value == "HEAD"
            or value.startswith("refs") or "/" in value)


def _resolve_push_refspecs(repo, refspecs, args) -> dict:
    """local ref → remote ref.  Defaults to the current branch, as git does."""
    mapping = {}
    if args.flag("all"):
        for name, sha in repo.refs.branches().items():
            mapping[f"refs/heads/{name}"] = f"refs/heads/{name}"
    if args.flag("tags"):
        for name in repo.refs.tags():
            mapping[f"refs/tags/{name}"] = f"refs/tags/{name}"

    if not refspecs and not mapping:
        current = repo.refs.current_branch()
        if not current:
            raise GitPromptError(
                "you are not currently on a branch\n"
                "hint: push an explicit ref, e.g. 'gitprompt push origin HEAD:refs/heads/main'"
            )
        mapping[f"refs/heads/{current}"] = f"refs/heads/{current}"
    else:
        for spec in refspecs:
            if spec.startswith("+"):
                spec = spec[1:]
            if spec.startswith("^"):
                continue
            if ":" in spec:
                local, _, remote = spec.partition(":")
            else:
                local = remote = spec
            if local == "HEAD":
                local = repo.refs.head_ref() or "HEAD"
            elif not local.startswith("refs/"):
                candidate = f"refs/heads/{local}"
                local = candidate if repo.refs.read(candidate) else local
            if not remote.startswith("refs/"):
                remote = f"refs/heads/{remote}" if remote else local
            mapping[local] = remote

    # resolve anything symbolic now, so a failure happens before any upload
    resolved = {}
    for local, remote in mapping.items():
        sha = repo.refs.read(local)
        if sha is None and local == "HEAD":
            sha = repo.refs.head_sha()
        if sha is None:
            raise GitPromptError(f"src refspec '{local}' does not match any ref")
        resolved[local] = remote
    return resolved


def _update_push_tracking(repo, name, refs, set_upstream):
    for local, remote in refs.items():
        sha = repo.refs.read(local)
        if sha:
            repo.refs.update(f"refs/remotes/{name}/{remote[len('refs/heads/'):]}", sha,
                             message=f"push to {name}", reflog=False)
        if set_upstream and local.startswith("refs/heads/"):
            branch = local[len("refs/heads/"):]
            repo.config.set(f"branch.{branch}.remote", name)
            repo.config.set(f"branch.{branch}.merge", remote)
    repo.reload_config()


def _check_fast_forward(repo, transport, updates, force=False):
    """Refuse a push that would discard commits the remote already has.

    A non-fast-forward push silently deletes work for whoever else is using
    that branch, so it is refused unless --force says otherwise — the same
    protection git gives, for the same reason.
    """
    if force:
        return
    try:
        remote_refs = transport.refs().refs
    except GitPromptError:
        return
    for refname, sha in updates.items():
        old = remote_refs.get(refname)
        if not old or old == sha:
            continue
        if not _is_ancestor(repo, old, sha):
            raise GitPromptError(
                f"non-fast-forward: {refname} would lose commits already on the remote\n"
                f"hint: merge or rebase first, or push with --force if you mean it"
            )


def _is_ancestor(repo, maybe_ancestor: str, tip: str) -> bool:
    """Whether `maybe_ancestor` is reachable from `tip`.

    The same walk the server runs when it receives a push, so client and
    server cannot disagree about whether an update is a fast-forward.
    """
    return is_ancestor(repo.store, maybe_ancestor, tip)


# --------------------------------------------------------------------------

def cmd_pull(repo, argv):
    args = Args.parse([
        Opt("-q", "--quiet"), Opt("--no-commit"), Opt("--rebase"),
        Opt("--ff-only"), Opt("-f", "--force"),
    ], argv)
    name = args.arg(0) or "origin"
    branch = args.arg(1) or repo.refs.current_branch()

    rc = cmd_fetch(repo, ["--quiet", name])
    if rc != 0:
        return rc

    if not branch:
        raise GitPromptError("pull: cannot determine which branch to merge")

    remote_ref = f"refs/remotes/{name}/{branch}"
    remote_sha = repo.refs.read(remote_ref)
    if not remote_sha:
        raise GitPromptError(
            f"couldn't find remote ref '{branch}' on '{name}'\n"
            f"hint: available: {', '.join(sorted(
                r.split('/')[-1] for r in repo.refs.list(f'refs/remotes/{name}/')))}"
        )

    head = repo.refs.head_sha()
    if head == remote_sha:
        if not args.flag("q"):
            err("Already up to date.")
        return 0
    if head and repo.is_ancestor(remote_sha, head):
        if not args.flag("q"):
            err("Already up to date.")
        return 0

    from .history import cmd_merge
    merge_args = [f"refs/remotes/{name}/{branch}"]
    if args.flag("ff_only"):
        merge_args.append("--ff-only")
    if not args.flag("q"):
        err(f"Updating from {name}/{branch}")
    return cmd_merge(repo, merge_args)


# --------------------------------------------------------------------------

def cmd_serve(repo, argv):
    args = Args.parse([
        Opt("--port", "-p", takes_value=True, default="8765"),
        Opt("--host", takes_value=True, default="127.0.0.1"),
        Opt("--read-only"), Opt("--path", takes_value=True),
    ], argv)
    from ..server import serve
    root = args.get("path") or (repo.gpdir if repo else os.getcwd())
    return serve(root, host=args.get("host"), port=int(args.get("port")),
                 read_only=args.flag("read_only"))
