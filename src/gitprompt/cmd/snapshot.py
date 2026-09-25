"""Snapshot commands — move worktree state into history.

`add` / `rm` / `mv` / `status` / `commit` / `reset`.  The behaviour is git's;
what differs is only that prompt files under the prompt directory become
`prompt` objects along the way, so a commit records provenance and not just
content.
"""

from __future__ import annotations

import os
import shutil
import sys

from ..argparser import Args, Opt
from ..index import Index, stat_of
from ..objects import MODE_BLOB, MODE_PROMPT, TYPE_PROMPT
from ..prompt import prompt_to_file
from ..utils import GitPromptError, normalize_path, paint
from ._helpers import err, matches, out, relpaths, resolve_paths, working_status


# --------------------------------------------------------------------------

def cmd_add(repo, argv):
    args = Args.parse([
        Opt("-A", "--all"),
        Opt("-u", "--update"),
        Opt("-f", "--force"),
        Opt("-n", "--dry-run"),
        Opt("-v", "--verbose"),
        Opt("-p", "--patch"),
    ], argv)

    pathspecs = resolve_paths(repo, args.positional, allow_all=False)
    do_all = args.flag("A", "all") or args.flag("u", "update")
    if not pathspecs and not do_all:
        raise GitPromptError("nothing specified, nothing added")

    index = repo.read_index()
    head = repo.head_tree()
    added, removed = [], []

    if do_all:
        # every tracked file, whether or not it still exists on disk
        candidates = set(index.paths())
        if args.flag("A", "all"):
            candidates |= set(repo.list_worktree_files("."))
        else:
            candidates |= {p for p in repo.list_worktree_files(".") if p in index}
    else:
        candidates = set()
        for spec in pathspecs:
            fs_path = repo._fs_path(spec)
            if os.path.isdir(fs_path):
                candidates |= set(repo.list_worktree_files(spec))
            else:
                candidates.add(spec)

    for relpath in sorted(candidates):
        relpath = normalize_path(relpath)
        fs_path = repo._fs_path(relpath)

        if not os.path.exists(fs_path):
            if relpath in index:
                if args.flag("dry_run"):
                    out(f"remove '{relpath}'")
                else:
                    index.remove(relpath)
                    removed.append(relpath)
            elif not do_all:
                raise GitPromptError(f"pathspec '{relpath}' did not match any files")
            continue

        if os.path.isdir(fs_path):
            continue
        if repo.ignore_stack().is_ignored(relpath) and not args.flag("f", "force"):
            if not do_all:
                err(f"The following paths are ignored by one of your .gitpromptignore files:\n{relpath}\n"
                    "hint: Use -f if you really want to add them.")
            continue

        mode, sha, kind = repo.hash_worktree_file(relpath)
        if args.flag("dry_run"):
            out(f"add '{relpath}'")
            continue
        index.add(relpath, sha, mode, stat_of(repo._fs_path(relpath)))
        added.append(relpath)
        if args.flag("v", "verbose"):
            out(f"add '{relpath}'")

    if not args.flag("dry_run"):
        repo.write_index(index)
        # Staging a conflicted path is how a merge conflict is declared
        # resolved.  Without this the conflict list never empties and the
        # merge can never be concluded.
        if added and repo.merge_head():
            for relpath in added:
                repo.resolve_merge_path(relpath)
    if args.flag("p", "patch"):
        err("hint: interactive hunk selection is not implemented; "
            "all changes to the named paths were staged")
    return 0


# --------------------------------------------------------------------------

def cmd_rm(repo, argv):
    args = Args.parse([Opt("-r"), Opt("--cached"), Opt("-f", "--force"),
                       Opt("--ignore-unmatch"), Opt("-q")], argv)
    pathspecs = relpaths(repo, args.positional)
    if not pathspecs:
        raise GitPromptError("rm: no paths given")

    index = repo.read_index()
    head = repo.head_tree()
    removed_cached, removed_disk = [], []

    for spec in pathspecs:
        fs_path = repo._fs_path(spec)
        targets = [spec]
        if os.path.isdir(fs_path):
            if not args.flag("r"):
                raise GitPromptError(f"not removing '{spec}' recursively without -r")
            targets = [p for p in index.paths() if matches(p, [spec])]
        for relpath in targets:
            entry = index.get(relpath)
            if entry is None:
                if not args.flag("ignore_unmatch"):
                    raise GitPromptError(f"pathspec '{relpath}' did not match any files")
                continue
            if not args.flag("force") and not args.flag("cached"):
                head_entry = head.get(relpath)
                if head_entry and head_entry[1] != entry.sha:
                    raise GitPromptError(
                        f"the following file has staged content different from "
                        f"both the file and HEAD: {relpath}\n"
                        "use --cached to keep the file, or -f to force removal"
                    )
            index.remove(relpath)
            removed_cached.append(relpath)
            if not args.flag("cached") and os.path.exists(repo._fs_path(relpath)):
                os.remove(repo._fs_path(relpath))
                removed_disk.append(relpath)

    repo.write_index(index)
    if not args.flag("q"):
        for path in removed_disk:
            out(f"rm '{path}'")
    return 0


