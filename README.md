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

Three things make that work, and each is a thing a chat log cannot do:

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

## The one design decision everything else follows from

**A prompt is an ordinary git blob.** A prompt is a markdown file with a
frontmatter block at git's normal file mode, `100644`, stored in a tree under
`prompts/`. There are no extension object types, no unusual file modes, no
side-channel format. Only git's four object types — blob, tree, commit, tag —
are ever written.

That is why the rest of the design needs no special cases:

- `git clone` of a gitprompt repository works, because it *is* a git repository.
  It brings the prompt files across and leaves the store behind, so the clone is
  told exactly that if you ask it to do something, and `gitprompt init .` there
  takes the `prompts/` tree over as the history — one command, and the clone
  records, replays and pushes again.
- GitHub renders the prompt files, diffs them, and shows their history.
- `git --git-dir=.gitprompt push origin main` works.
- `git reset --hard` in a directory whose `.git` is a copy of `.gitprompt`
  materialises the whole project, prompts included.
- `git gc` repacks the store, and `gitprompt` reads the packs it leaves: loose
  and packed objects both, deltas and all.

The store is `.gitprompt/` rather than `.git/`, so a gitprompt repository and a
git repository can sit in the same working tree without either noticing the
other.

## What a commit is

A commit is the join between the code it adds and the prompts that produced it.
`gitprompt commit` records that join **in the commit**, one `gp-prompt` header
per prompt the commit adds or changes:

```console
$ gitprompt log -n 1
commit f449d920a62b40806f922cda0fc6cebf970f2c7b
Author: You <you@example.com>
Date:   2026-10-01T15:28:39+08:00
Session: s_1790839718_nwfj5r
Prompts: p_lv2thw47

    reject trailing commas

$ git --git-dir=.gitprompt cat-file -p HEAD
tree 18b8ed55d472272ad7706f5de51a5c6e9da98f90
parent b947c10c982ddce2f9b73fa4b231139d068d4237
author You <you@example.com> 1790839719 +0800
committer You <you@example.com> 1790839719 +0800
gp-session s_1790839718_nwfj5r
gp-prompt p_lv2thw47

reject trailing commas
```

Which prompts those are is decided by the commit's own tree against its first
parent's: a prompt file the commit adds or changes is one of them, a commit of
code alone carries none, and editing a prompt's outcome and committing that
carries it too. So the answer cannot drift from what was committed — it is read
out of the objects, not out of the working tree.

Because the join lives in the commit object, a plain `git clone` has it without
gitprompt being part of the transfer, and the clone can say which prompts go
with which code before it has any store of its own:

```console
$ git cat-file -p HEAD | grep gp-prompt
gp-prompt p_lv2thw47
```

So the history is not two lists that happen to sit in the same repository: the
code says which prompts made it, and a clone that has only ever run `git clone`
can read that off the commits.

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
    responses/
      p_yyyyyyyy.md                 what the agent answered 0001
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

A session that was left and returned to carries a `segments` line as well: one
`start..end` pair per stretch, with the end left empty while a stretch is still
open.

```
started_at: 2026-09-10T09:00:00+08:00
ended_at: 2026-09-10T17:00:00+08:00
segments: 2026-09-10T09:00:00+08:00..2026-09-10T12:00:00+08:00, 2026-09-10T15:00:00+08:00..2026-09-10T17:00:00+08:00
```

`started_at` and `ended_at` remain the span of the whole session, so a reader
that knows nothing of `segments` still gets the right answer; `segments` is what
keeps the hours in between from being swallowed by it.

An answer is its own file too, `prompts/responses/<prompt id>.md`, named after
the prompt rather than after itself:

```
---
id: r_xxxxxxxx
prompt: p_yyyyyyyy
session: s_1790354733_42fkw2
timestamp: 2026-09-26T00:47:11+08:00
model: claude-sonnet-5
---
Created login.html. Validation misses empty input.
```

It is separate because of what a prompt file is: prompt text is somebody's own
words, with no delimiter in it that could be trusted to mean "the answer starts
here". And because of when it arrives -- the prompt is recorded as it is said,
the answer only once an agent has replied, and it may never. `prompt` is the id
the two are joined on, and the file's name repeats it so an answer can be found
without opening every file.

