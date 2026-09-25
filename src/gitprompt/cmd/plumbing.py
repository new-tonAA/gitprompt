"""Plumbing commands — the low-level layer.

These operate directly on objects and refs and make no assumptions about the
worktree.  They are the honest test of whether the object model is right: if
`hash-object`, `write-tree` and `commit-tree` behave, the porcelain on top of
them is bookkeeping.
"""

from __future__ import annotations

import os
import sys

from ..argparser import Args, Opt
from ..objects import (
    MODE_NAMES,
    MODE_TREE,
    TYPE_BLOB,
    TYPE_COMMIT,
    TYPE_PROMPT,
    TYPE_SESSION,
    TYPE_TREE,
    Commit,
    resolve_prefix,
)
from ..refs import check_ref_name
from ..utils import GitPromptError, ObjectNotFound, paint, to_bytes, to_text
from ._helpers import err, out


# --------------------------------------------------------------------------

def cmd_hash_object(repo, argv):
    args = Args.parse([
        Opt("-t", "--type", takes_value=True, default="blob"),
        Opt("-w", "--write"),
        Opt("--stdin"),
        Opt("--literally"),
    ], argv)
    objtype = args.get("t", "blob")
    write = args.flag("w")
    paths = list(args.positional)

    if args.flag("stdin"):
        payload = sys.stdin.buffer.read()
        sources = [(None, payload)]
    else:
        if not paths:
            raise GitPromptError("no files given (or use --stdin)")
        sources = []
        for path in paths:
            with open(path, "rb") as fh:
                sources.append((path, fh.read()))

    if repo is None:
        from ..objects import hash_object
        for _name, payload in sources:
            out(hash_object(objtype, payload))
        return 0

    for name, payload in sources:
        if objtype == TYPE_PROMPT and name:
            try:
                from ..prompt import file_to_prompt
                prompt = file_to_prompt(payload.decode("utf-8"), name)
                sha = repo.store.write(TYPE_PROMPT, prompt)
            except Exception:
                sha = repo.store.write_raw(TYPE_PROMPT, payload)
        elif objtype in (TYPE_BLOB, TYPE_TREE, TYPE_COMMIT, TYPE_PROMPT, TYPE_SESSION):
            sha = repo.store.write_raw(objtype, payload)
        else:
            raise GitPromptError(f"invalid object type '{objtype}'")
        out(sha)
    return 0


# --------------------------------------------------------------------------

def cmd_cat_file(repo, argv):
    args = Args.parse([
        Opt("-t"), Opt("-s"), Opt("-p"), Opt("-e"),
        Opt("--batch"), Opt("--batch-check"),
    ], argv)
    if not args.positional:
        raise GitPromptError("cat-file: an object name is required")
    sha = repo.resolve(args.positional[0])

    if args.flag("e"):
        return 0 if repo.store.exists(sha) else 1
    if args.flag("t"):
        out(repo.store.type_of(sha))
        return 0
    if args.flag("s"):
        _t, payload = repo.store.read_raw(sha)
        out(str(len(payload)))
        return 0
    if args.flag("p") or not any(args.flag(f) for f in ("t", "s", "e")):
        return _pretty(repo, sha)
    raise GitPromptError("cat-file: specify one of -t, -s, -p or -e")


def _pretty(repo, sha) -> int:
    objtype, payload = repo.store.read_raw(sha)
    if objtype == TYPE_TREE:
        for mode, name, entry_sha in repo.store.read(sha):
            out(f"{MODE_NAMES.get(mode, oct(mode)[2:])} {entry_sha}\t{name}")
        return 0
    if objtype == TYPE_PROMPT:
        from ..prompt import prompt_to_file
        sys.stdout.write(prompt_to_file(repo.store.read(sha)))
        return 0
    if objtype == TYPE_SESSION:
        import json
        out(json.dumps(repo.store.read(sha).to_dict(), indent=2, ensure_ascii=False))
        return 0
    sys.stdout.write(to_text(payload))
    return 0


# --------------------------------------------------------------------------