# --------------------------------------------------------------------------

def cmd_mv(repo, argv):
    args = Args.parse([Opt("-f", "--force"), Opt("-k"), Opt("-v", "--verbose")], argv)
    if len(args.positional) < 2:
        raise GitPromptError("mv: need <source> and <destination>")
    sources = relpaths(repo, args.positional[:-1])
    dest = relpaths(repo, [args.positional[-1]])[0]

    index = repo.read_index()
    dest_is_dir = os.path.isdir(repo._fs_path(dest))
    if len(sources) > 1 and not dest_is_dir:
        raise GitPromptError("mv: destination must be a directory when moving several files")

    for src in sources:
        if src not in index and not args.flag("k"):
            raise GitPromptError(f"mv: '{src}' is not under version control (use -k to ignore)")
        target = f"{dest}/{os.path.basename(src)}" if dest_is_dir else dest
        src_fs, dst_fs = repo._fs_path(src), repo._fs_path(target)
        if os.path.exists(dst_fs) and not args.flag("f", "force"):
            raise GitPromptError(f"mv: destination '{target}' already exists (use -f to overwrite)")
        os.makedirs(os.path.dirname(dst_fs), exist_ok=True)
        shutil.move(src_fs, dst_fs)
        index.remove(src)
        mode, sha, _kind = repo.hash_worktree_file(target)
        index.add(target, sha, mode, stat_of(dst_fs))
        if args.flag("v", "verbose"):
            out(f"Renaming {src} to {target}")
    repo.write_index(index)
    return 0


# --------------------------------------------------------------------------

def cmd_status(repo, argv):
    args = Args.parse([
        Opt("-s", "--short"), Opt("--porcelain"),
        Opt("-b", "--branch"), Opt("--long"), Opt("--prompts"),
    ], argv)
    index = repo.read_index()
    head_tree = repo.head_tree()
    state = working_status(repo, index, head_tree)
    cfg = repo.config
    branch = repo.refs.current_branch()
    color = not args.flag("porcelain")

    if args.flag("s", "short") or args.flag("porcelain"):
        return _short_status(repo, state, branch, args.flag("branch"))

    if branch:
        tracking = _tracking_line(repo, branch)
        out(paint(f"On branch {branch}", "bold") + (f"\n{tracking}" if tracking else ""))
    else:
        sha = repo.refs.head_sha()
        out(paint("HEAD detached at " + (sha[:7] if sha else "(no commits yet)"), "bold"))

    if not repo.refs.head_sha():
        out("\nNo commits yet")

    unmerged = set(repo.merge_conflicts())
    if unmerged:
        out("\nYou have unmerged paths.")
        out(paint('  (fix conflicts and run "gitprompt commit")', "dim"))
        for path in sorted(unmerged):
            out(paint(f"\tboth modified:   {path}", "red"))

    if state["staged"]:
        out("\nChanges to be committed:")
        out(paint('  (use "gitprompt reset HEAD <file>..." to unstage)', "dim"))
        for path in state["staged"]:
            head_entry = head_tree.get(path)
            entry = index.get(path)
            label = "new file" if head_entry is None else "modified"
            if head_entry is None:
                label = "new file"
            elif entry is None:
                label = "deleted"
            out(paint(f"\t{label}:   {path}", "green"))

    unstaged = sorted((set(state["modified"]) | set(state["deleted"])) - unmerged)
    if unstaged:
        out("\nChanges not staged for commit:")
        out(paint('  (use "gitprompt add <file>..." to update what will be committed)', "dim"))
        for path in unstaged:
            label = "deleted" if path in state["deleted"] else "modified"
            out(paint(f"\t{label}:   {path}", "red"))

    if state["untracked"]:
        out("\nUntracked files:")
        out(paint('  (use "gitprompt add <file>..." to include in what will be committed)', "dim"))
        for path in state["untracked"][:50]:
            out(paint(f"\t{path}", "red"))
        if len(state["untracked"]) > 50:
            out(paint(f"\t... and {len(state['untracked']) - 50} more", "dim"))

    if not any(state.values()):
        out("\nnothing to commit, working tree clean")
    elif unmerged:
        out(paint('\nfix the conflicts, "gitprompt add" them, then '
                  '"gitprompt commit" to conclude the merge', "dim"))
    elif not state["staged"]:
        out(paint('\nno changes added to commit '
                  '(use "gitprompt add" and/or "gitprompt commit -a")', "dim"))

    if args.flag("prompts") and repo.current_session_id():
        out(paint(f"\nActive session: {repo.current_session_id()}", "cyan"))
    return 0