So an agent handed only the checkout can reconstruct the run: `NNNN` and `seq`
fix the order, `session` places each prompt in a session, the session file says
when that session ran, and a response file says what the agent said back.
Everything else -- who wrote what, when, and what came before -- is ordinary git
history, which is why `git log` and `gitprompt log` tell the same story.

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
them, and the outcome of each prompt is part of the record -- as is what the
agent answered, when somebody kept it. `gitprompt replay`
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

### A task that spanned several sessions

Opening a session over one that is already open is a switch, and the session
being left is closed at the instant the new one opens -- so the two meet exactly,
and the one left behind records the stretch it really had rather than a span with
the interruption silently inside it. The recorder is told which session it left,
because otherwise there is no way to know that the next prompts are not going
into the conversation they were meant for:

```console
$ gitprompt session start -t "Plan the tokenizer"
session s_1790514149_qr22tt started
$ gitprompt session start -t "Check the CI logs"
warning: pausing session s_1790514149_qr22tt; `gitprompt session use s_1790514149_qr22tt` records into it again
session s_1790514149_4e29cj started
$ gitprompt session use s_1790514149_qr22tt
session s_1790514149_qr22tt had ended at 2026-09-27T21:02:29+08:00
(recording into it again opens a new stretch)
now recording into s_1790514149_qr22tt
```

`session use` refuses an id that names no session file, rather than pointing the
recorder at a boundary that exists nowhere but in the pointer.

`replay` groups by session, because a conversation is one context and
interleaving two of them would read as one that never happened. Its `--flat`
form answers the other question -- what was worked on, in what order:

```console
$ gitprompt replay --flat                  # markdown, each prompt naming its session
$ gitprompt replay --flat --format=txt
$ gitprompt replay --flat --format=json
```

The order there is the sequence number rather than the timestamp, which is what
makes a conversation imported after a later one come out in the order it was
filed rather than the order its dates happen to run.

### Handing the history back to an agent

`attach` writes the history where an agent reads it, in the mode where reading
is all it does. `rerun` is the other half: the same history given to an agent
that is allowed to act on it, one prompt at a time.

What makes it more than a loop over the prompt files is the conversations. Each
session is mapped to one agent conversation, and the prompts are handed over in
written order — so a task that moved between sessions and back is replayed the
same way, the prompt that returns resuming the conversation that session began:

```console
$ gitprompt rerun
Which agent should replay this history?
  1. claude   claude -p
  2. codex    -- cannot resume a session, so a replay into it would not be one
Pick one [1]:
rerun: 3 of 3 prompt(s), agent claude, permission mode acceptEdits
       2 conversation(s): one per session, and a session
         returned to is resumed rather than begun again,
         so the replay crosses them as the history did
       recorded 2026-09-26T00:31:02+08:00 .. 2026-09-26T01:14:47+08:00
       replayed in seq order, which is the order they were
         written -- the one thing a clock cannot say for them
       the agent works in /home/you/project and may change it

Start the replay now? [y/N] y

run    1  p_crmwszhq  s_1790510692_39lroz      start
run    2  p_gy32kr9c  s_1790510692_2fb6jq      start
run    3  p_xbr72wvy  s_1790510692_39lroz      resume

rerun: 3 prompt(s) across 2 conversation(s)
```

The conversation ids are derived from the session ids rather than handed out at
random, so the same history replays into the same conversations — the same ones
on the machine that pushed and on the machine that cloned — and a run that
stopped half way can be started again from where it stopped with
`--from <prompt id>`. `--salt` asks for a fresh set instead.

A prompt is arbitrary text, so it is never put on a command line: it is written
to a file in the store and handed to the agent on standard input, which is what
removes the question of what a quote or a percent sign in someone's prompt would
have done to the shell.

Nothing runs unless it is asked for. A rerun starts processes that edit the work
tree, so at a terminal it asks which agent to replay into and then asks to
confirm before it begins — the agent is a fact about the machine that pulled the
history, and a clone carries no answer to it. With no terminal to ask, the plan
above is printed and nothing more happens until `--yes`:

```console
$ gitprompt rerun --agent=claude             # skips the menu, still confirms
$ gitprompt rerun --yes                      # runs it, claude's default mode
$ gitprompt rerun --yes --permission-mode=bypassPermissions
$ gitprompt rerun --yes --model opus --from p_xbr72wvy
$ gitprompt rerun --only-session s_1790510692_39lroz
```

