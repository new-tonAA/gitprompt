"""History commands — walk, compare and rearrange what has been recorded."""

from __future__ import annotations

import os
import sys

from ..argparser import Args, Opt
from ..diff import diff_trees, render_diff
from ..objects import (TYPE_COMMIT, TYPE_PROMPT, TYPE_SESSION, TYPE_TAG,
                       Commit, make_tag)
from ..refs import check_ref_name, full_branch_name
from ..revision import short_sha
from ..utils import (
    GitPromptError,
    format_tz,
    normalize_path,
    now_rfc3339,
    now_unix,
    paint,
    parse_rfc3339,
    tz_offset_minutes,
)
from ._helpers import (
    commit_line,
    err,
    in_date_range,
    matches,
    out,
    parse_date_filter,
    relpaths,
    working_status,
)


# --------------------------------------------------------------------------

def _collect_commits(repo, args, default_all=False):
    """Resolve the ref list a log/show/diff command should walk."""
    revs = [a for a in args.positional if not a.startswith("-")]
    if args.flag("all") or default_all:
        refs = sorted(set(repo.refs.list("refs/heads/").values())
                      | set(repo.refs.list("refs/remotes/").values()))
        if not refs:
            refs = [repo.refs.head_sha()] if repo.refs.head_sha() else []
        return refs, []
    if not revs:
        head = repo.refs.head_sha()
        return ([head] if head else []), []
    if ".." in revs[0]:
        left, _, right = revs[0].partition("..")
        return [_resolve_any(repo, right or "HEAD")], ([_resolve_any(repo, left)] if left else [])
    if len(revs) > 1 and "..." in args.positional:
        return [repo.resolve(revs[0])], [repo.resolve(revs[1])]
    return [_resolve_any(repo, revs[0])], revs[1:]


def _resolve_any(repo, spec):
    """Accept a commit-ish, a session id, or a prompt id as a starting point."""
    if spec.startswith("s_") or spec.startswith("sessions/"):
        session_id = spec.split("/")[-1]
        sha = repo.session_sha(session_id)
        if sha:
            return repo.refs.head_sha() or sha
    return repo.resolve(spec)


# --------------------------------------------------------------------------

def cmd_log(repo, argv):
    args = Args.parse([
        Opt("--oneline"), Opt("-n", "--max-count", takes_value=True),
        Opt("--all"), Opt("--graph"), Opt("--reverse"), Opt("--no-color"),
        Opt("--since", takes_value=True), Opt("--until", takes_value=True),
        Opt("--author", takes_value=True), Opt("--session", takes_value=True),
        Opt("--stat"), Opt("--name-only", takes_value="?"),
        Opt("-1"), Opt("--pretty", takes_value=True),
    ], argv)

    color = not args.flag("no_color")
    oneline = args.flag("oneline") or args.flag("1") or args.get("pretty") == "oneline"
    limit = int(args.get("n") or args.get("max_count") or 0)
    since = parse_date_filter(args.get("since"))
    until = parse_date_filter(args.get("until"))
    author_filter = args.get("author")
    session_filter = args.get("session")

    refs, excludes = _collect_commits(repo, args)
    if not refs:
        raise GitPromptError("your current branch does not have any commits yet")

    seen = set()
    commits = []
    for ref in refs:
        for sha, commit in repo.iter_commits(ref):
            if sha in seen:
                continue
            seen.add(sha)
            commits.append((sha, commit))

    if excludes:
        hidden = set()
        for ex in excludes:
            for sha, _ in repo.iter_commits(ex):
                hidden.add(sha)
        commits = [(s, c) for s, c in commits if s not in hidden]

    commits.sort(key=lambda pair: _commit_time(pair[1]), reverse=True)

    filtered = []
    for sha, commit in commits:
        if author_filter and author_filter.lower() not in commit.author.lower():
            continue
        if session_filter and commit.session != session_filter:
            continue
        ts = _commit_iso(commit)
        if not in_date_range(ts, since, until):
            continue
        filtered.append((sha, commit))

    if args.flag("reverse"):
        filtered.reverse()
    if limit:
        filtered = filtered[:limit]

    decorations = _build_decorations(repo)
    if oneline:
        for sha, commit in filtered:
            line = commit_line(repo, sha, commit,
                               decorate=decorations.get(sha, ""), color=color)
            if color:
                line = _colorize_oneline(line, commit)
            out(line)
        return 0

    blocks = []
    for i, (sha, commit) in enumerate(filtered):
        blocks.append(_format_commit(repo, sha, commit,
                                     decorations.get(sha, ""),
                                     color=color,
                                     show_stat=args.flag("stat")))
    out(("\n" if not args.flag("stat") else "").join(blocks).rstrip("\n"))
    return 0


