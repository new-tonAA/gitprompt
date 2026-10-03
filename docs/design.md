# The design

Why a prompt is an ordinary git blob, what a commit joins together,
and how the prompt files and their history are laid out on disk.

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
- `git log`, `git show` and the rest of git's readers read its history as any
  other repository's.
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

[`format.md`](format.md) is the byte-level version of this: the object
encodings, the index, the refs, and the state files a merge leaves behind.