The prompts are handed over **one at a time** — each is a separate agent
invocation, and the next starts only when the one before it has exited — in the
order the sequence numbers were handed out when they were recorded, which is the
order a clone months later can still reconstruct.

**It reconstructs the prompts, not the project.** The prompts are stored exactly
and are handed over verbatim; an agent doing the work a second time may do it
differently, and nothing here can promise otherwise. Answers are kept only when
somebody kept them — see the next section — so a replay carries the ones the
history already has and can add the new ones to it. The project itself is
restored exactly by checking out the commit, which is what `checkout` is for —
the prompts are the *how it was made*, the commits are the *what was made*, and
only the second is byte-exact.

`codex` is refused rather than half-supported. Its conversations cannot be given
an id to resume by, so an interrupted history could not be played back as the
conversations it was — and running each session as a string of unrelated ones
would not be a replay of anything.

### Keeping what the agent answered

A prompt on its own is half of what happened. `response` records the other half,
against the prompt it belongs to:

```console
$ gitprompt prompt -m "Build a login page with email and password fields."
p_ibd076r6 prompts/0001-build-a-login-page-with-email-and-passwo.md
$ gitprompt response -m "Created login.html. Validation misses empty input."
r_vql9qir6 prompts/responses/p_ibd076r6.md
$ gitprompt response -m "Added the check; empty input is refused now." p_ibd076r6
error: response: p_ibd076r6 is already answered (use --force to replace it)
```

The answer goes in a file of its own, `prompts/responses/<prompt id>.md`, rather
than inside the prompt file. A prompt file is text somebody may have written by
hand, so there is no delimiter in it that could be trusted to mean "the answer
starts here" — and the two arrive at different times, since the answer exists
only once an agent has replied, and may never. Being an ordinary file in the
tree is what makes it travel: a clone, a push and a `checkout` carry the answers
with the prompts, and `git --git-dir=.gitprompt ls-tree` shows both.

With no prompt id the newest prompt is the one being answered, and the text can
also come from a file or down a pipe, which is how it normally arrives:

```console
$ claude -p < notes.md | gitprompt response p_ibd076r6
```

Nothing else has to change for the answer to be part of the history:

```console
$ gitprompt replay --flat          # the document gains **Response.** blocks
$ gitprompt log-prompt             # the listing gains a response line
$ gitprompt attach                 # the agent's context file carries them
$ gitprompt replay --flat --format=json   # "response" is an object, or null
```

`rerun --record` is the same thing without a second step: it runs the agent with
its output coming back rather than going only to the terminal, prints it through
unchanged, and records it against the prompt it was answering. A run that
finishes records; a run that fails or is interrupted records nothing for the
prompt it was on, so an answer is never half a one. Recording replaces an answer
that is already there, because a rerun is what is being kept now.

The run path was verified by hand against the real CLI, and the suite runs a
stub agent for it: the plan `rerun` would execute, the conversations it maps
sessions to, and what `--record` keeps. No runner has a real agent installed,
and one that did would not answer the same twice.

### Recording a conversation that happened earlier

A prompt recorded today is dated today, which is right for one being typed now
and wrong for one being written down afterwards — and when a run is imported
after the fact, the dates are most of what tells one run from another. So they
can be given:

```console
$ gitprompt session start -t "Plan the tokenizer" --date='2026-03-01T09:00:00+08:00'
$ gitprompt prompt --date='2026-03-01T09:12:40+08:00' -m "Write a tokenizer first."
$ gitprompt session end --date='2026-03-01T09:30:00+08:00'
```

`--date` takes what git takes in `GIT_AUTHOR_DATE` — ISO 8601 with or without
an offset, a bare day, or an `<epoch> <offset>` pair — and that variable is
honoured when no `--date` is given, so a script that sets it for git sets it
here too. An explicit `--date` wins over it. A date given with an offset is kept
in that offset; one given without is read as the machine's local time and
written back out that way, so the document `replay` produces shows the wall
clock the prompt was written at. A date that cannot be read is refused rather
than silently replaced with now.