def cmd_ls_tree(repo, argv):
    args = Args.parse([
        Opt("-r"), Opt("-l", "--long"), Opt("-t"), Opt("-d"),
        Opt("--name-only"), Opt("--name-status"), Opt("--full-tree"),
        Opt("-z"),
    ], argv)
    if not args.positional:
        raise GitPromptError("ls-tree: a tree-ish is required")
    spec = args.positional[0]
    pathspecs = args.positional[1:]

    sha = repo.peel_to_commit(spec) if repo.store.type_of(repo.resolve(spec)) == TYPE_COMMIT \
        else repo.resolve(spec)
    objtype = repo.store.type_of(sha)
    if objtype == TYPE_COMMIT:
        trees = repo.read_tree(repo.store.read(sha).tree)
        _emit_flat(repo, trees, args, pathspecs)
        return 0
    if objtype == TYPE_TREE:
        if args.flag("r"):
            _emit_flat(repo, repo.read_tree(sha), args, pathspecs)
        else:
            for mode, name, entry_sha in repo.store.read(sha):
                if pathspecs and not any(name == p or name.startswith(p.rstrip("/") + "/")
                                         for p in pathspecs):
                    continue
                _emit_entry(repo, mode, name, entry_sha, args)
        return 0
    raise GitPromptError(f"not a tree object: {spec}")


def _emit_flat(repo, flat, args, pathspecs):
    names_only = args.flag("name_only")
    for path in sorted(flat):
        if pathspecs and not any(path == p or path.startswith(p.rstrip("/") + "/")
                                 for p in pathspecs):
            continue
        mode, sha, objtype = flat[path]
        if args.flag("d"):
            continue
        if names_only:
            out(path)
        else:
            _emit_entry(repo, mode, path, sha, args, objtype)


def _emit_entry(repo, mode, name, sha, args, objtype=None):
    objtype = objtype or (TYPE_PROMPT if mode == 0o100640 else
                          TYPE_TREE if mode == MODE_TREE else TYPE_BLOB)
    if args.flag("l", "long"):
        try:
            size = len(repo.store.read_raw(sha)[1])
        except GitPromptError:
            size = 0
        out(f"{mode:06o} {objtype} {sha}\t{'    ' if size < 1000 else ''}{size}\t{name}")
    else:
        out(f"{mode:06o} {objtype} {sha}\t{name}")


# --------------------------------------------------------------------------

def cmd_write_tree(repo, argv):
    args = Args.parse([Opt("--missing-ok"), Opt("--prefix", takes_value=True)], argv)
    index = repo.read_index()
    if len(index) == 0:
        raise GitPromptError("write-tree: the index is empty; run 'gitprompt add' first")
    out(repo.write_tree_from_index(index))
    return 0


# --------------------------------------------------------------------------

def cmd_commit_tree(repo, argv):
    args = Args.parse([
        Opt("-p", takes_value=True, repeatable=True),
        Opt("-m", "--message", takes_value=True, repeatable=True),
        Opt("-F", "--file", takes_value=True),
        Opt("--session", takes_value=True),
        Opt("-S"),
    ], argv)
    if not args.positional:
        raise GitPromptError("commit-tree: a tree object is required")
    tree = repo.resolve(args.positional[0])
    if repo.store.type_of(tree) != TYPE_TREE:
        raise GitPromptError(f"{args.positional[0]} is not a tree object")

    parents = [repo.peel_to_commit(p) for p in args.list("p")]
    message = "\n\n".join(args.list("m"))
    if args.get("F"):
        if args.get("F") == "-":
            message = sys.stdin.read()
        else:
            with open(args.get("F"), "r", encoding="utf-8") as fh:
                message = fh.read()
    if not message:
        message = sys.stdin.read()
    if not message.strip():
        raise GitPromptError("commit-tree: empty commit message")

    session = args.get("session", repo.current_session_id())
    sha = repo.create_commit(tree, parents, message, session=session)
    out(sha)
    return 0


# --------------------------------------------------------------------------