def _tracking_line(repo, branch):
    upstream = repo.config.get(f"branch.{branch}.remote")
    merge = repo.config.get(f"branch.{branch}.merge")
    if not upstream or not merge:
        return ""
    remote_ref = f"refs/remotes/{upstream}/{merge.split('/')[-1]}"
    remote_sha = repo.refs.read(remote_ref)
    local_sha = repo.refs.head_sha()
    if not remote_sha:
        return paint(f"Your branch is not tracking an existing ref ({upstream}).", "dim")
    if remote_sha == local_sha:
        return paint(f"Your branch is up to date with '{upstream}/{merge.split('/')[-1]}'.", "dim")
    ahead = sum(1 for _ in repo.commit_range(local_sha, [remote_sha]))
    behind = sum(1 for _ in repo.commit_range(remote_sha, [local_sha]))
    bits = []
    if ahead:
        bits.append(f"ahead of '{upstream}' by {ahead} commit(s)")
    if behind:
        bits.append(f"behind '{upstream}' by {behind} commit(s)")
    return paint("Your branch is " + " and ".join(bits) + ".", "dim")


def _short_status(repo, state, branch, show_branch):
    lines = []
    if show_branch:
        if branch:
            lines.append(f"## {branch}")
        else:
            sha = repo.refs.head_sha()
            lines.append(f"## HEAD (no branch) {sha[:7] if sha else ''}".rstrip())
    head_tree = repo.head_tree()
    index = repo.read_index()
    for path in state["staged"]:
        if path not in head_tree:
            code = "A"          # new file
        elif index.get(path) is None:
            code = "D"          # in HEAD, already removed from the index
        else:
            code = "M"
        lines.append(f"{code}  {path}")
    # An unresolved path is reported as such, not merely as "modified" —
    # otherwise a conflicted tree looks like an ordinary edit list and the
    # merge is easy to commit without noticing it was never resolved.
    unmerged = set(repo.merge_conflicts())
    for path in sorted(unmerged):
        lines.append(f"UU {path}")
    for path in state["modified"]:
        if path in unmerged:
            continue
        lines.append(f" M {path}")
    for path in state["deleted"]:
        if path in unmerged:
            continue
        lines.append(f" D {path}")
    for path in state["untracked"]:
        lines.append(f"?? {path}")
    for line in lines:
        out(line)
    return 0


# --------------------------------------------------------------------------

def cmd_commit(repo, argv):
    args = Args.parse([
        Opt("-m", "--message", takes_value=True, repeatable=True),
        Opt("-F", "--file", takes_value=True),
        Opt("-a", "--all"),
        Opt("--amend"),
        Opt("--allow-empty"),
        Opt("--author", takes_value=True),
        Opt("-q", "--quiet"),
        Opt("-v", "--verbose"),
        Opt("--session", takes_value=True),
    ], argv)
    pathspecs = resolve_paths(repo, args.positional, allow_all=False)

    index = repo.read_index()
    if args.flag("a", "all") or pathspecs:
        _stage_everything(repo, index, pathspecs)

    merging = repo.merge_head()

    message = "\n\n".join(args.list("m"))
    if args.get("F"):
        if args.get("F") == "-":
            message = sys.stdin.read()
        else:
            with open(args.get("F"), "r", encoding="utf-8") as fh:
                message = fh.read()
    if not message and merging:
        message = repo.merge_message()
    if not message:
        message = _compose_message(repo, index)
    if not message.strip():
        raise GitPromptError("aborting commit due to empty commit message")

    author = _parse_author(args.get("author")) if args.get("author") else None
    parents = None
    if merging:
        pending = repo.merge_conflicts()
        if pending:
            raise GitPromptError(
                "you have unmerged paths\n"
                + "".join(f"\tboth modified: {p}\n" for p in pending)
                + "hint: fix them and 'gitprompt add' each one"
            )
        if args.flag("amend"):
            raise GitPromptError("you are in the middle of a merge -- cannot amend")
        # Both parents, or the merge is recorded as an ordinary commit and the
        # history silently loses the branch that was joined in.
        parents = [repo.refs.head_sha(), merging]
    elif args.flag("amend"):
        head = repo.refs.head_sha()
        if not head:
            raise GitPromptError("you have nothing to amend")
        parents = repo.commit_of(head).parents

    session = args.get("session", repo.current_session_id())
    if session and args.flag("amend"):
        pass
    sha = repo.commit_index(index, message, parents=parents, author=author,
                            allow_empty=args.flag("allow_empty") or bool(merging))
    if merging:
        repo.clear_merge_state()
    if args.flag("amend"):
        repo.refs.append_log("HEAD", repo.refs.head_sha(), sha, "commit (amend)")

    # point the current branch (or HEAD, when detached) at the new commit
    branch_ref = repo.refs.head_ref()
    if branch_ref:
        repo.refs.update(branch_ref, sha, message=f"commit: {message.splitlines()[0][:60]}")
    else:
        repo.refs.detach_head(sha)
    repo.write_index(index)

    if not args.flag("q"):
        branch = repo.refs.current_branch() or "detached HEAD"
        short = sha[:7]
        subject = message.strip().split("\n", 1)[0]
        out(f"[{branch} {short}] {subject}")
        stats = _commit_stats(repo, sha)
        if stats:
            out(stats)
    return 0


