# gitprompt

**git versions the code. gitprompt versions the prompts that produced it.**

[![ci](https://github.com/new-tonAA/gitprompt/actions/workflows/ci.yml/badge.svg)](https://github.com/new-tonAA/gitprompt/actions/workflows/ci.yml)

A distributed version control system for **prompts** — written in C, on git's
own object model.

Every prompt that was sent, in order, grouped by session, with the outcome each
one had. Clone somebody's prompt repository, hand it to an agent, and the project
can be rebuilt — not only the final artifact, but the sequence of decisions that
got there, including the wrong turns that were corrected.

It is the other half of a project to git: git keeps the code, gitprompt keeps the
instructions that wrote it, and the two can sit in the same directory without
either noticing the other.

```console
$ gitprompt clone https://github.com/you/my-project-prompts.git
$ cd my-project-prompts
$ gitprompt replay -o PROMPTS.md      # hand this file to an agent
```

Four things make that work, and each is a thing a chat log cannot do:

- **Every prompt, in order.** Each one is a numbered file,
  `prompts/0001-write-a-tokenizer-first.md`. The number is repository-wide, so a
  prompt read on its own — with no commit and no other file — still says where
  in the sequence it belongs.
- **Session boundaries are stored, not inferred.** Where context was reset is a
  file in the tree, not something a reader guesses from timestamps.
- **What came back can be kept too.** The agent's answer is a file of its own
  beside the prompt, so a history read later carries both the question and what
  it got — and a replay can add to them.
- **Ordinary git all the way down.** A gitprompt repository *is* a git
  repository: `git clone`, `git push`, GitHub's file view, `git reset --hard`
  and `git gc` all work on it unchanged.

## The one design decision

**A prompt is an ordinary git blob.** A prompt is a markdown file with a
frontmatter block at git's normal file mode, `100644`, stored in a tree under
`prompts/`. There are no extension object types, no unusual file modes, no
side-channel format. Only git's four object types — blob, tree, commit, tag —
are ever written, which is why `git clone`, `git log`, `git gc` and GitHub's
file view all work on a gitprompt repository unchanged.

What that buys, and what a commit joins together, is in
[The design](docs/design.md), along with how the prompt files, their
sessions and their answers are laid out on disk.

## Build

Needs a C99 compiler, `make`, and nothing else. zlib is vendored under
`third_party/zlib` as a static library, so the result depends on no DLLs and no
system packages.

```console
$ git clone https://github.com/new-tonAA/gitprompt.git
$ cd gitprompt
$ make                     # Unix
$ mingw32-make             # Windows, TDM-GCC or MinGW-w64
$ ./gitprompt version
```

Set an identity once, the same way as git:

```console
$ gitprompt config --global user.name "Your Name"
$ gitprompt config --global user.email "you@example.com"
```

## Quick start

```console
$ gitprompt init my-project
$ cd my-project

$ gitprompt session start -t "Scaffold the login page"
$ gitprompt prompt -m "Build a login page with email and password fields."
$ gitprompt prompt -t validation -m "Add client-side validation."
$ gitprompt outcome --last "Fields render; validation misses empty input."
$ gitprompt session end

$ gitprompt add -A
$ gitprompt commit -m "Scaffold the login page"

$ gitprompt timeline            # the prompts, in order, with their sessions
$ gitprompt replay -o PROMPTS.md
```

## Commands

`gitprompt help` lists them all; `gitprompt help <command>` describes one. The
names are git's, and the behaviour is meant to match:

- **start** — `init`, `clone`, `config`
- **record prompts** — `session`, `prompt`, `capture`, `response`, `outcome`,
  `add`, `rm`,
  `mv`, `commit`
- **reconstruct** — `replay`, `timeline`, `log-prompt`, `attach`, `rerun`
- **examine** — `status`, `log`, `show`, `diff`, `reflog`, `blame`, `grep`,
  `bisect`
- **branch and history** — `branch`, `checkout`, `switch`, `merge`,
  `cherry-pick`, `rebase`, `revert`, `stash`, `tag`, `reset`, `describe`
- **collaborate** — `remote`, `push`, `fetch`, `pull`, `serve`
- **plumbing** — `hash-object`, `cat-file`, `ls-tree`, `write-tree`,
  `commit-tree`, `rev-parse`, `rev-list`, `merge-base`, `update-ref`,
  `symbolic-ref`, `for-each-ref`, `ls-files`, `count-objects`,
  `verify-objects`, `check-ref-format`
- **maintenance** — `gc`, `fsck`, `stats`, `help`, `version`

## Transports

| URL form | how it works |
| --- | --- |
| `/path/to/repo`, `../repo`, `file:///path` | handled natively: every object is read out of the source and written into the target, and the other repository's refs are written directly |
| `gp://host[:port][/path]` | gitprompt's own transport, with `gitprompt serve` on the far end: HTTP with a `Content-Length`, one request per connection |
| `https://`, `git://`, `ssh://` | delegated to the `git` binary, as `git --git-dir=.gitprompt push <url> <refspec>` |

The delegation is not a workaround. It is correct precisely because
`.gitprompt` is a real git object store: git fetches and pushes it as it would
any other repository, and authentication, proxies and credential helpers come
along for free.

It is also why packfiles are read. A `git fetch` does not leave loose objects
behind above a small threshold: it leaves a pack, with the objects inside it
delta-compressed against each other. A store with no pack reader would be
unable to read back its own fetched history.

## What is not implemented

Stated plainly, because a tool that quietly does the wrong thing is worse than
one that says no:

- **Byte-for-byte `diff` output.** A move is reported as a move, and the changes
  are the changes git reports, but the text around them is not git's. No `index`
  line is printed, nor a `deleted file mode` or `new file mode` one; a hunk that
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
- **Platforms.** Developed and built on Windows with TDM-GCC. The code is
  plain C99: what is Windows-specific is a small `#ifdef _WIN32` block for
  `_getcwd`/`_getpid`, `__USE_MINGW_ANSI_STDIO`, and putting the streams in
  binary mode so that a newline written out is a newline and not a carriage
  return before it, and the sockets in `net.c`, which winsock provides on
  Windows and libc on Unix. CI builds and runs the suite on Linux and macOS as
  well as Windows, which is the only place the Unix builds are exercised: the
  development machine has one compiler for one of the three.
- **The commands git has that gitprompt does not.** git 2.49 lists 176; a
  gitprompt built from this tree lists 62. Missing are `clean`,
  `archive`, `notes`, `worktree`, `submodule`, `apply`, `shortlog` and
  `range-diff`, along with the layers under them -- packfile writing of the
  kind `repack` and `prune` need, credential helpers, sparse checkout, `replace`
  and `rerere`. The object model, the index, committing, history, branches,
  merging including conflicts, replaying a commit elsewhere, undoing one,
  setting work aside, tags,
  reset, the ref plumbing, remotes and the prompt layer are all here.
- **`stash` with `--index`, and with a pathspec.** Everything else is here:
  `push` (with `-m`, `-u` and `-k`), `list`, `show` (with `-p`),
  `apply`/`pop`, `drop`, `clear` and `branch`, with a conflict stopping in the
  shape a merge stops in and left at the `refs/stash` entry it came from. The
  two absent forms are refused as unknown options rather than quietly ignored.

## Status

**969 checks, 0 failures** — 852 in `test/smoke.sh`, 81 in `test/surface.sh`
and 36 in `test/restore.sh`.

```console
$ make test
```

[How it is tested](docs/testing.md) describes what each suite is for, and what
the two of them have caught.

## Documentation

- [The design](docs/design.md) — why a prompt is an ordinary git blob, what a
  commit joins together, and how the files are laid out on disk.
- [The guide](docs/guide.md) — recording a task, following it across sessions,
  and handing the history back to an agent.
- [Notes](docs/notes.md) — the decisions behind individual commands, and the
  places where the agreement with git is not exact.
- [How it is tested](docs/testing.md) — the three suites, and what each is for.
- [The format](docs/format.md) — the on-disk format, in full.

## Layout

```
src/                 the implementation
test/smoke.sh        the end-to-end suite
test/surface.sh      the command surface, as a user meets it
test/restore.sh      a history across two machines, restored to an agent
third_party/         zlib 1.3.1, vendored as a static library
docs/design.md       the design, and what a repository looks like on disk
docs/guide.md        using it, end to end
docs/notes.md        the decisions behind the commands
docs/format.md       the on-disk format, in full
docs/testing.md      the three suites
```