def _colorize_oneline(line: str, commit: Commit) -> str:
    marker = ""
    if commit.session:
        marker = " " + paint(f"[{commit.session}]", "magenta")
    return line + marker


def _commit_time(commit: Commit) -> int:
    parts = commit.committer.split()
    for part in reversed(parts):
        if part.isdigit() and len(part) >= 9:
            return int(part)
    return 0


def _commit_iso(commit: Commit) -> str:
    ts = _commit_time(commit)
    if not ts:
        return ""
    from datetime import datetime, timedelta, timezone
    tz = commit.committer.split()[-1] if commit.committer else "+0000"
    try:
        sign = 1 if tz[0] == "+" else -1
        offset = timedelta(hours=int(tz[1:3]), minutes=int(tz[3:5])) * sign
    except (ValueError, IndexError):
        offset = timedelta(0)
    return datetime.fromtimestamp(ts, timezone(offset)).replace(microsecond=0).isoformat()


def _build_decorations(repo):
    deco = {}
    for name, sha in repo.refs.list("refs/heads/").items():
        deco.setdefault(sha, []).append(paint(name[len("refs/heads/"):], "green"))
    for name, sha in repo.refs.list("refs/tags/").items():
        deco.setdefault(sha, []).append(paint(name[len("refs/tags/"):], "yellow"))
    for name, sha in repo.refs.list("refs/remotes/").items():
        deco.setdefault(sha, []).append(paint("remotes/" + name[len("refs/remotes/"):], "red"))
    for name, sha in repo.refs.list("refs/sessions/").items():
        deco.setdefault(sha, []).append(paint("session:" + name[len("refs/sessions/"):], "magenta"))
    head = repo.refs.head_sha()
    if head:
        branch = repo.refs.current_branch()
        marker = paint(f"HEAD -> {branch}", "cyan") if branch else paint("HEAD", "cyan")
        deco.setdefault(head, []).insert(0, marker)
    return {sha: ", ".join(items) for sha, items in deco.items()}


def _format_commit(repo, sha, commit, decoration="", color=True, show_stat=False) -> str:
    lines = []
    head = short_sha(repo, sha, 7)
    if color:
        lines.append(paint(f"commit {head}", "yellow")
                     + (f" ({decoration})" if decoration else ""))
    else:
        lines.append(f"commit {head}" + (f" ({decoration})" if decoration else ""))
    if commit.parents:
        lines.append("parent" + ("s" if len(commit.parents) > 1 else "") + ": "
                     + " ".join(p[:7] for p in commit.parents))
    if commit.session:
        lines.append(paint(f"session: {commit.session}", "magenta"))
    lines.append(f"author: {commit.author}")
    lines.append(f"commit: {commit.committer}")
    if commit.model:
        lines.append(f"model:  {commit.model}")
    lines.append("")
    for line in (commit.message or "").strip().split("\n"):
        lines.append("    " + line)
    if show_stat:
        stats = _stat_for(repo, commit)
        if stats:
            lines.append("")
            lines.append(stats.rstrip("\n"))
    return "\n".join(lines) + "\n"


def _stat_for(repo, commit) -> str:
    parent_tree = {}
    if commit.parents:
        parent_tree = repo.read_tree(repo.commit_of(commit.parents[0]).tree)
    my_tree = repo.read_tree(commit.tree)
    diffs = list(diff_trees(repo, parent_tree, my_tree))
    return render_diff(diffs, stat_only=True)


# --------------------------------------------------------------------------

