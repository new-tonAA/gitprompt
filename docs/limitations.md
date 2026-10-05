# What is not implemented

Stated plainly, because a tool that quietly does the wrong thing is worse than
one that says no. These are the gaps worth knowing before you rely on the tool;
the smaller divergences inside a command that does exist are in
[Notes](notes.md). The command list itself, and which of git's names have no
counterpart here, is in [The commands](commands.md).

- **Byte-for-byte `diff` output.** A move is reported as a move, and the changes
  are the changes git reports, but the text around them is not git's. A hunk that
  empties one side is notated `-1,0` where git writes `-0,0`; and `--stat`
  neither widens its path column to fit the longest path in the block nor scales
  its bar to the terminal, so a change of a thousand lines draws a thousand
  marks. A reader skimming a diff sees the same changes; a script that parses one
  should be pointed at git instead. The `similarity index` a rename reports is the
  share of lines the two files still have in common rather than git's byte
  estimate, so the number can read differently from git's even where the
  judgement behind it does not.
- **What an answer is.** `response` and `rerun --record` keep what the agent
  wrote to standard output, verbatim — its own formatting, progress lines and
  all, or a JSON envelope if that is what it was asked for. Nothing else about
  the run is kept: not its standard error, not the files it changed, not what it
  cost. And an agent nobody asked to record is not recorded, so a history is as
  complete as whoever kept it.
