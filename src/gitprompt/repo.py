"""Repository discovery, initialisation, and worktree/tree/commit plumbing."""

from __future__ import annotations

import os
import shutil

from . import GP_DIR, GP_VERSION
from .config import Config, default_branch, prompt_dir, user_identity
from .ignore import IgnoreStack
from .index import Index, stat_of
from .objects import (
    MODE_BLOB,
    MODE_BLOB_EXEC,
    MODE_PROMPT,
    MODE_TREE,
    TYPE_BLOB,
    TYPE_COMMIT,
    TYPE_PROMPT,
    TYPE_SESSION,
    TYPE_TAG,
    TYPE_TREE,
    Commit,
    ObjectStore,
    Prompt,
    Session,
    hash_object,
)
from .prompt import (
    file_to_prompt,
    parse_frontmatter,
    prompt_filename,
    prompt_to_file,
    slugify,
)
from .refs import HEAD, RefStore, full_branch_name
from .utils import (
    GitPromptError,
    RepoNotFound,
    format_tz,
    join_path,
    normalize_path,
    now_rfc3339,
    now_unix,
    short_id,
    tz_offset_minutes,
)


class Repository:
    """A gitprompt repository.

    `gpdir` is the `.gitprompt` directory.  `root` is the worktree, or None for
    a bare repository.  Everything that touches the object database or the
    worktree hangs off here.
    """

    def __init__(self, gpdir: str, root: str = None):
        self.gpdir = os.path.abspath(gpdir)
        self.root = os.path.abspath(root) if root else None
        self._config = None
        self.store = ObjectStore(self.gpdir, self.object_format)
        self.refs = RefStore(self.gpdir)

    # -- identity ----------------------------------------------------------

    @property
    def is_bare(self) -> bool:
        return self.root is None

    @property
    def object_format(self) -> str:
        return _read_fmt(os.path.join(self.gpdir, "config")) or "sha1"

    @property
    def config(self) -> Config:
        if self._config is None:
            self._config = Config.load(self)
        return self._config

    def reload_config(self) -> Config:
        self._config = None
        return self.config

    @property
    def index_path(self) -> str:
        return os.path.join(self.gpdir, "index")

    def read_index(self) -> Index:
        return Index.read(self.index_path)

    def write_index(self, index: Index) -> None:
        index.write(self.index_path)

    @property
    def prompt_dir(self) -> str:
        return prompt_dir(self.config)

    # -- discovery ---------------------------------------------------------

    @classmethod
    def discover(cls, start: str = None, required: bool = True):
        """Walk up from `start` looking for `.gitprompt`, like git does."""
        cur = os.path.abspath(start or os.getcwd())
        if os.path.isfile(cur):
            cur = os.path.dirname(cur)
        while True:
            candidate = os.path.join(cur, GP_DIR)
            if os.path.isdir(candidate):
                if os.path.isdir(os.path.join(candidate, "objects")):
                    return cls(candidate, cur)
            # a bare repo has objects/ refs/ HEAD directly in the directory
            if (os.path.isdir(os.path.join(cur, "objects"))
                    and os.path.exists(os.path.join(cur, "HEAD"))
                    and os.path.exists(os.path.join(cur, "config"))
                    and _read_fmt(os.path.join(cur, "config")) is not None):
                return cls(cur, None)
            parent = os.path.dirname(cur)
            if parent == cur:
                break
            cur = parent
        if required:
            raise RepoNotFound(start)
        return None

    # -- init --------------------------------------------------------------

    @classmethod
    def init(cls, path: str = ".", bare: bool = False, fmt: str = "sha1",
             initial_branch: str = None, quiet: bool = False) -> "Repository":
        path = os.path.abspath(path or ".")
        if fmt not in ("sha1", "sha256"):
            raise GitPromptError(f"unknown object format '{fmt}' (expected sha1 or sha256)")

        if bare:
            gpdir, root = path, None
        else:
            gpdir, root = os.path.join(path, GP_DIR), path

        if os.path.exists(gpdir):
            raise GitPromptError(f"reinitialized existing gitprompt repository in {gpdir}")

        for sub in ("objects/info", "objects/pack", "refs/heads", "refs/tags",
                    "refs/remotes", "logs", "hooks", "info"):
            os.makedirs(os.path.join(gpdir, *sub.split("/")), exist_ok=True)

        branch = initial_branch or default_branch(Config.load(None))
        with open(os.path.join(gpdir, "HEAD"), "w", encoding="utf-8") as fh:
            fh.write(f"ref: refs/heads/{branch}\n")
        with open(os.path.join(gpdir, "description"), "w", encoding="utf-8") as fh:
            fh.write("Unnamed prompt repository\n")

        cfg_path = os.path.join(gpdir, "config")
        lines = [
            "# gitprompt configuration file",
            "",
            "[core]",
            "\trepositoryformatversion = 0",
            f"\tfilemode = {'false' if os.name == 'nt' else 'true'}",
            "\tbare = " + ("true" if bare else "false"),
            "",
            "[gitprompt]",
            f"\tversion = {GP_VERSION}",
            f"\tpromptdir = prompts",
            "",
        ]
        if fmt != "sha1":
            lines += ["[extensions]", f"\tobjectformat = {fmt}", ""]
        with open(cfg_path, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))

        repo = cls(gpdir, root)
        if not quiet:
            kind = "bare " if bare else ""
            print(f"Initialized empty gitprompt {kind}repository in {gpdir}")
        return repo

    # -- worktree helpers --------------------------------------------------

    def _fs_path(self, relpath: str) -> str:
        if self.is_bare:
            raise GitPromptError("this operation must be run in a work tree")
        return os.path.join(self.root, *normalize_path(relpath).split("/"))

    def is_prompt_path(self, relpath: str) -> bool:
        relpath = normalize_path(relpath)
        prefix = self.prompt_dir.strip("/")
        return (relpath.startswith(prefix + "/") or relpath == prefix) \
            and relpath.lower().endswith((".md", ".markdown", ".txt"))

    def ignore_stack(self) -> IgnoreStack:
        if self.is_bare:
            return IgnoreStack()
        return IgnoreStack.load(self.root)

    def list_worktree_files(self, prefix: str = "."):
        """Every non-ignored file under `prefix`, repo-relative."""
        if self.is_bare:
            return
        ignores = self.ignore_stack()
        base = self._fs_path(prefix) if prefix not in (".", "") else self.root
        if os.path.isfile(base):
            rel = os.path.relpath(base, self.root).replace("\\", "/")
            if not ignores.is_ignored(rel):
                yield rel
            return
        for dirpath, dirnames, filenames in os.walk(base):
            rel_dir = os.path.relpath(dirpath, self.root).replace("\\", "/")
            rel_dir = "" if rel_dir == "." else rel_dir
            dirnames[:] = [
                d for d in dirnames
                if not ignores.is_ignored(join_path(rel_dir, d), is_dir=True)
            ]
            for name in filenames:
                rel = join_path(rel_dir, name)
                if rel and not ignores.is_ignored(rel):
                    yield rel

    # -- object creation from the worktree ---------------------------------

    def hash_worktree_file(self, relpath: str) -> tuple:
        """Create the object for one worktree file.

        Files under the prompt directory become `prompt` objects carrying
        their provenance; everything else becomes a plain blob.  Returns
        (mode, sha, kind).
        """
        relpath = normalize_path(relpath)
        fs_path = self._fs_path(relpath)
        if os.path.islink(fs_path):
            target = os.readlink(fs_path).encode("utf-8")
            return 0o120000, self.store.write_raw(TYPE_BLOB, target), "blob"
        with open(fs_path, "rb") as fh:
            raw = fh.read()
        mode = MODE_BLOB
        if os.name != "nt" and os.access(fs_path, os.X_OK):
            mode = MODE_BLOB_EXEC
        if self.is_prompt_path(relpath):
            try:
                text = raw.decode("utf-8")
            except UnicodeDecodeError:
                raise GitPromptError(f"{relpath}: prompt files must be UTF-8")
            name, email = _identity_or_placeholder(self.config)
            prompt = file_to_prompt(
                text, relpath,
                fallback_author={"name": name, "email": email},
                fallback_session=self.current_session_id(),
            )
            return MODE_PROMPT, self.store.write(TYPE_PROMPT, prompt), "prompt"
        return mode, self.store.write_raw(TYPE_BLOB, raw), "blob"

    def next_prompt_seq(self, session_id: str = None) -> int:
        """One past the highest seq already used in a session.

        Counts both committed prompts and prompt files still sitting in the
        worktree, because a session is usually built up over several prompts
        before anything is committed — and if only committed prompts counted,
        every prompt in an uncommitted session would be numbered 1.
        """
        session_id = session_id or self.current_session_id()
        highest = 0
        for prompt in self.iter_prompts(session=session_id):
            highest = max(highest, prompt.seq or 0)
        for relpath in self.list_worktree_files(self.prompt_dir):
            try:
                with open(self._fs_path(relpath), "r", encoding="utf-8") as fh:
                    text = fh.read()
            except OSError:
                continue
            try:
                meta, _body = parse_frontmatter(text)
            except GitPromptError:
                continue
            if session_id and meta.get("session") not in (None, session_id):
                continue
            seq = str(meta.get("seq") or "")
            if seq.isdigit():
                highest = max(highest, int(seq))
        return highest + 1

    def next_file_seq(self) -> int:
        """One past the highest numeric prefix used in the prompt directory.

        This number is repository-wide, not per-session, because prompts from
        every session share one flat directory: per-session numbering would
        restart at 1 in each sitting and hand a plain directory listing a
        reading order that does not match the order the prompts were written.
        `Prompt.seq` stays per-session — that is the number that means
        something *inside* a session.

        Both the worktree and the current commit are consulted, so a prompt
        whose file was deleted still reserves its number.
        """
        candidates = set(self.list_worktree_files(self.prompt_dir))
        try:
            candidates |= {p for p in self.head_tree() if self.is_prompt_path(p)}
        except GitPromptError:
            pass
        highest = 0
        for relpath in candidates:
            head = os.path.basename(relpath).split("-", 1)[0]
            if head.isdigit():
                highest = max(highest, int(head))
        return highest + 1

    def write_prompt(self, text: str, session_id: str = None, slug: str = None,
                     author=None, model=None, tags=None, write_file: bool = True) -> tuple:
        """Persist one prompt: object first, then the matching worktree file.

        Returns (prompt, sha, relpath).
        """
        session_id = session_id or self.current_session_id() or self.new_session()
        seq = self.next_prompt_seq(session_id)
        name, email = _identity_or_placeholder(self.config)
        prompt = Prompt(
            body=text if text.endswith("\n") else text + "\n",
            id=f"p_{short_id(8)}",
            session=session_id,
            seq=seq,
            timestamp=now_rfc3339(),
            author=author or {"name": name, "email": email},
            model=model or self.config.get("gitprompt.model"),
            tags=list(tags or []),
        )
        sha = self.store.write(TYPE_PROMPT, prompt)
        relpath = join_path(self.prompt_dir,
                            prompt_filename(self.next_file_seq(), slug or slugify(text)))
        if write_file:
            fs_path = self._fs_path(relpath)
            if os.path.exists(fs_path):
                raise GitPromptError(f"{relpath} already exists")
            os.makedirs(os.path.dirname(fs_path), exist_ok=True)
            with open(fs_path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(prompt_to_file(prompt))
        return prompt, sha, relpath

    # -- tree construction -------------------------------------------------

    def write_tree_from_index(self, index: Index) -> str:
        """Fold a flat path→entry index into nested tree objects.

        Works bottom-up: group entries by their first path component, recurse
        into directories, and write each tree once its children exist.
        """
        return self._write_tree_level(_nest(index))

    def _write_tree_level(self, node: dict) -> str:
        entries = []
        for name, value in node.items():
            if isinstance(value, dict):
                sub = self._write_tree_level(value)
                entries.append((MODE_TREE, name, sub))
            else:
                entries.append((value.mode, name, value.sha))
        return self.store.write(TYPE_TREE, entries)

    def write_tree_from_paths(self, paths) -> str:
        """Build a tree from (relpath, mode, sha) triples without an index."""
        node = {}
        for relpath, mode, sha in paths:
            parts = normalize_path(relpath).split("/")
            cur = node
            for part in parts[:-1]:
                cur = cur.setdefault(part, {})
            from .index import IndexEntry
            cur[parts[-1]] = IndexEntry(normalize_path(relpath), sha, mode)
        return self._write_tree_level(node)

    # -- tree reading ------------------------------------------------------

    def read_tree(self, tree_sha: str, prefix: str = "") -> dict:
        """Flatten a tree into {relpath: (mode, sha, objtype)}."""
        out = {}
        self._walk_tree(tree_sha, prefix, out)
        return out

    def _walk_tree(self, tree_sha: str, prefix: str, out: dict) -> None:
        if not tree_sha:
            return
        for mode, name, sha in self.store.read(tree_sha):
            path = join_path(prefix, name)
            if mode == MODE_TREE:
                self._walk_tree(sha, path, out)
            else:
                objtype = TYPE_PROMPT if mode == MODE_PROMPT else TYPE_BLOB
                out[path] = (mode, sha, objtype)

    def head_tree(self):
        sha = self.refs.head_sha()
        if not sha:
            return {}
        return self.read_tree(self.commit_of(sha).tree)

    # -- in-progress merge -------------------------------------------------
    #
    # A conflicted merge has to outlive the process that started it: the user
    # resolves the files by hand and commits afterwards, possibly in a later
    # invocation.  The state lives in files named after git's own (MERGE_HEAD,
    # MERGE_MSG) so the parallel is obvious.

    def _merge_path(self, name: str) -> str:
        return os.path.join(self.gpdir, name)

    def merge_head(self):
        """The other parent of an in-progress merge, or None."""
        return self._read_merge_file("MERGE_HEAD") or None

    def merge_message(self) -> str:
        return self._read_merge_file("MERGE_MSG") or ""

    def merge_conflicts(self):
        """Paths still unresolved.  Empty when there is no merge in progress."""
        raw = self._read_merge_file("MERGE_CONFLICTS")
        return [line for line in (raw or "").splitlines() if line.strip()]

    def _read_merge_file(self, name: str):
        path = self._merge_path(name)
        if not os.path.exists(path):
            return ""
        with open(path, "r", encoding="utf-8") as fh:
            return fh.read().strip()

    def set_merge_state(self, other_sha: str, message: str, conflicts) -> None:
        self._write_merge_file("MERGE_HEAD", other_sha)
        self._write_merge_file("MERGE_MSG", message)
        self._write_merge_file("MERGE_CONFLICTS", "\n".join(sorted(conflicts)))

    def resolve_merge_path(self, relpath: str) -> bool:
        """Mark one conflicted path resolved.  Returns whether it was one."""
        remaining = self.merge_conflicts()
        if relpath not in remaining:
            return False
        remaining = [p for p in remaining if p != relpath]
        self._write_merge_file("MERGE_CONFLICTS", "\n".join(remaining))
        return True

    def clear_merge_state(self) -> None:
        for name in ("MERGE_HEAD", "MERGE_MSG", "MERGE_CONFLICTS"):
            path = self._merge_path(name)
            if os.path.exists(path):
                os.remove(path)

    def _write_merge_file(self, name: str, text: str) -> None:
        with open(self._merge_path(name), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text + ("\n" if text else ""))

    def commit_of(self, sha_or_ref: str) -> Commit:
        return self.store.read(self.resolve(sha_or_ref))

    def peel_to_commit(self, sha_or_ref: str) -> str:
        """Follow a name — ref, tag, or abbreviated sha — down to a commit."""
        return self.peel_sha(self.resolve(sha_or_ref), sha_or_ref)

    def peel_sha(self, sha: str, original: str = None) -> str:
        """Peel an already-resolved sha down to a commit.

        Separate from `peel_to_commit` because revision parsing must not run
        twice: resolving an abbreviated sha yields a sha, and feeding that
        back through `resolve` would re-enter the parser that produced it.
        """
        label = original or sha
        for _ in range(10):
            objtype = self.store.type_of(sha)
            if objtype == TYPE_COMMIT:
                return sha
            if objtype == TYPE_TAG:
                target = self.store.read(sha)
                sha = getattr(target, "object", None)
                if not sha:
                    raise GitPromptError(f"{label} does not point to a commit")
                continue
            raise GitPromptError(f"{label} does not point to a commit")
        raise GitPromptError(f"{label}: too many levels of tags")

    def resolve(self, name: str) -> str:
        """Turn a ref name, `HEAD`, `HEAD~2`, or abbreviated sha into a full sha."""
        from .revision import resolve_revision
        return resolve_revision(self, name)

    # -- commits -----------------------------------------------------------

    def create_commit(self, tree_sha: str, parents, message: str,
                      author=None, session: str = None, session_seq=None,
                      model: str = None, extra=None) -> str:
        name, email = _identity_or_placeholder(self.config)
        ident = f"{name} <{email}> {now_unix()} {format_tz(tz_offset_minutes())}"
        commit = Commit(
            tree=tree_sha,
            parents=[p for p in (parents or []) if p],
            author=f"{_fmt_ident(author)} {now_unix()} {format_tz(tz_offset_minutes())}"
                   if author else ident,
            committer=ident,
            session=session if session is not None else self.current_session_id(),
            session_seq=session_seq,
            model=model or self.config.get("gitprompt.model"),
            extra=extra,
            message=message if message.endswith("\n") else message + "\n",
        )
        return self.store.write(TYPE_COMMIT, commit)

    def commit_index(self, index: Index, message: str, parents=None,
                     author=None, allow_empty: bool = False) -> str:
        tree_sha = self.write_tree_from_index(index)
        parents = list(parents) if parents is not None else ([self.refs.head_sha()] if self.refs.head_sha() else [])
        if not allow_empty and parents:
            parent_tree = self.commit_of(parents[0]).tree
            if parent_tree == tree_sha:
                raise GitPromptError(
                    "nothing to commit, working tree clean\n"
                    "(use --allow-empty to record a commit with no changes)"
                )
        session = self.current_session_id()
        seq = None
        if session:
            seq = self.next_session_commit_seq(session)
        return self.create_commit(tree_sha, parents, message, author=author,
                                  session=session, session_seq=seq)

    def next_session_commit_seq(self, session_id: str) -> int:
        highest = 0
        for _sha, commit in self.iter_commits(self.refs.head_sha(), limit=1000):
            if commit.session == session_id and commit.session_seq:
                highest = max(highest, commit.session_seq)
        return highest + 1

    # -- history walking ---------------------------------------------------

    def iter_commits(self, start: str = None, limit: int = None, first_parent: bool = False):
        """Yield (sha, Commit) newest-first, breadth-first over the DAG.

        A work queue rather than recursion, so a long linear history cannot
        blow the Python stack.
        """
        start = start or self.refs.head_sha()
        if not start:
            return
        try:
            start = self.peel_to_commit(start)
        except GitPromptError:
            # a ref that does not name a commit (a session ref, say) has no
            # history to walk; skipping it is correct, not an error
            return
        seen = set()
        queue = [start]
        emitted = 0
        while queue:
            sha = queue.pop(0)
            if sha in seen:
                continue
            seen.add(sha)
            try:
                commit = self.store.read(sha)
            except GitPromptError:
                continue
            if not isinstance(commit, Commit):
                continue
            yield sha, commit
            emitted += 1
            if limit and emitted >= limit:
                return
            parents = commit.parents[:1] if first_parent else commit.parents
            queue.extend(p for p in parents if p not in seen)

    def commit_range(self, include: str = None, exclude=None):
        """Commits reachable from `include` but not from any ref in `exclude`."""
        exclude = set(exclude or [])
        hidden = set()
        for ref in exclude:
            for sha, _ in self.iter_commits(ref):
                hidden.add(sha)
        for sha, commit in self.iter_commits(include):
            if sha not in hidden:
                yield sha, commit

    def is_ancestor(self, maybe_ancestor: str, descendant: str) -> bool:
        a = self.peel_to_commit(maybe_ancestor)
        for sha, _ in self.iter_commits(descendant):
            if sha == a:
                return True
        return False

    def merge_base(self, a: str, b: str):
        """Best common ancestor, or None for unrelated histories."""
        ancestors_a = {}
        for i, (sha, _) in enumerate(self.iter_commits(a)):
            ancestors_a[sha] = i
        for i, (sha, _) in enumerate(self.iter_commits(b)):
            if sha in ancestors_a:
                return sha
        return None

    # -- prompts / sessions ------------------------------------------------

    def iter_prompts(self, ref: str = None, session: str = None,
                     all_history: bool = True, include_worktree: bool = True):
        """Every prompt object in the history, chronological, de-duplicated.

        Scans all reachable trees rather than just HEAD so that prompts on
        abandoned branches still show up in a timeline — branch pruning is a
        separate, explicit decision.

        Uncommitted prompt files are folded in too, keyed by prompt id so a
        prompt whose file gained an outcome since the last commit is not
        counted twice.  Without this a session reads as empty until the first
        commit, which is exactly when a user is most likely to look at it.
        """
        seen_objs = set()
        seen_shas = set()
        seen_ids = set()
        prompts = []
        refs_to_scan = [ref] if ref else self._all_history_refs()
        for r in refs_to_scan:
            for _sha, commit in self.iter_commits(r):
                if commit.tree in seen_objs:
                    continue
                seen_objs.add(commit.tree)
                for path, (mode, sha, objtype) in self.read_tree(commit.tree).items():
                    if objtype != TYPE_PROMPT or sha in seen_shas:
                        continue
                    seen_shas.add(sha)
                    try:
                        prompt = self.store.read(sha)
                    except GitPromptError:
                        continue
                    prompt.sha = sha
                    prompt.meta.setdefault("path", path)
                    seen_ids.add(prompt.id or sha)
                    if session and prompt.session != session:
                        continue
                    prompts.append(prompt)

        if include_worktree:
            for relpath in self.list_worktree_files(self.prompt_dir):
                if not self.is_prompt_path(relpath):
                    continue
                prompt = self.read_worktree_prompt(relpath)
                if prompt is None or (prompt.id or relpath) in seen_ids:
                    continue
                seen_ids.add(prompt.id or relpath)
                if session and prompt.session != session:
                    continue
                prompts.append(prompt)

        prompts.sort(key=lambda p: p.sort_key())
        return prompts

    def read_worktree_prompt(self, relpath: str):
        """Parse a prompt straight out of the working tree, or None.

        A read path must not write, so the sha is computed rather than
        stored: an unreferenced object left behind by a listing command would
        be garbage the moment it was created.
        """
        try:
            with open(self._fs_path(relpath), "r", encoding="utf-8") as fh:
                text = fh.read()
        except (OSError, UnicodeDecodeError):
            return None
        name, email = _identity_or_placeholder(self.config)
        try:
            prompt = file_to_prompt(
                text, relpath,
                fallback_author={"name": name, "email": email},
                fallback_session=self.current_session_id(),
            )
        except GitPromptError:
            return None
        # Hash first: `path` is provenance for this walk, not part of the
        # object, and folding it in would give the same prompt a different
        # sha here than it has in the store.
        prompt.sha = hash_object(TYPE_PROMPT, prompt.encode())
        prompt.meta.setdefault("path", relpath)
        return prompt

    def _all_history_refs(self):
        refs = list(self.refs.list("refs/").values())
        head = self.refs.head_sha()
        if head:
            refs.append(head)
        if not refs:
            return []
        return refs

    # -- sessions ----------------------------------------------------------

    @property
    def session_path(self) -> str:
        return os.path.join(self.gpdir, "SESSION")

    def current_session_id(self):
        """The session new prompts are appended to.

        Stored in `.gitprompt/SESSION`, the same way HEAD names the current
        branch — so a session survives across shells and across the restarts
        that separate one work sitting from the next.
        """
        if os.path.exists(self.session_path):
            with open(self.session_path, "r", encoding="utf-8") as fh:
                value = fh.read().strip()
            if value.startswith("ref:"):
                return None
            return value or None
        return None

    def set_current_session(self, session_id) -> None:
        os.makedirs(os.path.dirname(self.session_path), exist_ok=True)
        with open(self.session_path, "w", encoding="utf-8") as fh:
            fh.write((session_id or "") + "\n")

    def new_session(self, title: str = "", notes: str = None, model: str = None) -> str:
        session_id = f"s_{now_unix()}_{short_id(6)}"
        name, email = _identity_or_placeholder(self.config)
        session = Session(
            id=session_id,
            title=title or "untitled session",
            started_at=now_rfc3339(),
            author={"name": name, "email": email},
            model=model or self.config.get("gitprompt.model"),
            notes=notes,
        )
        sha = self.store.write(TYPE_SESSION, session)
        self.write_session_ref(session_id, sha)
        self.set_current_session(session_id)
        return session_id

    def write_session_ref(self, session_id: str, sha: str) -> None:
        """Sessions are refs too, so they travel with push/fetch for free."""
        self.refs.update(f"refs/sessions/{session_id}", sha,
                         message=f"session {session_id}")

    def session_sha(self, session_id: str):
        return self.refs.read(f"refs/sessions/{session_id}")

    def load_session(self, session_id: str) -> Session:
        sha = self.session_sha(session_id)
        if not sha:
            raise GitPromptError(f"no such session: {session_id}")
        return self.store.read(sha)

    def save_session(self, session: Session) -> str:
        """Persist a session and point its ref at the new object."""
        sha = self.store.write(TYPE_SESSION, session)
        self.write_session_ref(session.id, sha)
        return sha

    def list_sessions(self):
        out = []
        for refname, sha in self.refs.list("refs/sessions/").items():
            try:
                out.append(self.store.read(sha))
            except GitPromptError:
                continue
        out.sort(key=lambda s: s.started_at or "")
        return out

    def end_session(self, session_id: str = None, notes: str = None):
        session_id = session_id or self.current_session_id()
        if not session_id:
            raise GitPromptError("no session is currently active")
        session = self.load_session(session_id)
        session.ended_at = now_rfc3339()
        if notes:
            session.notes = notes
        session.prompts = [p.sha for p in self.iter_prompts(session=session_id)]
        self.save_session(session)
        return session

    # -- worktree sync -----------------------------------------------------

    def checkout_tree(self, tree_sha: str, force: bool = False,
                      update_index: bool = True, prefix: str = "") -> dict:
        """Materialise a tree into the worktree, git-checkout style.

        Refuses to clobber local modifications unless `force` is set — the
        working tree is where someone's uncommitted prompts live, and silently
        overwriting them would lose real work.
        """
        if self.is_bare:
            raise GitPromptError("this operation must be run in a work tree")
        target = self.read_tree(tree_sha)
        index = self.read_index() if update_index else None
        current = {p: (e.mode, e.sha) for p, e in index.entries.items()} if index else self.head_tree()
        stats = {"written": 0, "removed": 0, "kept": 0}

        # delete files tracked in the old tree but absent from the new one
        for relpath in set(current) - set(target):
            if prefix and not relpath.startswith(prefix):
                continue
            fs_path = self._fs_path(relpath)
            if os.path.exists(fs_path):
                if not force and self._is_modified(relpath, current[relpath]):
                    raise GitPromptError(
                        f"local changes to '{relpath}' would be overwritten by checkout\n"
                        "commit them, or use --force to discard"
                    )
                os.remove(fs_path)
                stats["removed"] += 1
            if index is not None:
                index.remove(relpath)

        for relpath, (mode, sha, objtype) in target.items():
            if prefix and not relpath.startswith(prefix):
                continue
            fs_path = self._fs_path(relpath)
            content = self._object_bytes(sha, objtype)
            unchanged = False
            if os.path.exists(fs_path):
                with open(fs_path, "rb") as fh:
                    unchanged = fh.read() == content
            if unchanged:
                stats["kept"] += 1
            else:
                if os.path.exists(fs_path) and not force \
                        and self._is_modified(relpath, current.get(relpath)):
                    raise GitPromptError(
                        f"local changes to '{relpath}' would be overwritten by checkout\n"
                        "commit them, or use --force to discard"
                    )
                os.makedirs(os.path.dirname(fs_path), exist_ok=True)
                with open(fs_path, "wb") as fh:
                    fh.write(content)
                stats["written"] += 1
            if index is not None:
                index.add(relpath, sha, mode, stat_of(fs_path))
        if index is not None:
            self.write_index(index)
        return stats

    def _object_bytes(self, sha: str, objtype: str) -> bytes:
        obj = self.store.read(sha)
        if objtype == TYPE_PROMPT:
            return prompt_to_file(obj).encode("utf-8")
        return obj

    def _is_modified(self, relpath: str, current_entry) -> bool:
        fs_path = self._fs_path(relpath)
        if not os.path.exists(fs_path):
            return True
        mode, sha = current_entry
        _m, new_sha, _kind = self.hash_worktree_file(relpath)
        return new_sha != sha

    def reset_index_to_tree(self, tree_sha: str) -> Index:
        index = Index(self.index_path)
        for relpath, (mode, sha, _objtype) in self.read_tree(tree_sha).items():
            index.add(relpath, sha, mode, stat_of(self._fs_path(relpath)))
        return index


def _nest(index: Index) -> dict:
    """Flat paths → nested dict of dicts, leaves being IndexEntry."""
    node: dict = {}
    for entry in index:
        parts = entry.path.split("/")
        cur = node
        for part in parts[:-1]:
            cur = cur.setdefault(part, {})
            if not isinstance(cur, dict):
                raise GitPromptError(
                    f"path conflict: '{entry.path}' is both a file and a directory"
                )
        if isinstance(cur.get(parts[-1]), dict):
            raise GitPromptError(
                f"path conflict: '{entry.path}' is both a file and a directory"
            )
        cur[parts[-1]] = entry
    return node


def _fmt_ident(author) -> str:
    name, email = _identity_or_placeholder(None, author)
    return f"{name} <{email}>"


def _identity_or_placeholder(cfg, author=None) -> tuple:
    """Best-effort identity.

    Prompts and commits always want *some* attribution, so a missing config
    falls back to the OS user rather than failing the way `git commit` does —
    except when an explicit author was supplied, which always wins.
    """
    if isinstance(author, dict) and author.get("name"):
        return author.get("name"), author.get("email") or \
            f"{author.get('name', 'unknown')}@localhost"
    if cfg is not None:
        name, email = cfg.get("user.name"), cfg.get("user.email")
        if name and email:
            return name, email
    user = os.environ.get("USERNAME") or os.environ.get("USER") or "unknown"
    return user, f"{user}@localhost"


def _read_fmt(config_path: str):
    """Return the object format declared in a config file, or None."""
    try:
        with open(config_path, "r", encoding="utf-8") as fh:
            text = fh.read()
    except OSError:
        return None
    if "[extensions]" in text:
        section = text.split("[extensions]", 1)[1]
        section = section.split("[", 1)[0]
        for line in section.splitlines():
            key, _, value = line.partition("=")
            if key.strip().lower() == "objectformat":
                return value.strip() or "sha1"
    return "sha1" if "[core]" in text else None