def cmd_show(repo, argv):
    args = Args.parse([
        Opt("--stat"), Opt("--oneline"), Opt("--no-color"),
        Opt("--session", takes_value=True), Opt("--prompt"),
        Opt("-s", "--no-patch"),
    ], argv)
    color = not args.flag("no_color")
    spec = args.positional[0] if args.positional else "HEAD"

    # A bare session id or prompt id shows that object instead of a commit.
    if spec.startswith("s_") and repo.session_sha(spec):
        return _show_session(repo, spec)
    if spec.startswith("p_") or args.flag("prompt"):
        sha = _find_prompt(repo, spec)
        if sha:
            return _show_prompt(repo, sha)

    sha = repo.peel_to_commit(repo.resolve(spec))
    commit = repo.commit_of(sha)
    decoration = _build_decorations(repo).get(sha, "")
    text = _format_commit(repo, sha, commit, decoration, color=color)
    if not args.flag("no_patch"):
        parent_tree = repo.read_tree(repo.commit_of(commit.parents[0]).tree) \
            if commit.parents else {}
        my_tree = repo.read_tree(commit.tree)
        diffs = list(diff_trees(repo, parent_tree, my_tree))
        if args.flag("stat"):
            text += "\n" + render_diff(diffs, stat_only=True)
        else:
            text += "\n" + render_diff(diffs, color=color)
    sys.stdout.write(text)
    return 0


def _find_prompt(repo, spec):
    if spec.startswith("p_"):
        for prompt in repo.iter_prompts():
            if prompt.id == spec:
                return prompt.sha
    try:
        return repo.resolve(spec)
    except GitPromptError:
        return None


def _show_prompt(repo, sha) -> int:
    from ..prompt import prompt_to_file
    prompt = repo.store.read(sha)
    if repo.store.type_of(sha) != TYPE_PROMPT:
        raise GitPromptError(f"{sha[:8]} is not a prompt object")
    sys.stdout.write(prompt_to_file(prompt))
    return 0


def _show_session(repo, session_id) -> int:
    session = repo.load_session(session_id)
    out(paint(f"session {session.id}", "bold"))
    out(f"title:   {session.title}")
    out(f"started: {session.started_at}")
    if session.ended_at:
        out(f"ended:   {session.ended_at}")
    if session.author.get("name"):
        out(f"author:  {session.author['name']} <{session.author.get('email', '')}>")
    if session.model:
        out(f"model:   {session.model}")
    if session.tags:
        out("tags:    " + ", ".join(session.tags))
    if session.notes:
        out(f"notes:   {session.notes}")
    prompts = repo.iter_prompts(session=session_id)
    out("")
    out(paint(f"{len(prompts)} prompt(s):", "bold"))
    for p in prompts:
        first = p.body.strip().split("\n", 1)[0]
        out(f"  {int(p.seq or 0):>3}. [{p.timestamp}] {first[:88]}")
    return 0


# --------------------------------------------------------------------------

def cmd_diff(repo, argv):
    args = Args.parse([
        Opt("--stat"), Opt("--cached", "--staged"), Opt("--no-color"),
        Opt("--name-only"), Opt("--name-status"), Opt("--prompts"),
        Opt("-U", "--unified", takes_value=True),
    ], argv)
    color = not args.flag("no_color")
    revs = [a for a in args.positional if not a.startswith("-")]
    pathspecs = relpaths(repo, [a for a in args.positional if _looks_like_path(a)])

    index = repo.read_index()

    if args.flag("cached", "staged"):
        base_tree = repo.head_tree()
        new_tree = {p: (e.mode, e.sha, _kind_of(repo, e.mode))
                    for p, e in index.entries.items()}
    elif revs:
        left, right = _two_sided(repo, revs)
        base_tree = repo.read_tree(repo.commit_of(left).tree)
        new_tree = repo.read_tree(repo.commit_of(right).tree)
    else:
        base_tree = {p: (e.mode, e.sha, _kind_of(repo, e.mode))
                     for p, e in index.entries.items()}
        new_tree = {}
        for relpath in repo.list_worktree_files("."):
            if repo.ignore_stack().is_ignored(relpath):
                continue
            if os.path.isdir(repo._fs_path(relpath)):
                continue
            mode, sha, _kind = repo.hash_worktree_file(relpath)
            new_tree[relpath] = (mode, sha, _kind_of(repo, mode))

    diffs = list(diff_trees(repo, base_tree, new_tree, pathspecs or None))
    if args.flag("prompts"):
        diffs = [d for d in diffs if d.prompt is not None]

    if args.flag("name_only"):
        for d in diffs:
            out(d.path)
        return 0
    if args.flag("name_status"):
        for d in diffs:
            out(f"{d.status[0].upper()}\t{d.path}")
        return 0
    if not diffs:
        return 0
    sys.stdout.write(render_diff(diffs, color=color, stat_only=args.flag("stat")))
    return 0