- **Which agents can be replayed into.** `claude` is the only agent this tree
  replays into as conversations. `codex` and `dsh` cannot be given a name for a
  new conversation, so `rerun --agent=codex` and `--agent=dsh` are refused
  rather than run as a string of unrelated sessions. An agent the table has never
  heard of is usable once `gitprompt.agent.<name>.command` says how this machine
  runs it, and every other piece of how one is driven is settable the same way;
  the whole of it is in
  [Notes](notes.md#how-this-machine-runs-an-agent).
- **Platforms.** Developed and built on Windows with TDM-GCC. The code is
  plain C99: what is Windows-specific is a small `#ifdef _WIN32` block for
  `_getcwd`/`_getpid`, `__USE_MINGW_ANSI_STDIO`, and putting the streams in
  binary mode so that a newline written out is a newline and not a carriage
  return before it, and the sockets in `net.c`, which winsock provides on
  Windows and libc on Unix. CI builds and runs the suite on Linux and macOS as
  well as Windows, which is the only place the Unix builds are exercised: the
  development machine has one compiler for one of the three.
- **The commands git has that gitprompt does not.** git 2.49 lists 176; a
  gitprompt built from this tree lists 74. Missing is the credential helper
  protocol. The object model, the index, committing, history, branches,
  merging including conflicts, replaying a commit elsewhere, undoing one,
  reusing a conflict's resolution, setting work aside, tags, reset, the ref
  plumbing, remotes, the prompt layer, a second working directory with
  `worktree`, a repository nested in another with `submodule`, a work tree that
  holds only part of the index with `sparse-checkout`, and the object-store
  maintenance `archive`, `notes`, `gc`, `repack` and `prune` are all here.
- **`archive`, at its edges.** The container holds the files of the tree and
  nothing else: no directory entries are written, so an empty directory is not
  in the archive, and the entries carry the commit's timestamp rather than a
  per-file one. `--remote`, the gitattributes filters (`export-ignore`,
  `export-subst`) and `--add-file` are refused as unknown options. The tar is
  ustar, so a path longer than the 100-byte name field is refused rather than
  written as the pax extension git writes, and the zip entries are stored rather
  than deflated, which is why a zip here is larger than the one `git archive`
  writes but is read by `unzip` exactly. The reasoning is in
  [Notes](notes.md#handing-a-tree-out).
- **`notes`, at its edges.** The ref is `refs/notes/commits` and nothing else:
  `--ref`, `-c`/`-C` and `--allow-empty` are refused as unknown options, and
  there is no merging of two notes trees, so a `merge` of the notes ref is not
  something this can carry. A note is written with the object's name in full at
  the top level of the tree, which is where git puts it for any ordinary
  repository; a tree git has fanned out into a directory per two hex digits is
  read, but the fanout is not reproduced on the way back. `notes add` without
  `-m` or `-F` does not open an editor as git's does -- the message has to be
  given -- while `notes edit` is there for the interactive case. The reasoning
  is in [Notes](notes.md#a-note-is-a-blob-with-the-objects-name).
- **`replace`, at its edges.** A replace ref is read at the object store, so
  every reader here follows it and the store's own maintenance deliberately does
  not -- `fsck`, `gc`, `repack` and `prune` walk the history as it literally is,
  which is what git's do, so a commit reachable only through a replaced parent
  link is neither reported dangling nor thrown away. Two of git's liberties are
  not taken: an object cannot be made to stand in for itself, and two objects
  cannot stand in for each other, because either builds a chain the read has to
  give up on part-way; and a stand-in of another type is refused whether or not
  `-f` is given, where git checks only when it is about to create the ref. The
  pattern `-l` takes is a glob over the whole 40-hex name and not a prefix, so
  a seven-character abbreviation matches nothing. A replace ref is read and
  written as a loose file: one that `pack-refs` has folded into `packed-refs` is
  listed here but not followed on a read, and `-d` does not remove it. `-e`
  edits what would be read
  -- the stand-in when there is one, the object itself otherwise -- and writes
  the ref, where git refuses with `already exists`; `--graft` and
  `--convert-graft-file` are refused as unknown options. Reads written here are
  seen by git and the other way round, and the environment variable
  `GIT_NO_REPLACE_OBJECTS` turns the whole mechanism off for either. The
  reasoning is in [Notes](notes.md#reading-one-object-in-place-of-another).
- **`apply`, at its edges.** It reads git's unified diff and writes through
  whichever layer is asked for -- the work tree by default, the index with
  `--cached`, both with `--index` -- but only that diff: a `GIT binary patch`,
  an `@@@` combined diff and a `|||||||` diff3 conflict are refused rather than
  guessed at, and a hunk is matched by its exact preimage with no fuzz, so a
  patch that would need one fails with git's two error lines. There is no
  `-p<n>`, `-R`, `--3way`, `--reject`, `--whitespace`, `--directory`,
  `--include`/`--exclude`, `--stat` or `--binary`; each is refused as an
  unknown option. Paths are read against the root of the work tree rather than
  against the directory it is run from, so a patch naming `f.txt` reaches
  `<root>/f.txt` from anywhere, where git under a subdirectory would refuse it
  as outside the prefix. A path that would leave the work tree is refused, and
  `--unsafe-paths` does not let it out -- the path is normalised against the
  root, so this never writes above the work tree, where git would. The
  reasoning is in [Notes](notes.md#applying-a-patch).
- **`range-diff`, at its edges.** The comparison is by a patch id of this
  command's own making -- the paths, the modes and the changed lines, with the
  hunk positions and the blob names left out -- and not by git's, so the two are
  never compared against each other, and the body printed under a `!` is this
  command's diff of the pair rather than git's rendering of the two commits.
  The pairing is a rule of its own as well: a left pairs with the next right
  whose patch has at least half its changed lines in common, where git weighs a
  cost against a creation factor, so a pair near that line can be marked `!`
  here and `<`/`>` there. `--creation-factor`, `--notes`, `--diff-merges`,
  `--remerge-diff` and the diff options are refused as unknown options, and
  `--no-dual-color` is accepted and does nothing, there being no colour here to
  turn off. The reasoning is in
  [Notes](notes.md#two-versions-of-the-same-series).
- **`worktree`, at its edges.** A linked working directory is a directory
  holding a `.git` file that names its registration under the main store's
  `worktrees/`; the objects, the refs and the configuration are the
  repository's, and the HEAD, the index and whatever merge or rebase is under
  way belong to the directory. That is why a commit made in one is at once
  visible from the other, and why an unfinished merge in one is not a merge in
  the other. One branch still has one HEAD, so a branch another directory holds
  is refused by `checkout` as well as by `worktree add`. `add` takes `-b`,
  `--detach` and `-f`, and an option outside that set -- `--track`, `--orphan`,
  `list --porcelain` -- is refused rather than ignored; `remove` and `move`
  refuse a locked directory unless `-f` is given; `prune` drops only the
  registrations whose directory is gone, leaves a locked one alone, and `-n`
  reports without dropping, with `--expire` refused as an unknown option. A
  bare repository is not a form this reads, so the questions git answers about
  one with `core.bare` and `--relative-paths` do not come up. The reasoning is
  in [Notes](notes.md#a-second-working-directory).
- **`submodule`, at its edges.** A submodule is the three things git makes it:
  a gitlink in the parent's tree, an entry in `.gitmodules` naming where that
  commit comes from, and a repository of its own whose store lives under the
  parent's `modules/`, keyed by the submodule's path, with the path holding a
  `.git` file that names it. `add`, `init`, `update` (`--init`, `--remote`,
  `-f`), `status`, `sync`, `deinit` and `foreach` are here. `update` will not
  check a submodule out over uncommitted changes to its tracked files, which
  `-f` overrides; `deinit` refuses for the same reason, counting an untracked
  file as well, until forced. So a submodule is never quietly emptied. What is
  missing is `--recursive` (nothing
  descends into a submodule's own submodules), `absorbgitdirs`, `summary`, the
  merge of a `.gitmodules` both sides changed, and `--depth`/`--jobs`. A remote
  is whatever `.gitmodules` names: an `https` or `ssh` url goes through git, and
  a local path is read directly, which means it has to be a gitprompt store --
  a plain git repository at a local path is not a form a local fetch reads, the
  same way `remote` will not take one. The reasoning is in
  [Notes](notes.md#a-repository-inside-a-repository).
- **`sparse-checkout`, at its edges.** The index is not thinned: every path is
  still in it, and what says a path is not in this work tree is the
  skip-worktree bit, which is why `ls-files -t` prints `S` for one. That bit
  only has a place to live in version 3 of the index format, so a repository
  that has any sparse path writes version 3 -- which git reads, the two
  directions of it are in the surface suite. The patterns are git's, in git's
  `info/sparse-checkout` and in git's syntax, with the one difference the file
  itself carries: they are read from the root, so a pattern with no leading
  slash still means the top of the tree. The last line that matches decides and
  a path no line matches is out, so an empty pattern file is an empty work tree;
  `init` writes the two lines git's own `init` writes, which keeps the files at
  the top and cuts the directories. Only the pattern list is here -- `--cone`,
  `--no-cone`, `--stdin`, `-z` and the sparse-index files are refused as unknown
  options, so the directory expansion cone mode does has to be written out by
  hand, and `reapply` is not here. What is also not here is the rest of the
  tree learning about it: a `checkout` writes no marked path and marks what it
  read into the index, `status` does not call a missing marked path a deletion,
  `commit -a` leaves its entry alone and `ls-files -t` reports it, but a sparse
  index, `add --sparse` and the `--sparse` option on commands that take a
  pathspec are not implemented. The reasoning is in
  [Notes](notes.md#only-part-of-the-index-in-the-work-tree).
- **The ignore file, at its edges.** What `status`, `add` and `clean` read is
  git's rules -- globs, anchoring, `**`, `!`, a file per directory, and a path
  in the index is never ignored -- and both `.gitignore` and
  `.gitpromptignore` are read, the latter with the last word inside a
  directory. What is missing is the edges: `[[:alpha:]]` character classes,
  `core.excludesFile`, `.gitprompt/info/exclude`, `status --ignored` and
  `clean -i`. `git add`'s third hint line is not printed either, since there is
  no `advice.addIgnoredFile` here to turn the message off. What the rules mean,
  rule by rule, is in [Notes](notes.md#what-the-ignore-file-means).
- **`clean`, at its edges.** `-i`/`--interactive` is refused as an unknown
  option, a directory that is a repository in its own right is left alone
  however many `-f` are given, a pathspec is the path of the directory it
  names rather than a wildmatch pattern, and an `-e` pattern is a whole
  basename or a whole path rather than a wildmatch pattern, so
  `-e '*.log'` names a file by that literal name and nothing else.
- **`grep`'s pattern engine and its option set.** The engine is gitprompt's own,
  so interval expressions `{n,m}`, back references, the POSIX classes, the word
  boundaries and a repetition that can match nothing are refused at compile time
  and exit `128` rather than matching; `-A`/`-B`/`-C`, `-P`, `--and`/`--or`/`--not`,
  `--untracked`, `-o` and `-H` are refused as unknown options; a file with a NUL
  in it is passed over rather than reported as
  `Binary file <path> matches`; and a usage error exits 1 where git exits 128 or
  129. The exit statuses a script branches on — `0`, `1`, `128` — are git's
  exactly. The reasoning is in [Notes](notes.md#finding-a-line-without-a-regex-library).
- **`repack` and `prune`, at their edges.** The packs both write hold whole
  objects and no deltas, so a repository packed here is larger on disk than the
  one `git repack` would leave; `-f`, `-F`, `--window`, `--depth`, `--threads`
  and `--write-bitmap-index` are refused as unknown options. `prune` deletes
  only loose objects: an unreachable object inside a pack is left alone, and the
  way to reclaim it is `repack -A -d` first, which is the round trip git
  describes too. `--expire` reads `now` and `<n>.<unit>.ago` and refuses
  anything else rather than guessing.
- **`rerere`, at its edges.** A conflict is filed under this tool's own hash of
  its two sides, so the entries under `rr-cache` are not git's and the two
  tools do not read each other's -- what they share is the words they say, which
  `test/surface.sh` holds them to. `--status`, `--diff` and `--forget` are
  options here where git has subcommands, and a path stays in `MERGE_RR` until
  the merge it belongs to is concluded, rather than being dropped the moment a
  resolution is recorded; keeping it is what lets `--status` still describe a
  conflict that has been answered. `--gc` drops the entries that hold no
  resolution and nothing else, with no age policy, and a conflict written in the
  diff3 style (`|||||||`, or a nested or unterminated set of markers) is neither
  recorded nor replayed, since its two sides cannot be read back out of it. The
  setting `rerere.enabled` defaults to whether the cache is there, as git's
  does, and `rerere.autoupdate` stages what was put back -- but the merge is
  still left for `commit` to conclude, with `MERGE_HEAD` written and exit 1, as
  git leaves it.
- **`stash` with `--index`, and with a pathspec.** Everything else is here:
  `push` (with `-m`, `-u` and `-k`), `list`, `show` (with `-p`),
  `apply`/`pop`, `drop`, `clear` and `branch`, with a conflict stopping in the
  shape a merge stops in and left at the `refs/stash` entry it came from. The
  two absent forms are refused as unknown options rather than quietly ignored.
