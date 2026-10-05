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

A prompt is an ordinary git blob, and that one decision is why the rest needs no
special cases: [The design](docs/design.md) says what a commit joins together,
and how the files sit on disk.

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

## Status

**1573 checks, 0 failures** — 1380 in `test/smoke.sh`, 157 in `test/surface.sh`
and 36 in `test/restore.sh`.

```console
$ make test
```

## Documentation

- [The design](docs/design.md) — why a prompt is an ordinary git blob, what a
  commit joins together, and how the files are laid out on disk.
- [The guide](docs/guide.md) — recording a task, following it across sessions,
  and handing the history back to an agent.
- [The commands](docs/commands.md) — all 75, grouped by what they are for.
- [Notes](docs/notes.md) — the decisions behind individual commands, the
  transports, and the places where the agreement with git is not exact.
- [What is not implemented](docs/limitations.md) — the gaps, stated plainly.
- [The format](docs/format.md) — the on-disk format, in full.
- [How it is tested](docs/testing.md) — the three suites, what each is for, and
  what they have caught.

## Layout

```
src/                 the implementation
test/smoke.sh        the end-to-end suite
test/surface.sh      the command surface, as a user meets it
test/restore.sh      a history across two machines, restored to an agent
third_party/         zlib 1.3.1, vendored as a static library
docs/design.md       the design, and what a repository looks like on disk
docs/guide.md        using it, end to end
docs/commands.md     the commands, by what they are for
docs/notes.md        the decisions behind the commands
docs/limitations.md  what is not implemented
docs/format.md       the on-disk format, in full
docs/testing.md      the three suites
```