def _looks_like_path(value: str) -> bool:
    return value.startswith(("./", "../", "/")) or os.path.exists(value)


def _kind_of(repo, mode):
    return TYPE_PROMPT if mode == 0o100640 else "blob"


def _two_sided(repo, revs):
    spec = revs[0]
    if ".." in spec:
        left, _, right = spec.partition("..")
        return (repo.resolve(left) if left else repo.refs.head_sha(),
                repo.resolve(right or "HEAD"))
    if len(revs) >= 2:
        return repo.resolve(revs[0]), repo.resolve(revs[1])
    return repo.refs.head_sha(), repo.resolve(revs[0])


# --------------------------------------------------------------------------

def cmd_branch(repo, argv):
    args = Args.parse([
        Opt("-d", "--delete"), Opt("-D"), Opt("-m", "--move"), Opt("-M"),
        Opt("-a", "--all"), Opt("-r", "--remotes"), Opt("-v", "--verbose"),
        Opt("-f", "--force"), Opt("--set-upstream-to", takes_value=True),
        Opt("--unset-upstream"), Opt("--show-current"), Opt("--merged"),
        Opt("--contains", takes_value="?"),
    ], argv)

    if args.flag("show_current"):
        out(repo.refs.current_branch() or "")
        return 0

    if args.flag("delete") or args.flag("D"):
        if not args.positional:
            raise GitPromptError("branch: a branch name is required to delete")
        for name in args.positional:
            refname = full_branch_name(name)
            if not repo.refs.read(refname):
                raise GitPromptError(f"branch '{name}' not found")
            if repo.refs.head_ref() == refname and not args.flag("D"):
                raise GitPromptError(f"cannot delete branch '{name}' checked out at HEAD")
            if not args.flag("D") and args.get("merged") is not None:
                pass
            repo.refs.delete(refname)
            out(f"Deleted branch {name}")
        return 0

    if args.flag("move") or args.flag("M"):
        old = args.arg(0) or repo.refs.current_branch()
        new = args.arg(1) or args.arg(0)
        if not new:
            raise GitPromptError("branch: new branch name required")
        if not old:
            raise GitPromptError("branch: cannot rename a detached HEAD")
        src, dst = full_branch_name(old), full_branch_name(new)
        sha = repo.refs.read(src)
        if not sha:
            raise GitPromptError(f"branch '{old}' not found")
        if repo.refs.read(dst) and not args.flag("M"):
            raise GitPromptError(f"branch '{new}' already exists")
        repo.refs.update(dst, sha, message=f"branch: renamed refs/heads/{old} to refs/heads/{new}")
        repo.refs.delete(src, reflog=False)
        if repo.refs.head_ref() == src:
            repo.refs.set_head(dst)
        return 0

    if args.flag("set_upstream_to"):
        target = args.get("set_upstream_to")
        remote, _, branch = target.partition("/")
        current = repo.refs.current_branch()
        if not current:
            raise GitPromptError("branch: not on a branch")
        repo.config.set(f"branch.{current}.remote", remote)
        repo.config.set(f"branch.{current}.merge", f"refs/heads/{branch or current}")
        out(f"branch '{current}' set up to track '{target}'")
        return 0

    # create a branch when given a name and a start point
    if args.positional:
        name = args.positional[0]
        start = args.positional[1] if len(args.positional) > 1 else "HEAD"
        refname = full_branch_name(name)
        if repo.refs.read(refname) and not args.flag("f", "force"):
            raise GitPromptError(f"a branch named '{name}' already exists")
        sha = repo.peel_to_commit(repo.resolve(start))
        repo.refs.update(refname, sha, message=f"branch: Created from {start}")
        return 0

    return _list_branches(repo, args)


