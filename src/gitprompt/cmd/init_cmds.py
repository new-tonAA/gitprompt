"""Repository creation and configuration: init, config, clone."""

from __future__ import annotations

import os
import shutil

from ..argparser import Args, Opt
from ..config import Config, default_branch, prompt_dir
from ..repo import Repository
from ..transport import open_transport, transfer_objects, remote_kind
from ..utils import GitPromptError, normalize_path, paint, to_text
from ._helpers import err, out


# --------------------------------------------------------------------------

def cmd_init(repo, argv):
    args = Args.parse([
        Opt("--bare"),
        Opt("--object-format", takes_value=True, default="sha1"),
        Opt("-b", "--initial-branch", takes_value=True),
        Opt("--separate-git-dir", takes_value=True),
        Opt("-q", "--quiet"),
        Opt("--prompt-dir", takes_value=True),
    ], argv)
    path = args.arg(0) or "."
    new = Repository.init(
        path,
        bare=args.flag("bare"),
        fmt=args.get("object_format", "sha1"),
        initial_branch=args.get("b") or args.get("initial_branch"),
        quiet=args.flag("q"),
    )
    if args.get("prompt_dir"):
        new.reload_config()
        new.config.set("gitprompt.promptDir", args.get("prompt_dir"))
    if not args.flag("bare"):
        os.makedirs(os.path.join(new.root, prompt_dir(new.config)), exist_ok=True)
        _seed_gitpromptignore(new)
    return 0


def _seed_gitpromptignore(repo):
    path = os.path.join(repo.root, ".gitpromptignore")
    if os.path.exists(path):
        return
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(
            "# Files matching these patterns are never tracked by gitprompt.\n"
            "# Same syntax as .gitignore.\n"
            "\n"
            ".git/\n"
            ".gitprompt/\n"
            "__pycache__/\n"
            "*.pyc\n"
            ".DS_Store\n"
            "Thumbs.db\n"
        )


# --------------------------------------------------------------------------

def cmd_config(repo, argv):
    args = Args.parse([
        Opt("--global"), Opt("--local"), Opt("--system"),
        Opt("-l", "--list"), Opt("--get", takes_value=True),
        Opt("--get-all", takes_value=True), Opt("--unset", takes_value=True),
        Opt("--unset-all", takes_value=True), Opt("--remove-section", takes_value=True),
        Opt("-f", "--file", takes_value=True), Opt("-e", "--edit"),
        Opt("--show-origin"), Opt("--type", takes_value=True),
    ], argv)

    scope = "local"
    if args.flag("global"):
        scope = "global"
    elif args.flag("system"):
        scope = "system"

    if args.get("file"):
        cfg = Config()
        path = args.get("file")
        cfg._files["file"] = path
        cfg._read_file(path)
        return _config_run(cfg, args, repo, scope_override=path)

    if args.flag("list"):
        cfg = Config.load(repo) if repo else Config.load(None)
        if args.get("file"):
            cfg = Config()
            cfg._read_file(args.get("file"))
        for key, value in cfg.items():
            if args.flag("show_origin"):
                out(f"{key}={value}")
            else:
                out(f"{key}={value}")
        return 0

    if repo is None and scope == "local":
        raise GitPromptError(
            "not in a gitprompt repository\n"
            "hint: use --global to read or write the global config"
        )
    return _config_run(Config.load(repo) if repo else Config.load(None), args, repo, scope)


