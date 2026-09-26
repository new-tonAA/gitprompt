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
- `git gc` repacks the store, and `gitprompt` reads the packs it leaves: loose
  and packed objects both, deltas and all.

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
    objects/pack/                   packs, read and written as git writes them
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
- **record prompts** — `session`, `prompt`, `capture`, `outcome`, `add`, `rm`,
  `mv`, `commit`
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
| `/path/to/repo`, `../repo`, `file:///path` | handled natively: every object is read out of the source and written into the target, and the other repository's refs are written directly |
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

- **`serve` and the `gp://` transport.** There is no HTTP server. `gitprompt
  serve` prints `not implemented` and exits non-zero rather than pretending.
  Use the delegated transports above.
- **Rename detection.** A rename is a delete and an add. Merging a path that
  one side renamed and the other side changed does not follow the rename.
- **Unknown options are rejected** with `unknown option '--bogus'` and exit 1.
  The list of accepted options is program-wide rather than per-command, so an
  option belonging to a *different* command is accepted and ignored. Catching
  typos is the goal; the check is not a substitute for per-command validation.
- **Platforms.** Developed and built on Windows with TDM-GCC. The code is
  plain C99 with a small `#ifdef _WIN32` block for `_getcwd`/`_getpid`,
  `__USE_MINGW_ANSI_STDIO`, and putting the streams in binary mode so that a
  newline written out is a newline and not a carriage return before it; it has
  not been built on Unix.

## Status

The end-to-end suite passes: **315 checks, 0 failures**. `test/smoke.sh` covers
the object model, sessions and prompts, committing, reconstruction (ordering and
session boundaries), branches, tags, history editing, merges including conflicts
and `--abort`, local remotes, packed object stores, and git interoperability —
the last being the section that matters most, since a gitprompt repository is
meant to be an ordinary git repository. As part of it, `git verify-pack` checks
the pack `gc` writes against git's own index, and `git ls-files` checks the
index gitprompt wrote against git's own reader.

A commit with no `-m` and no `-F` opens an editor, looked for the way git looks
for one: `GIT_EDITOR`, then `core.editor`, then `VISUAL`, then `EDITOR`. The
buffer is `COMMIT_EDITMSG`, it starts from the message the commit already has
when there is one — a merge's, or the one `--amend` is replacing — and what the
editor saves is stripped the way git strips it, so `#` lines are comments, blank
runs collapse, and a message that comes back empty aborts the commit rather than
recording a commit with no reason. `-e` asks for the editor after a `-m`,
`--no-edit` takes the message the commit already has and stops if there is none.
The suite hands `commit` editors it writes itself: one that replaces the buffer,
one that appends to it, one that fails, and none at all, and asserts on the
message that ends up in the commit rather than on the exit status.

A merge conflict is where that claim is tested hardest, because a conflict is
not only in the objects: it is in the index. gitprompt records one the way git
does — the path's base, our version and their version as index stages 1, 2 and
3 — so `git ls-files -u` inside a gitprompt store lists the same stages for the
same paths that `gitprompt status` reports, the letters `UU`/`AA`/`DU`/`UD` come
off which stages are present, and resolving is staging the path, which drops
them. The suite asserts that agreement with git directly, in both directions:
git's own conflicted index is read back by gitprompt, and git's reader sees the
one gitprompt wrote.

What goes into those stages is a line-wise three-way merge, not a choice between
whole files. Two sides that edit different lines of one file keep both edits;
edits that meet conflict, and the file is marked the way git marks it — the
lines the two sides agree on are left outside the markers rather than repeated
in both halves, either half may come out empty, and the labels are `HEAD` and
the branch being merged in. `--no-ff` records a merge commit where a
fast-forward was possible, `--squash` stages the merge without moving `HEAD` or
writing `MERGE_HEAD`, and `-X ours|theirs` settles a conflicting region for one
side instead of stopping. The suite pins those bytes down exactly rather than
checking the exit status alone: in the trimming case the whole conflicted file
is spelled out and compared, and those are the bytes `git merge-file` prints for
the same three inputs.

One caveat, since it is not visible from the exit status: the alignment is not
byte-for-byte git's. Where a file repeats a line, one edit can be described
equally well as a change to the first copy or to the second, and the two
descriptions disagree about whether a nearby edit overlaps. gitprompt keeps the
alignment its longest-common-subsequence search finds, so on such a file the two
can disagree about whether to conflict at all — gitprompt may report a conflict
where git merges cleanly, or merge cleanly where git conflicts. Neither
direction loses an edit: a clean merge is still both sides' changes applied, and
a conflict is still both sides' text. It takes a repeated line with an edit
beside it for the two to part company.

The pack reader is exercised against packs git wrote, not only against the ones
`gc` writes itself: `gc` writes whole objects, so a pack it made has no deltas
in it. Packs written by `git repack` and by `git pack-objects`, with offset
deltas and reference deltas, chains several deep, have been read back object by
object and re-hashed.

The packed-store section also pins down when a pack may be dropped. A second
`gc` of an unchanged store rewrites the same pack under the same name, and
deleting that as superseded would take every object with it; a pack holding an
object no ref reaches is kept, because an object a fetch left packed has no
loose copy to fall back on; and every reader — `replay`, `timeline`,
`log-prompt`, `stats` — is run against a packed store, since wanting an object's
contents without its type is a different path through the store from wanting
both.

The suite runs offline, so it covers the local transport and leaves the carrier
transports to git. `push`, `fetch`, `pull` and `clone` against an `https://`
remote have been exercised by hand against a repository on GitHub; anything
added there is worth running the same way before it is trusted.

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