def _list_branches(repo, args) -> int:
    head_ref = repo.refs.head_ref()
    current = repo.refs.current_branch()
    detached = repo.refs.head_sha() if head_ref is None else None

    if detached:
        out(paint("* (HEAD detached at " + detached[:7] + ")", "cyan"))

    entries = []
    if not args.flag("remotes"):
        for name, sha in repo.refs.branches().items():
            entries.append((name, sha, False, f"refs/heads/{name}" == head_ref))
    if args.flag("all") or args.flag("remotes"):
        for name, sha in repo.refs.remotes().items():
            entries.append((name, sha, True, False))

    for name, sha, is_remote, is_current in sorted(entries):
        prefix = "* " if is_current else "  "
        if is_remote:
            prefix = "  "
        text = prefix + name
        if args.flag("v", "verbose"):
            try:
                commit = repo.store.read(sha)
                subject = (commit.message or "").strip().split("\n", 1)[0][:60]
                text += " " + sha[:7] + " " + subject
            except GitPromptError:
                text += " " + sha[:7]
        out(paint(text, "green") if is_current else text)
    return 0


# --------------------------------------------------------------------------

def cmd_checkout(repo, argv):
    args = Args.parse([
        Opt("-b", takes_value=True), Opt("-B", takes_value=True),
        Opt("-f", "--force"), Opt("--detach"), Opt("-q", "--quiet"),
        Opt("--", takes_value="?"),
    ], argv)

    new_branch = args.get("b") or args.get("B")
    detach = args.flag("detach")

    if new_branch:
        start = args.positional[0] if args.positional else "HEAD"
        sha = repo.peel_to_commit(repo.resolve(start))
        refname = full_branch_name(new_branch)
        if repo.refs.read(refname) and not args.get("B"):
            raise GitPromptError(f"a branch named '{new_branch}' already exists")
        repo.refs.update(refname, sha, message=f"branch: Created from {start}")
        return _switch_to(repo, refname, sha, args)

    if not args.positional:
        raise GitPromptError("checkout: a branch or commit is required")

    spec = args.positional[0]
    rest = args.positional[1:]

    # `checkout -- <paths>` restores files from the index
    if spec == "--" or (args.flag("__") and not rest):
        return _restore_paths(repo, args.positional[1:] or [], "index", args)

    try:
        sha = repo.peel_to_commit(repo.resolve(spec))
    except GitPromptError as exc:
        raise GitPromptError(f"pathspec '{spec}' did not match any branch, tag or commit ({exc})")

    # `checkout <ref> -- <paths>` restores those paths from that ref
    if "--" in argv:
        idx = argv.index("--")
        paths = argv[idx + 1:]
        return _restore_paths(repo, paths, spec, args)

    refname = full_branch_name(spec)
    if repo.refs.read(refname):
        return _switch_to(repo, refname, repo.refs.read(refname), args)
    if detach or True:
        return _switch_to(repo, None, sha, args)
    return 0


def _switch_to(repo, refname, sha, args) -> int:
    commit = repo.commit_of(sha)
    force = args.flag("f", "force")
    stats = repo.checkout_tree(commit.tree, force=force, update_index=True)
    if refname:
        repo.refs.set_head(refname)
    else:
        repo.refs.detach_head(sha)
    if not args.flag("q"):
        if refname:
            msg = f"Switched to branch '{refname[len('refs/heads/'):]}'"
        else:
            msg = f"Note: switching to '{sha[:7]}' (detached HEAD)"
        err(paint(msg, "green"))
        if stats["written"] or stats["removed"]:
            err(f"  {stats['written']} file(s) updated, {stats['removed']} removed")
    return 0


def _restore_paths(repo, paths, source, args) -> int:
    if not paths:
        raise GitPromptError("checkout: no paths given")
    rel = relpaths(repo, paths)
    tree = repo.head_tree() if source == "index" else \
        repo.read_tree(repo.commit_of(repo.resolve(source)).tree)
    index = repo.read_index()
    restored = 0
    for path in sorted(tree):
        if not matches(path, rel):
            continue
        mode, sha, objtype = tree[path]
        content = repo._object_bytes(sha, objtype)
        fs_path = repo._fs_path(path)
        if args.flag("f", "force") or not os.path.exists(fs_path):
            os.makedirs(os.path.dirname(fs_path), exist_ok=True)
            with open(fs_path, "wb") as fh:
                fh.write(content)
            restored += 1
        from ..index import stat_of
        index.add(path, sha, mode, stat_of(fs_path))
    repo.write_index(index)
    if not args.flag("q"):
        for path in rel:
            err(f"Restored '{path}'")
    return 0