def cmd_rev_parse(repo, argv):
    args = Args.parse([
        Opt("--verify"), Opt("--quiet", "-q"), Opt("--short", takes_value="?"),
        Opt("--abbrev-ref"), Opt("--is-inside-work-tree"), Opt("--is-bare-repository"),
        Opt("--git-dir"), Opt("--show-toplevel"), Opt("--show-prefix"),
        Opt("--symbolic"), Opt("--all"), Opt("--not"), Opt("--default", takes_value=True),
    ], argv)

    if args.flag("is_inside_work_tree"):
        out("true" if (repo and not repo.is_bare) else "false")
        return 0
    if args.flag("is_bare_repository"):
        out("true" if (repo and repo.is_bare) else "false")
        return 0
    if args.flag("git_dir"):
        if repo is None:
            raise GitPromptError("not a gitprompt repository")
        out(repo.gpdir)
        return 0
    if args.flag("show_toplevel"):
        if repo is None or repo.is_bare:
            raise GitPromptError("this operation must be run in a work tree")
        out(repo.root)
        return 0
    if args.flag("all"):
        for name, sha in sorted(repo.refs.list("refs/").items()):
            out(sha)
        return 0

    if not args.positional:
        raise GitPromptError("rev-parse: no arguments")

    failed = False
    for spec in args.positional:
        try:
            if args.flag("abbrev_ref"):
                ref = repo.refs.head_ref() if spec == "HEAD" else spec
                out(ref[len("refs/heads/"):] if ref and ref.startswith("refs/heads/")
                    else (ref or spec))
                continue
            if spec in ("HEAD", "@") and repo.refs.head_sha() is None and args.flag("verify"):
                raise GitPromptError(f"unknown revision: {spec}")
            sha = repo.resolve(spec)
            if "short" in args.values:
                from ..revision import short_sha
                width = args.get("short")
                n = int(width) if str(width).isdigit() else 7
                out(short_sha(repo, sha, n))
            else:
                out(sha)
        except GitPromptError as exc:
            if args.flag("quiet"):
                return 1
            if args.flag("verify"):
                raise GitPromptError(f"Needed a single revision: {exc}")
            if args.get("default") is not None:
                out(args.get("default"))
                continue
            failed = True
            err(str(exc))
    return 128 if failed else 0


# --------------------------------------------------------------------------

def cmd_update_ref(repo, argv):
    args = Args.parse([Opt("-m", takes_value=True), Opt("-d", "--delete"),
                       Opt("--no-deref")], argv)
    if not args.positional:
        raise GitPromptError("update-ref: a ref name is required")
    refname = check_ref_name(args.positional[0])

    if args.flag("d", "delete"):
        repo.refs.delete(refname)
        return 0
    if len(args.positional) < 2:
        raise GitPromptError("update-ref: a new value is required")
    newvalue = repo.resolve(args.positional[1])
    oldvalue = None
    if len(args.positional) >= 3:
        oldvalue = repo.resolve(args.positional[2])
    repo.refs.update(refname, newvalue, oldvalue, args.get("m") or f"update-ref: {newvalue}")
    return 0


# --------------------------------------------------------------------------

def cmd_symbolic_ref(repo, argv):
    args = Args.parse([Opt("-d", "--delete"), Opt("-q", "--quiet"), Opt("--short")], argv)
    if not args.positional:
        raise GitPromptError("symbolic-ref: a ref name is required")
    refname = args.positional[0]

    if args.flag("d", "delete"):
        if repo.refs.read_symbolic(refname):
            path = repo.refs._path(refname)
            if os.path.exists(path):
                os.remove(path)
            return 0
        if not args.flag("quiet"):
            raise GitPromptError(f"ref {refname} is not a symbolic ref")
        return 1

    if len(args.positional) == 1:
        target = repo.refs.read_symbolic(refname)
        if not target:
            if args.flag("quiet"):
                return 1
            raise GitPromptError(f"ref {refname} is not a symbolic ref")
        out(target[len("refs/heads/"):] if (args.flag("short")
            and target.startswith("refs/heads/")) else target)
        return 0

    repo.refs.write_symbolic(refname, args.positional[1])
    return 0