Sessions are ordered by when they began, so a run recorded out of order still
comes back in the order it happened.

## Commands

`gitprompt help` lists them all; `gitprompt help <command>` describes one. The
names are git's, and the behaviour is meant to match:

- **start** — `init`, `clone`, `config`
- **record prompts** — `session`, `prompt`, `capture`, `response`, `outcome`,
  `add`, `rm`,
  `mv`, `commit`
- **reconstruct** — `replay`, `timeline`, `log-prompt`, `attach`, `rerun`
- **examine** — `status`, `log`, `show`, `diff`, `reflog`
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
  gitprompt built from this tree lists 58. Missing are
  `bisect`, `blame`, `clean`, `grep`,
  `archive`, `notes`, `worktree`, `submodule`, `apply`, `shortlog`
  and `range-diff`, along with the layers under them -- packfile writing of the
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

The end-to-end suite passes: **860 checks, 0 failures** — 757 in
`test/smoke.sh`, 67 in `test/surface.sh` and 36 in `test/restore.sh`.

```console
$ make test
```

`test/smoke.sh` covers the object model, sessions and prompts, the dates a
prompt and a session can be given, a task recorded in sessions that were
interleaved and returned to, committing, reconstruction (ordering, session
boundaries, and the flat chronology), the plan `rerun` would execute, the agent
conversations it maps sessions to, and what `response` and `rerun --record` keep
and how every rendering shows it, branches, tags, history editing, the reflog of
where HEAD has been, the commits a range of revisions reaches, merges
including conflicts
and `--abort`, merges that follow a file that moved, commits replayed with
`cherry-pick`, `rebase` and `revert` including their conflicts, empty results and
`--continue`/`--skip`/`--abort`, work set aside and put back with `stash`
including the untracked files, the index kept, a clash and the entry a branch
can be made from, `status` and `diff` on a
move, the commit editor, per-command option validation, that the replay plan and
a recorded date read the same from any clock, local remotes, serving
over `gp://`, packed object stores, and git interoperability — the last being
the section that matters most, since a gitprompt repository is meant to be an
ordinary git repository. As part of it, `git verify-pack` checks the pack `gc`
writes against git's own index, and `git ls-files` checks the index gitprompt
wrote against git's own reader.

`test/surface.sh` asks the other question: not whether each command is right in
depth, but whether the whole surface still is when the commands are used in the
order a user meets them. Every check is the operation git does — commit,
branch, merge, tag, describe, reset, mv, rm, checkout `--`, clone, push, pull —
run against a history of prompts, and each has to produce the thing git
produces. What needs git itself is skipped rather than faked when git is not on
PATH. Two things `smoke.sh` did not catch were found here — a merge that carried
no prompts, and a reflog that forgot the past after a checkout — which is what
made it worth keeping rather than folding in.

`test/restore.sh` runs the design claim end to end. It builds a history that
crosses two sessions and returns to the first, records it out of clock order,
pushes it to a bare git remote, and restores it on another machine through a
plain `git clone` with no gitprompt store in it: adopting the clone, comparing
the plan it would replay against the one the original would, and handing the
prompts to a stub agent — one at a time, in the order they were written, into
the conversations they came from — with the same replay planned under any clock.

### Following a file that moved

Three places look for a move — a merge that has to follow one, `status` at the
index, and `diff` between two trees — and all three look the same way, so they
agree about what moved.

The judgement itself: a path one side no longer has and the other side has
gained is the same file when it holds the same object, and failing that when
enough of its text is still the same — half the lines, the line git draws, which
is what catches a file that was moved *and* edited. The pairing is one to one, so
two copies of one file are not two renames of the original, and the search is
capped at a thousand pairs the way git's `diff.renameLimit` is, past which the
contents are not read.

With the move recognised the merge happens under the new name, contents and all
— a three-way merge of the file when both sides edited it. Where a move cannot be
followed it comes back unmerged with git's own stages, so `status` prints git's
letters: `DU`/`UD` when a move met a deletion, `DD`/`AU`/`UA` when both sides
gave the file a different name.

`status` reports a move where the index has one: a staged move is `renamed: a ->
b` in the long report and `R  a -> b` in `--short`, both as git prints them. A
move in the work tree that was never staged is not one — the index knows the old
name and not the new — which is also what git says. The words in the long report
are padded to the column git pads them to, and the staged section is printed
before the unstaged one whatever order the paths sort in.