def cmd_switch(repo, argv):
    args = Args.parse([
        Opt("-c", "--create", takes_value=True), Opt("-C", takes_value=True),
        Opt("-f", "--force"), Opt("-d", "--detach"),
    ], argv)
    create = args.get("c") or args.get("C")
    if create:
        return cmd_checkout(repo, ["-b", create] + args.positional)
    if not args.positional:
        current = repo.refs.current_branch()
        if current:
            out(current)
            return 0
        raise GitPromptError("switch: a branch name is required")
    return cmd_checkout(repo, list(args.positional))


# --------------------------------------------------------------------------

def cmd_merge(repo, argv):
    args = Args.parse([
        Opt("-m", "--message", takes_value=True),
        Opt("--no-commit"), Opt("--abort"), Opt("--ff-only"),
        Opt("--squash"), Opt("--strategy", takes_value=True),
    ], argv)
    if args.flag("abort"):
        return _merge_abort(repo)

    if not args.positional:
        raise GitPromptError("merge: a branch or commit is required")

    if repo.merge_head():
        pending = repo.merge_conflicts()
        detail = ("unresolved paths: " + ", ".join(pending)) if pending else "the merge is uncommitted"
        raise GitPromptError(
            f"you have not concluded your merge ({detail})\n"
            "hint: fix the files and 'gitprompt commit', or discard with 'gitprompt merge --abort'"
        )

    other = repo.peel_to_commit(repo.resolve(args.positional[0]))
    head = repo.refs.head_sha()
    if not head:
        raise GitPromptError("merge: HEAD has no commits yet")

    if repo.is_ancestor(other, head):
        out("Already up to date.")
        return 0
    if repo.is_ancestor(head, other):
        target = repo.commit_of(other)
        repo.checkout_tree(target.tree, force=True, update_index=True)
        branch_ref = repo.refs.head_ref()
        if branch_ref:
            repo.refs.update(branch_ref, other, message=f"merge {args.positional[0]}: Fast-forward")
        else:
            repo.refs.detach_head(other)
        out(f"Updating {head[:7]}..{other[:7]}\nFast-forward")
        return 0

    # The refusals come before any three-way work, so --ff-only never leaves a
    # conflicted tree behind for a merge the user asked not to happen.
    if args.flag("ff_only"):
        raise GitPromptError(
            f"not possible to fast-forward, aborting\n"
            f"hint: {args.positional[0]} has diverged from HEAD; "
            "merge without --ff-only, or rebase"
        )
    if args.flag("squash"):
        raise GitPromptError(
            "merge --squash is not implemented\n"
            "hint: merge, or use a plain commit"
        )

    base = repo.merge_base(head, other)
    if base is None:
        raise GitPromptError("refusing to merge unrelated histories")

    ours = commit_of_tree(repo, head)
    theirs = repo.read_tree(repo.commit_of(other).tree)
    base_tree = repo.read_tree(repo.commit_of(base).tree)

    merged, conflicts, sides = _three_way(repo, base_tree, ours, theirs)
    if conflicts:
        label = args.positional[0]
        for path in conflicts:
            b, o, t = sides[path]
            _write_conflict_markers(repo, path, b, o, t,
                                    ours_label="HEAD", theirs_label=label)
        # The state has to persist: resolving by hand and committing happens in
        # a later invocation, and without MERGE_HEAD that commit would have a
        # single parent — a merge that silently is not one.
        repo.set_merge_state(
            other,
            args.get("m") or f"Merge {label} into {repo.refs.current_branch() or 'HEAD'}",
            conflicts)
        err("Automatic merge failed; fix conflicts and commit the result.")
        err("Conflicting paths:")
        for path in conflicts:
            err(f"  both modified: {path}")
        raise GitPromptError(
            f"merge conflict in {len(conflicts)} file(s)\n"
            "hint: edit the marked files, then 'gitprompt add' them and 'gitprompt commit'"
        )

    tree_sha = repo.write_tree_from_paths(
        [(p, m, s) for p, (m, s, _t) in merged.items()]
    )
    if args.flag("no_commit"):
        # Prepare the merge but leave it for the user to commit.  The state has
        # to be recorded, or the follow-up commit cannot know it is a merge and
        # the second parent is lost.
        repo.checkout_tree(tree_sha, force=True, update_index=True)
        repo.set_merge_state(
            other,
            args.get("m") or f"Merge {args.positional[0]} into {repo.refs.current_branch() or 'HEAD'}",
            [])
        out("Merge prepared; not committing (--no-commit)")
        out(paint("hint: 'gitprompt commit' to conclude it", "dim"))
        return 0

    message = args.get("m") or f"Merge {args.positional[0]} into {repo.refs.current_branch() or 'HEAD'}"
    sha = repo.create_commit(tree_sha, [head, other], message)
    branch_ref = repo.refs.head_ref()
    if branch_ref:
        repo.refs.update(branch_ref, sha, message=f"merge {args.positional[0]}")
    else:
        repo.refs.detach_head(sha)
    repo.checkout_tree(tree_sha, force=True, update_index=True)
    out(f"Merge made by the 'three-way' strategy ({sha[:7]}).")
    return 0