def _config_run(cfg, args, repo, scope="local"):
    if args.flag("e") or args.flag("edit"):
        path = cfg._files.get(scope) or Config.global_path()
        err(f"hint: edit {path} directly; interactive editing is not implemented")
        return 0

    if args.get("get"):
        value = cfg.get(args.get("get"))
        if value is None:
            return 1
        out(value)
        return 0
    if args.get("get_all"):
        values = cfg.get_all(args.get("get_all"))
        if not values:
            return 1
        for value in values:
            out(value)
        return 0
    if args.get("unset"):
        if not cfg.unset(args.get("unset"), scope):
            raise GitPromptError(f"key '{args.get('unset')}' not found")
        return 0
    if args.get("unset_all"):
        if not cfg.unset(args.get("unset_all"), scope):
            raise GitPromptError(f"key '{args.get('unset_all')}' not found")
        return 0
    if args.get("remove_section"):
        removed = cfg.unset_section(args.get("remove_section"), scope)
        if not removed:
            raise GitPromptError(f"section '{args.get("remove_section")}' not found")
        return 0

    if not args.positional:
        raise GitPromptError(
            "config: an action is required\n"
            "hint: gitprompt config --list | --get <key> | <key> <value> | --unset <key>"
        )
    key = args.positional[0]
    if len(args.positional) == 1:
        value = cfg.get(key)
        if value is None:
            return 1
        out(value)
        return 0
    value = " ".join(args.positional[1:])
    cfg.set(key, value, scope)
    if repo and scope == "local":
        repo.reload_config()
    return 0


# --------------------------------------------------------------------------

def cmd_clone(repo, argv):
    args = Args.parse([
        Opt("--bare"), Opt("--depth", takes_value=True),
        Opt("-b", "--branch", takes_value=True),
        Opt("-q", "--quiet"), Opt("--origin", takes_value=True, default="origin"),
    ], argv)
    if not args.positional:
        raise GitPromptError("clone: a repository URL is required")

    url = args.positional[0]
    dest = args.positional[1] if len(args.positional) > 1 else _default_dir_name(url)
    origin = args.get("origin") or "origin"

    if os.path.exists(dest) and os.listdir(dest):
        raise GitPromptError(f"destination path '{dest}' already exists and is not empty")

    kind = remote_kind(url)
    if not args.flag("q"):
        err(f"Cloning into '{dest}' ({kind} transport) ...")

    if kind == "local":
        source = _open_local(url)
        target = Repository.init(dest, bare=args.flag("bare"), fmt=source.store.fmt, quiet=True)
        target.config.set("remote.%s.url" % origin, source.remote_url)
        target.config.set("remote.%s.fetch" % origin, "+refs/heads/*:refs/remotes/%s/*" % origin)
        target.reload_config()
        refs = _local_refs(source)
        wants = list(refs.values())
        copied = transfer_objects(source.store, target.store, wants)
        _install_refs(target, refs, origin, bare=args.flag("bare"),
                      branch=args.get("b"), remote_url=source.remote_url)
    else:
        transport = open_transport(url, _mirror_cache(dest, origin), origin)
        target = Repository.init(dest, bare=args.flag("bare"), fmt="sha1", quiet=True)
        target.config.set("remote.%s.url" % origin, url)
        target.config.set("remote.%s.fetch" % origin, "+refs/heads/*:refs/remotes/%s/*" % origin)
        target.config.set("remote.%s.mirror" % origin, "true")
        target.reload_config()

        refs = transport.refs().refs
        wants = list(refs.values())
        if hasattr(transport, "materialise"):
            copied = transport.materialise(target.store, wants)
        else:
            copied = transfer_objects(transport.store, target.store, wants)
        if not wants or copied == 0:
            raise GitPromptError(
                f"remote '{url}' has no gitprompt history\n"
                "hint: push one first with 'gitprompt push', or check the URL"
            )
        _install_refs(target, refs, origin, bare=args.flag("bare"), branch=args.get("b"),
                      remote_url=url)

    if not args.flag("bare"):
        head = target.refs.head_sha()
        if head:
            target.checkout_tree(target.commit_of(head).tree, force=True, update_index=True)
        os.makedirs(os.path.join(target.root, prompt_dir(target.config)), exist_ok=True)

    if not args.flag("q"):
        head = target.refs.head_sha()
        if head:
            commit = target.commit_of(head)
            n_prompts = len(target.iter_prompts())
            err(f"  {n_prompts} prompt(s) in history")
            err(f"  head is {head[:7]} — {commit.message.strip().splitlines()[0][:60]}")
            err(paint("  run 'gitprompt replay' to reconstruct the prompt timeline", "dim"))
        else:
            err("  warning: cloned repository appears to be empty")
    return 0