`diff` reports the move in git's shape: the `diff --git` line names both paths, a
`similarity index` says how much of the file survived, and `rename from`/`rename
to` spell the pair out. The hunks follow only when the move carried a change, and
`--stat` writes the move as `a.txt => b.txt` — one that changed no lines being a
file changed with nothing added or removed. The score is the share of lines the
two copies have in common — the same measure the merge uses to decide the move
happened at all.

### The commit message, when there is no `-m`

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

### A merge is three-way, line by line

A merge conflict is where the agreement with git is tested hardest, because a
conflict is not only in the objects: it is in the index. gitprompt records one
the way git does — the path's base, our version and their version as index
stages 1, 2 and 3 — so `git ls-files -u` inside a gitprompt store lists the same
stages for the same paths that `gitprompt status` reports, the letters
`UU`/`AA`/`DU`/`UD` come off which stages are present, and resolving is staging
the path, which drops them. The suite asserts that agreement with git directly,
in both directions: git's own conflicted index is read back by gitprompt, and
git's reader sees the one gitprompt wrote.

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

### A replay is a merge read the other way

`cherry-pick` and `rebase` are not a second merge implementation. Applying a
commit somewhere else is the three-way merge `merge` already performs, with the
roles read differently: the commit's own parent is the base, the tree `HEAD`
holds now is ours, and the commit being replayed is theirs. So the line-wise
merge, the index stages a conflict leaves, the letters `status` reports for them
and `-X ours|theirs` are the ones described above — a replay that conflicts
stops in the same shape a merge does, and is resolved the same way. `rebase` is
that, once per commit, with the branch moved to the last one at the end.

What the replayed commit carries is the same rule every other commit follows,
read against the parent it has now: the prompts its new tree holds that its new
parent does not. A prompt the branch already has is therefore not carried twice,
and one it lacks travels with the code — nothing has to be copied, and the
prompt history of the replayed commit stays the prompt history it had. The suite
checks that from both sides: a prompt is carried by the commit that introduced
it and by no other, and no prompt appears twice in the history a rebase wrote.

A merge commit is not replayed, which is the choice git makes too: the commits
it joined are replayed and the merge commit itself is dropped, so a branch that
merged its upstream and then moved on comes back as a line. And the branch stays
where it is until the replay is done — the rebase detaches `HEAD` at the
upstream — so an interrupted replay, or one ended with `--abort`, leaves the
branch exactly where it was. Where an ordinary `merge` put a `MERGE_HEAD`, a
replay puts `sequencer/` (§7 of [docs/format.md](docs/format.md)), and `commit`
refuses while either is there: an interrupted replay is finished by
`cherry-pick`/`rebase --continue`, `--skip` or `--abort`, not by committing by
hand.

### An undo is that same merge, the other way round

`revert` is not a third merge either. Where a replay adds a commit's change to
where `HEAD` is, an undo subtracts it, which is the same three-way merge with the
two sides swapped: the commit itself is the base, the tree `HEAD` holds now is
ours, and the tree of the parent the change is measured against is theirs. `-m
<parent number>` names which parent that is on a merge commit — a merge has no
single side to undo, so it is required there — and a commit with no parents has
nothing to measure against but the empty tree, so reverting a root commit takes
away everything it introduced.

The prompt rule needs no exception, and it is worth stating plainly because it
reads the other way from the commit that is being undone. The undo is a commit
like any other, so it carries the prompts its new tree adds against its new
parent — which is to say, undoing a commit that introduced a prompt removes that
prompt along with the code it described. Reverting the undo (git's wording,
`Reapply "<subject>"`) brings both back. Nothing copies anything: the prompt
moves because it is a file in the tree, exactly as it moves under `cherry-pick`
and `rebase`.

An undo is the reverter's own commit, not the original author's work landing
elsewhere, so its author and committer are whoever ran it and its message says
which commit it undid. It conflicts, continues, skips and aborts through the same
`sequencer/` state the other two use, and `commit` refuses while one is in
flight.

### Setting work aside