def _merge_abort(repo):
    """Throw away an in-progress merge and put the tree back at HEAD."""
    if not repo.merge_head():
        raise GitPromptError("merge --abort: there is no merge to abort")
    head = repo.refs.head_sha()
    if head:
        repo.checkout_tree(repo.commit_of(head).tree, force=True, update_index=True)
    repo.clear_merge_state()
    out("Merge aborted; the working tree is back at HEAD.")
    return 0


def commit_of_tree(repo, sha):
    return repo.read_tree(repo.commit_of(sha).tree)


def _three_way(repo, base, ours, theirs):
    """Path-level three-way merge.

    Because a prompt history merges far more often than a codebase does, a
    conflict here is resolved by keeping *both* prompts rather than picking a
    winner: additive history is the common case, and dropping a prompt would
    silently break a replay.  A path whose contents genuinely diverged is
    reported as a conflict and left alone.

    Returns `(merged, conflicts, sides)`.  `sides` maps each conflicted path to
    its (base, ours, theirs) tree entries, so the caller can write conflict
    markers showing what actually diverged rather than just naming the file.
    """
    merged = {}
    conflicts = []
    sides = {}
    for path in set(base) | set(ours) | set(theirs):
        b, o, t = base.get(path), ours.get(path), theirs.get(path)
        if o == t:
            if o:
                merged[path] = o
            continue
        if o == b:
            if t:
                merged[path] = t
            continue
        if t == b:
            if o:
                merged[path] = o
            continue
        conflicts.append(path)
        sides[path] = (b, o, t)
        if o:
            merged[path] = o
    return merged, conflicts, sides