def _default_dir_name(url: str) -> str:
    name = url.rstrip("/").split("/")[-1]
    if name.endswith(".git"):
        name = name[:-4]
    return name or "repository"


def _mirror_cache(dest: str, origin: str) -> str:
    return os.path.join(os.path.abspath(dest), ".gitprompt", "remotes", origin, "mirror")


def _open_local(url: str):
    from ..objects import ObjectStore
    from ..refs import RefStore
    path = url[7:] if url.startswith("file://") else url
    path = os.path.abspath(os.path.expanduser(path))
    if os.path.isdir(os.path.join(path, "objects")):
        gpdir, root = path, None          # a bare repository
    else:
        gpdir, root = os.path.join(path, ".gitprompt"), path
    if not os.path.isdir(gpdir):
        raise GitPromptError(f"'{url}' is not a gitprompt repository")
    return type("LocalSource", (), {
        "store": ObjectStore(gpdir),
        "gpdir": gpdir,
        "root": root,
        "is_bare": root is None,
        "refs": RefStore(gpdir),
        # Point the clone's remote at the checkout root when there is one:
        # naming the `.gitprompt` directory makes the remote look bare, and a
        # push into it would then leave the origin's working tree stale.
        "remote_url": root or gpdir,
    })


def _local_refs(source):
    refs = dict(source.refs.list("refs/"))
    head = source.refs.head_sha()
    if head:
        refs["HEAD"] = head
        # The name matters as much as the sha: two branches pointing at the
        # same commit are indistinguishable by sha, and picking the wrong one
        # would check the clone out on a branch the origin does not use.
        head_ref = source.refs.head_ref()
        if head_ref:
            refs["HEAD_REF"] = head_ref
    return refs


def _install_refs(target, refs, origin, bare=False, branch=None, remote_url=None):
    """Set up remote-tracking refs and check out a local branch, as git does."""
    branches = {k: v for k, v in refs.items() if k.startswith("refs/heads/")}
    tags = {k: v for k, v in refs.items() if k.startswith("refs/tags/")}
    sessions = {k: v for k, v in refs.items() if k.startswith("refs/sessions/")}

    for name, sha in branches.items():
        target.refs.update(f"refs/remotes/{origin}/{name[len('refs/heads/'):]}", sha,
                           reflog=False)
    for name, sha in tags.items():
        target.refs.update(name, sha, reflog=False)
    for name, sha in sessions.items():
        target.refs.update(name, sha, reflog=False)

    if not branches:
        return

    if branch:
        wanted = "refs/heads/" + branch
        if wanted not in branches:
            raise GitPromptError(
                f"remote branch '{branch}' not found\n"
                f"available: {', '.join(k[len('refs/heads/'):] for k in sorted(branches))}"
            )
        chosen = wanted
    else:
        # prefer the branch the remote's HEAD points at, so a clone lands on
        # the same branch the origin considers default
        head_ref = refs.get("HEAD_REF")
        chosen = None
        if head_ref in branches:
            chosen = head_ref
        if chosen is None:
            remote_head = refs.get("HEAD")
            if remote_head:
                for name, value in branches.items():
                    if value == remote_head:
                        chosen = name
                        break
        if chosen is None:
            fallback = "refs/heads/" + default_branch(target.config)
            chosen = fallback if fallback in branches else sorted(branches)[0]

    target_branch = chosen[len("refs/heads/"):]
    target.refs.set_head(f"refs/heads/{target_branch}")

    if bare:
        return

    target.refs.update(f"refs/heads/{target_branch}", branches[chosen],
                       message=f"clone: from {remote_url or origin}")
    target.config.set(f"branch.{target_branch}.remote", origin)
    target.config.set(f"branch.{target_branch}.merge", f"refs/heads/{target_branch}")
    target.reload_config()
