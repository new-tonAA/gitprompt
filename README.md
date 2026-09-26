# gitprompt

A distributed version control system for **prompts** — written in C, on git's
own object model.

`git` versions the code. `gitprompt` versions the instructions that produced the
code: every prompt that was sent, in order, grouped by session, with the outcome
each one had. Clone somebody's prompt repository, hand it to an agent, and the
project can be rebuilt — not only the final artifact, but the sequence of
decisions that got there, including the wrong turns that were corrected.

```console
$ gitprompt clone https://github.com/you/my-project-prompts.git
$ cd my-project-prompts
$ gitprompt replay -o PROMPTS.md      # hand this file to an agent
```

## The one design decision everything else follows from

**A prompt is an ordinary git blob.** A prompt is a markdown file with a
frontmatter block at git's normal file mode, `100644`, stored in a tree under
`prompts/`. There are no extension object types, no unusual file modes, no
side-channel format. Only git's four object types — blob, tree, commit, tag —
are ever written.

That is why the rest of the design needs no special cases:

- `git clone` of a gitprompt repository works, because it *is* a git repository.
- GitHub renders the prompt files, diffs them, and shows their history.
- `git --git-dir=.gitprompt push origin main` works.
- `git reset --hard` in a directory whose `.git` is a copy of `.gitprompt`
  materialises the whole project, prompts included.

The store is `.gitprompt/` rather than `.git/`, so a gitprompt repository and a
git repository can sit in the same working tree without either noticing the
other.

## How the files are managed

A repository is a working tree plus a store. The prompts sit in the working
tree, next to the project they describe, and the store holds their history:

```
my-project/
  .gitprompt/                       the store -- an ordinary git repository
    HEAD  config  index
    objects/                        loose objects, written as git writes them
    refs/heads/  refs/tags/  refs/remotes/
    logs/                           reflogs
    SESSION                         the session prompts are being written to
  prompts/
    0001-write-a-tokenizer-first.md
    0002-add-bpe-encoding.md
    sessions/
      s_1790354733_42fkw2.md
  README.md  src/  ...              the project the prompts describe
```

Every prompt is `prompts/<NNNN>-<slug>.md`. `NNNN` is a repository-wide counter,
zero-padded -- not a commit count and not a per-session number. **It is what
makes the order recoverable:** a prompt file read on its own, without its commit
and without any other file, still says where in the sequence it belongs. The
slug is the first 40 characters of the prompt text, lowercased with runs of
non-alphanumerics collapsed to `-`, so a directory listing is readable without
opening anything.

The file is a frontmatter block and a body:

```
---
id: p_xxxxxxxx
session: s_1790354733_42fkw2
seq: 1
timestamp: 2026-09-26T00:45:33+08:00
author: Your Name <you@example.com>
model: claude-sonnet-5
tags: [validation, frontend]
outcome: Fields render; validation misses empty input.
---
Build a login page with email and password fields.
```

A session is its own file, `prompts/sessions/<id>.md`, frontmatter and no body --
the title, when it started, when it ended, and any notes. It is a file rather
than a field repeated on every prompt so that a session boundary is a fact of
the storage, not something a reader has to infer from timestamps:

```
---
id: s_1790354733_42fkw2
title: Scaffold the login page
started_at: 2026-09-26T00:31:02+08:00
ended_at: 2026-09-26T01:14:47+08:00
author: Your Name <you@example.com>
---
```

So an agent handed only the checkout can reconstruct the run: `NNNN` and `seq`
fix the order, `session` places each prompt in a session, and the session file
says when that session began and ended. Everything else -- who wrote what, when,
and what came before -- is ordinary git history, which is why `git log` and
`gitprompt log` tell the same story.

`docs/format.md` is the byte-level version of this: the object encodings, the
index, the refs, and the state files a merge leaves behind.

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

## Reconstructing a project from a prompt repository

This is the point of the whole thing. A prompt history is not a chat log: it
spans sessions, prompts revise earlier prompts rather than only appending to
them, and the outcome of each prompt is part of the record. `gitprompt replay`
renders that history as one document, with an agent-facing preamble, in
chronological order, marking where each session begins and ends:

```console
$ gitprompt replay                    # markdown, to stdout
$ gitprompt replay --format=json      # machine-readable
$ gitprompt replay --format=txt       # plain text
$ gitprompt replay --layout=tree/     # one file per prompt, plus tree/REPLAY.md
$ gitprompt replay --list-sessions
```