def _write_conflict_markers(repo, path, base_entry, ours_entry, theirs_entry,
                            ours_label="HEAD", theirs_label="MERGE_HEAD"):
    """Put a conflicted path on disk with markers around both versions.

    Without this the merge message tells the user to resolve a file by hand
    that nothing has touched — the file is still HEAD's version, with nothing
    to show what the other side said.  A base section is included (git's
    `diff3` style) because the common ancestor is what makes a prompt-file
    conflict readable: it shows which sentences each side added.
    """
    def text_of(entry):
        if not entry:
            return ""
        obj = repo.store.read(entry[1])
        if entry[2] == TYPE_PROMPT:
            # The file form, not just the body: a prompt's frontmatter carries
            # its id and session, and a resolved file that lost them would be
            # re-added as a new prompt and fall out of its session.
            from ..prompt import prompt_to_file
            return prompt_to_file(obj)
        return obj.data.decode("utf-8", "replace")

    body = []
    body.append(f"<<<<<<< {ours_label}")
    body.append(text_of(ours_entry).rstrip("\n"))
    if base_entry:
        body.append("||||||| base")
        body.append(text_of(base_entry).rstrip("\n"))
    body.append("=======")
    body.append(text_of(theirs_entry).rstrip("\n"))
    body.append(f">>>>>>> {theirs_label}")

    fs_path = repo._fs_path(path)
    parent = os.path.dirname(fs_path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(fs_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(body) + "\n")


# --------------------------------------------------------------------------

def cmd_tag(repo, argv):
    args = Args.parse([
        Opt("-l", "--list", takes_value="?"), Opt("-d", "--delete"),
        Opt("-m", "--message", takes_value=True), Opt("-a", "--annotate"),
        Opt("-f", "--force"), Opt("-n", takes_value="?"),
        Opt("--contains", takes_value="?"),
    ], argv)

    if args.flag("d", "delete"):
        if not args.positional:
            raise GitPromptError("tag: a tag name is required to delete")
        for name in args.positional:
            refname = "refs/tags/" + name
            if not repo.refs.read(refname):
                raise GitPromptError(f"tag '{name}' not found")
            repo.refs.delete(refname)
            out(f"Deleted tag '{name}'")
        return 0

    if args.positional and not args.flag("l", "list"):
        name = args.positional[0]
        target = args.positional[1] if len(args.positional) > 1 else "HEAD"
        sha = repo.peel_to_commit(repo.resolve(target))
        refname = check_ref_name("refs/tags/" + name)
        if repo.refs.read(refname) and not args.flag("f", "force"):
            raise GitPromptError(f"tag '{name}' already exists")
        if args.get("m") or args.flag("a", "annotate"):
            message = args.get("m") or f"tag {name}"
            tag = make_tag(sha, name, repo.commit_of(sha).committer, message)
            tag_sha = repo.store.write(TYPE_TAG, tag)
            repo.refs.update(refname, tag_sha, message=f"tag: {name}")
        else:
            repo.refs.update(refname, sha, message=f"tag: {name}")
        return 0

    pattern = args.get("l") if isinstance(args.get("l"), str) else None
    if args.positional and not pattern:
        pattern = args.positional[0]
    for name, sha in sorted(repo.refs.tags().items()):
        if pattern and pattern not in name:
            continue
        try:
            commit = repo.store.read(repo.peel_to_commit(sha))
            subject = (commit.message or "").strip().split("\n", 1)[0][:60]
            out(f"{name}    {subject}")
        except GitPromptError:
            out(name)
    return 0


# --------------------------------------------------------------------------

def cmd_reflog(repo, argv):
    args = Args.parse([Opt("-n", takes_value=True), Opt("--all")], argv)
    refname = args.positional[0] if args.positional else "HEAD"
    if refname == "HEAD":
        refname = repo.refs.head_ref() or "HEAD"
    entries = repo.refs.read_log(refname)
    if not entries:
        raise GitPromptError(f"no reflog for '{refname}'")
    limit = int(args.get("n") or 0)
    for i, entry in enumerate(entries[:limit] if limit else entries):
        ts = entry["ts"]
        from datetime import datetime
        when = datetime.fromtimestamp(int(ts)).strftime("%Y-%m-%d %H:%M:%S") if ts.isdigit() else ts
        out(f"{entry['new'][:7]} {refname}@{{{i}}}: {entry['message']}   ({when})")
    return 0


def cmd_describe(repo, argv):
    args = Args.parse([Opt("--tags"), Opt("--always"), Opt("--abbrev", takes_value="?")], argv)
    sha = repo.peel_to_commit(repo.resolve(args.positional[0] if args.positional else "HEAD"))
    width = int(args.get("abbrev") or 7) if str(args.get("abbrev") or "7").isdigit() else 7

    for name, tag_sha in repo.refs.tags().items():
        try:
            if repo.peel_to_commit(tag_sha) == sha:
                out(name)
                return 0
        except GitPromptError:
            continue

    for name, tag_sha in repo.refs.tags().items():
        try:
            base = repo.peel_to_commit(tag_sha)
        except GitPromptError:
            continue
        distance = 0
        for commit_sha, _c in repo.iter_commits(sha):
            if commit_sha == base:
                out(f"{name}-{distance}-g{sha[:width]}")
                return 0
            distance += 1
    if args.flag("always"):
        out(sha[:width])
        return 0
    raise GitPromptError("no tags can describe the given commit")