# --------------------------------------------------------------------------

def cmd_count_objects(repo, argv):
    args = Args.parse([Opt("-v", "--verbose"), Opt("-H", "--human-readable")], argv)
    loose = list(repo.store.iter_loose())
    total_bytes = 0
    by_type = {}
    for sha in loose:
        try:
            objtype, payload = repo.store.read_raw(sha)
        except GitPromptError:
            continue
        path = repo.store.object_path(sha)
        total_bytes += os.path.getsize(path) if os.path.exists(path) else 0
        by_type[objtype] = by_type.get(objtype, 0) + 1

    if args.flag("v", "verbose"):
        out(f"count: {len(loose)}")
        out(f"size: {total_bytes // 1024}")
        out(f"in-pack: 0")
        out(f"packs: 0")
        out(f"prune-packable: 0")
        out(f"garbage: 0")
        out(f"size-garbage: 0")
        if by_type:
            out("")
            for name in sorted(by_type):
                out(f"{name}: {by_type[name]}")
    else:
        out(f"{len(loose)} objects, {total_bytes // 1024} KiB")
    return 0


def cmd_verify_objects(repo, argv):
    args = Args.parse([Opt("--strict"), Opt("-q", "--quiet")], argv)
    from ..objects import hash_object
    bad, checked = [], 0
    for sha in repo.store.iter_loose():
        try:
            objtype, payload = repo.store.read_raw(sha)
        except Exception as exc:
            bad.append((sha, f"unreadable: {exc}"))
            continue
        actual = hash_object(objtype, payload, repo.store.fmt)
        checked += 1
        if actual != sha:
            bad.append((sha, f"hash mismatch (content hashes to {actual})"))
    if not args.flag("quiet"):
        for sha, reason in bad:
            err(f"error: {sha}: {reason}")
    out(f"Checked {checked} object(s): {'no problems found' if not bad else f'{len(bad)} corrupt'}")
    return 1 if bad else 0


def cmd_check_ref_format(repo, argv):
    args = Args.parse([Opt("--allow-onelevel"), Opt("--normalize"), Opt("--branch")], argv)
    if not args.positional:
        raise GitPromptError("check-ref-format: a ref name is required")
    name = args.positional[0]
    if args.flag("branch"):
        name = name if name.startswith("refs/heads/") else "refs/heads/" + name
    try:
        check_ref_name(name)
    except GitPromptError:
        return 1
    out(name if args.flag("normalize") else "")
    return 0


def cmd_for_each_ref(repo, argv):
    args = Args.parse([Opt("--format", takes_value=True), Opt("--sort", takes_value=True),
                       Opt("--count", takes_value=True)], argv)
    pattern = args.positional[0] if args.positional else "refs/"
    fmt = args.get("format") or "%(refname) %(objectname)"
    refs = repo.refs.list("refs/")
    rows = []
    for name, sha in sorted(refs.items()):
        if not name.startswith(pattern.rstrip("*")):
            continue
        objtype = "?"
        try:
            objtype = repo.store.type_of(sha)
        except GitPromptError:
            pass
        rows.append((name, sha, objtype))
    if args.get("sort", "").startswith("-"):
        rows.sort(key=lambda r: r[0], reverse=True)
    count = args.get("count")
    if count:
        rows = rows[:int(count)]
    for name, sha, objtype in rows:
        out(fmt.replace("%(refname)", name)
              .replace("%(refname:short)", name.replace("refs/heads/", "")
                       .replace("refs/tags/", "").replace("refs/sessions/", ""))
              .replace("%(objectname)", sha)
              .replace("%(objectname:short)", sha[:7])
              .replace("%(objecttype)", objtype))
    return 0


# --------------------------------------------------------------------------

def cmd_show_ref(repo, argv):
    for name, sha in sorted(repo.refs.list("refs/").items()):
        out(f"{sha} {name}")
    return 0
