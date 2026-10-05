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
  gitprompt built from this tree lists 64. Missing are
  `archive`, `notes`, `worktree`, `submodule`, `apply`, `shortlog` and
  `range-diff`, along with the layers under them -- credential helpers, sparse
  checkout, `replace`
  and `rerere`. The object model, the index, committing, history, branches,
  merging including conflicts, replaying a commit elsewhere, undoing one,
  setting work aside, tags,
  reset, the ref plumbing, remotes, the prompt layer and the object-store
  maintenance `gc`, `repack` and `prune` are all here.
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
- **`stash` with `--index`, and with a pathspec.** Everything else is here:
  `push` (with `-m`, `-u` and `-k`), `list`, `show` (with `-p`),
  `apply`/`pop`, `drop`, `clear` and `branch`, with a conflict stopping in the
  shape a merge stops in and left at the `refs/stash` entry it came from. The
  two absent forms are refused as unknown options rather than quietly ignored.