The order is the order the prompts were written in, and the session boundaries
are explicit, so an agent reading the document knows where context was reset.
That is the property the storage format exists to preserve.

## Commands

`gitprompt help` lists them all; `gitprompt help <command>` describes one. The
names are git's, and the behaviour is meant to match:

- **start** — `init`, `clone`, `config`
- **record prompts** — `session`, `prompt`, `capture`, `outcome`, `add`, `commit`
- **reconstruct** — `replay`, `timeline`, `log-prompt`
- **examine** — `status`, `log`, `show`, `diff`, `reflog`
- **branch and history** — `branch`, `checkout`, `switch`, `merge`, `tag`,
  `reset`, `describe`
- **collaborate** — `remote`, `push`, `fetch`, `pull`
- **plumbing** — `hash-object`, `cat-file`, `ls-tree`, `write-tree`,
  `commit-tree`, `rev-parse`, `update-ref`, `symbolic-ref`, `for-each-ref`,
  `ls-files`, `count-objects`, `verify-objects`, `check-ref-format`
- **maintenance** — `gc`, `fsck`, `stats`, `help`, `version`

## Transports

| URL form | how it works |
| --- | --- |
| `/path/to/repo`, `../repo`, `file:///path` | handled natively: loose objects are copied and the other repository's refs are written directly |
| `https://`, `git://`, `ssh://` | delegated to the `git` binary, as `git --git-dir=.gitprompt push <url> <refspec>` |

The delegation is not a workaround. It is correct precisely because
`.gitprompt` is a real git object store: git fetches and pushes it as it would
any other repository, and authentication, proxies and credential helpers come
along for free.

## What is not implemented

Stated plainly, because a tool that quietly does the wrong thing is worse than
one that says no:

- **`serve` and the `gp://` transport.** There is no HTTP server. `gitprompt
  serve` prints `not implemented` and exits non-zero rather than pretending.
  Use the delegated transports above.
- **Unmerged index stages.** git records a conflict by giving the path three
  index entries. gitprompt's index is a plain path-to-object map, so conflicted
  paths are recorded in `.gitprompt/MERGE_CONFLICTS` instead. The observable
  behaviour is the same — `UU` in `status --short`, "You have unmerged paths.",
  `commit` refusing, `merge --abort` restoring the tree, the concluding commit
  carrying two parents — but `git` reading the same index would not see the
  conflict.
- **Packfiles.** Objects are written loose and stay loose. `gc` prunes
  unreachable objects; it does not pack.
- **Merge options other than the basics.** `--no-commit`, `--ff-only` and
  `--abort` work. `--no-ff`, `--squash`, `-X` strategies and rename detection
  do not exist.
- **An editor.** `commit` needs `-m` or `-F`, except when concluding a merge,
  where `MERGE_MSG` supplies the message.
- **Unknown options are rejected** with `unknown option '--bogus'` and exit 1.
  The list of accepted options is program-wide rather than per-command, so an
  option belonging to a *different* command is accepted and ignored. Catching
  typos is the goal; the check is not a substitute for per-command validation.
- **Platforms.** Developed and built on Windows with TDM-GCC. The code is
  plain C99 with a small `#ifdef _WIN32` block for `_getcwd`/`_getpid` and
  `__USE_MINGW_ANSI_STDIO`; it has not been built on Unix.

## Status

The end-to-end suite passes: **176 checks, 0 failures**. `test/smoke.sh` covers
the object model, sessions and prompts, committing, reconstruction (ordering and
session boundaries), branches, tags, history editing, merges including conflicts
and `--abort`, local remotes, and git interoperability — the last being the
section that matters most, since a gitprompt repository is meant to be an
ordinary git repository.

```console
$ make test
```

One caveat about the machine this was developed on: Windows Smart App Control
blocks newly linked unsigned executables machine-wide, so `gitprompt.exe` cannot
be run at all after some builds. Relinking (`make clean && make`) has cleared it
every time so far, but a fresh Windows install or a different machine may need
Smart App Control turned off (Windows Security → App and browser control) before
the binary will start.

## Layout

```
src/            the implementation
test/smoke.sh   the end-to-end suite
third_party/    zlib 1.3.1, vendored as a static library
docs/format.md  the on-disk format, in full
```

`docs/format.md` documents the storage format precisely enough to write another
implementation against it — object encoding, the index, refs, the prompt and
session file formats, and the state files a merge leaves behind.