A stash is not a special kind of storage. An entry is three commits written
under `refs/stash`: the work tree, the index, and -- when `-u` is given -- the
untracked files as a commit with no parents, so that nothing else reaches them
and they cannot be mistaken for history. The work tree commit's first parent is
`HEAD`, its second is the index commit, and it is an ordinary revision: anything
that takes a revision takes a stash entry, and `stash branch` exists because
that is literally what it does.

That shape is chosen for the prompt rule's sake, not the other way round. The
prompts of the work tree are files in the stash commit's tree, so the prompt for
work in progress is set aside with the code it was written for and comes back
with it, with no rule for prompts and none for stashes. A stash made from a
repository that had just had a prompt written for it names that prompt on the
entry, and putting the entry back puts the prompt back with the rest.

Putting one back is the merge a replay performs, read with the commit the stash
was made on as the base: the stashed state is the far side, the index is the
near side, and the result is written to the work tree with the index left at
`HEAD` -- which is what makes a stash read as unstaged changes, and what makes
`-k` put an entry back as a clash rather than an overwrite, since the index a
`-k` push left behind is still ahead of `HEAD`. A conflict is a merge's
conflict, with git's names for the two sides, `Updated upstream` and `Stashed
changes`, and the entry is kept when `pop` stops there: a half-applied stash is
still the only copy of the rest.

### An option belongs to its command

Each command hands the argument parser the list of options it accepts, and
anything else is refused with `unknown option '--amend'` and a hint naming what
that command does take; a name ending in `=` is one that takes a value. The
distinction is the point: `gitprompt log --amend` used to be accepted and
ignored, which is worse than an error, because an option that belongs to a
different command looked as though it had taken effect. Options git has and
gitprompt has not implemented are refused the same way rather than quietly doing
nothing: `checkout --source`, `commit --author`, `fetch --depth`, `gc --prune`,
`fsck --strict`, `cat-file --batch`, `for-each-ref --format`, `push --prune`,
and `version --build-options` are all errors here; `gitprompt help <command>`
shows the list each one really takes. `show --stat` was the one option of that
kind cheap enough to implement instead, and it is implemented.

### The store

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

### Serving the store

`serve` is the one transport that is gitprompt's own, and it is deliberately not
git's wire protocol. It does not have to be: the store is already a git object
store, so the exchange it needs is the one the local transport already performs
— read the far side's refs, copy across the objects that are missing here — and
HTTP is only somewhere to carry those bytes, with a `Content-Length` so neither
end has to guess where a body ends. `gitprompt serve` answers `/info/refs`,
`/HEAD` and the objects, and takes a push at `/gp/push`; a push sends its objects
first and the ref is checked afterwards, so a rewind is refused with a reason
(`it would move backwards`) unless it is forced. It serves only the store:
`config` and `index` are 404, and so is a path that tries to climb out.

The same server answers git's *dumb* HTTP protocol, because a store that is a
real git object store should be clonable by the real git — the suite clones one
with `git` to say so. One request per connection also means a long-lived process
that reloads nothing: the packs are re-listed for every request, because the
repository being served is usually one whose owner is still using it, and a
server that had answered a request before a `gc` would otherwise go on offering
the loose objects that `gc` had just removed.

### Offline, and on this machine

The suite runs offline, so the transports it covers are the local ones: a path on
disk, and `gp://` on loopback with a server it starts itself. `push`, `fetch`,
`pull` and `clone` against an `https://` remote have been exercised by hand
against a repository on GitHub; anything added there is worth running the same
way before it is trusted.

One caveat about the machine this was developed on: Windows Smart App Control
blocks newly linked unsigned executables machine-wide, so `gitprompt.exe` cannot
be run at all after some builds. Relinking (`make clean && make`) has cleared it
every time so far, but a fresh Windows install or a different machine may need
Smart App Control turned off (Windows Security → App and browser control) before
the binary will start.

## Layout

```
src/              the implementation
test/smoke.sh     the end-to-end suite
test/surface.sh   the command surface, as a user meets it
test/restore.sh   a history across two machines, restored to an agent
third_party/      zlib 1.3.1, vendored as a static library
docs/format.md    the on-disk format, in full
```

`docs/format.md` documents the storage format precisely enough to write another
implementation against it — object encoding, the index, refs, the prompt and
session file formats, and the state files a merge leaves behind.