def _stage_everything(repo, index, pathspecs=None):
    for relpath in repo.list_worktree_files("."):
        if pathspecs and not matches(relpath, pathspecs):
            continue
        if repo.ignore_stack().is_ignored(relpath):
            continue
        if os.path.isdir(repo._fs_path(relpath)):
            continue
        if relpath in index and index.is_unchanged_on_disk(repo.root, index.get(relpath)):
            continue
        mode, sha, _kind = repo.hash_worktree_file(relpath)
        index.add(relpath, sha, mode, stat_of(repo._fs_path(relpath)))
    for path in list(index.paths()):
        if pathspecs and not matches(path, pathspecs):
            continue
        if not os.path.exists(repo._fs_path(path)):
            index.remove(path)


def _compose_message(repo, index) -> str:
    if not sys.stdin.isatty():
        text = sys.stdin.read()
        if text.strip():
            return text
    raise GitPromptError(
        "no commit message given\n"
        "hint: pass -m \"<message>\", or -F <file>, or pipe a message on stdin"
    )


def _parse_author(raw):
    import re
    m = re.match(r"^(.*?)\s*<([^>]*)>\s*$", raw)
    if m:
        return {"name": m.group(1).strip(), "email": m.group(2).strip()}
    return {"name": raw.strip(), "email": f"{raw.strip()}@localhost"}


def _commit_stats(repo, sha) -> str:
    commit = repo.store.read(sha)
    parent_tree = {}
    if commit.parents:
        parent_tree = repo.read_tree(repo.commit_of(commit.parents[0]).tree)
    my_tree = repo.read_tree(commit.tree)
    from ..diff import diff_trees
    diffs = list(diff_trees(repo, parent_tree, my_tree))
    if not diffs:
        return ""
    added = sum(1 for d in diffs if d.status == "added")
    deleted = sum(1 for d in diffs if d.status == "deleted")
    modified = len(diffs) - added - deleted
    bits = []
    if added:
        bits.append(f"{added} file(s) created")
    if modified:
        bits.append(f"{modified} file(s) changed")
    if deleted:
        bits.append(f"{deleted} file(s) deleted")
    return " " + ", ".join(bits)


# --------------------------------------------------------------------------

def cmd_reset(repo, argv):
    args = Args.parse([
        Opt("--soft"), Opt("--mixed"), Opt("--hard"), Opt("--merge"), Opt("--keep"),
        Opt("-q", "--quiet"),
    ], argv)
    mode = "mixed"
    for flag in ("soft", "mixed", "hard", "merge", "keep"):
        if args.flag(flag):
            mode = flag
    target = repo.resolve(args.positional[0]) if args.positional else "HEAD"
    target = repo.peel_to_commit(target)
    pathspecs = args.positional[1:]

    if pathspecs:
        # `reset <tree-ish> -- <paths>` only touches the index for those paths
        tree = repo.read_tree(repo.commit_of(target).tree)
        index = repo.read_index()
        rel = relpaths(repo, pathspecs)
        for path in sorted(tree):
            if not matches(path, rel):
                continue
            mode_, sha_, _t = tree[path]
            index.add(path, sha_, mode_, stat_of(repo._fs_path(path)))
        repo.write_index(index)
        return 0

    old = repo.refs.head_sha()
    branch_ref = repo.refs.head_ref()
    if branch_ref:
        repo.refs.update(branch_ref, target, old, f"reset: moving to {args.positional[0] if args.positional else 'HEAD'}")
    else:
        repo.refs.detach_head(target)

    if mode == "hard":
        # checkout compares against the index to decide what to delete, so it
        # has to run against the *pre-reset* index; --force then covers any
        # path the reset is deliberately discarding.
        repo.checkout_tree(repo.commit_of(target).tree, force=True, update_index=True)
    elif mode == "mixed":
        target_tree = repo.read_tree(repo.commit_of(target).tree)
        index = Index(repo.index_path)
        for path, (m, sha, _t) in target_tree.items():
            index.add(path, sha, m, stat_of(repo._fs_path(path)))
        repo.write_index(index)

    if mode == "soft":
        return 0
    if mode in ("mixed", "hard") and not args.flag("quiet"):
        out(f"HEAD is now at {target[:7]}")
    return 0
