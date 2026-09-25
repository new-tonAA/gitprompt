# gitprompt

A distributed version control system for **prompts** — the second git.

`git` versions the code. `gitprompt` versions the instructions that produced the
code: every prompt you sent, in order, grouped by session, with the outcome each
one had. Clone somebody's prompt repository, hand it to an agent, and the project
can be rebuilt — not just the final artifact, but the sequence of decisions that
got there, including the wrong turns that were corrected along the way.

```console
$ gitprompt clone https://github.com/you/my-project-prompts.git
$ gitprompt replay -o PROMPTS.md      # hand this file to an agent
```

## Why

A prompt history is not a chat log. A chat log is a flat, lossy record of one
sitting. A project built over weeks looks like this:

- many sessions, not one — the context was reset between them
- prompts that revise earlier prompts, not just append to them
- outcomes: what each prompt actually produced
- a final artifact that only makes sense alongside the road not taken

`gitprompt` stores that history with git's own machinery — same object model,
same refs, same transports — so it behaves the way you already expect, and so
the prompts can live next to the code on the same host.

## Install

Requires Python 3.9 or newer. No third-party dependencies.

```console
$ pip install .
$ gitprompt --version
gitprompt v0.1.0
```

From a checkout, without installing:

```console
$ PYTHONPATH=src python -m gitprompt --version
```

Set an identity once, the same way you would with git:

```console
$ gitprompt config --global user.name "Your Name"
$ gitprompt config --global user.email "you@example.com"
```

## Quick start

```console
$ gitprompt init my-project
$ cd my-project

$ gitprompt session start -t "Scaffold the login page"
$ gitprompt prompt "Build a login page with email and password fields."
$ gitprompt outcome p_9f3k2 "produced login.html; test_login fails on empty password"

$ gitprompt prompt "Empty password should be rejected client-side, not just server-side."
$ gitprompt outcome p_2mp91 "fixed; test_login passes"

$ gitprompt add -A
$ gitprompt commit -m "login page, with client-side validation"
```

Prompts are written to `prompts/` as ordinary markdown files with a small
frontmatter block, so they are readable, editable, diffable and reviewable
without any tooling at all:

```markdown
---
id: p_vw8xgbp7
session: s_1790354733_42fkw2
seq: 1
timestamp: 2026-09-26T00:45:33+08:00
author: Your Name <you@example.com>
outcome: produced login.html; test_login fails on empty password
path: prompts/001-build-a-login-page-with-email-and-passwo.md
---
Build a login page with email and password fields.
```

Then share it the way you share code:

```console
$ gitprompt remote add origin https://github.com/you/my-project-prompts.git
$ gitprompt push -u origin main
```

## Reconstructing a project with an agent

This is the point of the whole thing. `gitprompt replay` turns a repository into
a single document an agent can work from:

```console
$ gitprompt replay -o PROMPTS.md           # one file
$ gitprompt replay --layout out/           # one directory per session
$ gitprompt replay --json -o history.json  # machine-readable
$ gitprompt timeline                        # compact view for a human
```

The document opens with a preamble telling the receiving agent how to read it,
then walks the sessions in order:

```markdown
Work through the sessions in order. Within a session, work through the prompts
in the order given. Do not skip ahead: later prompts assume the state left by
earlier ones, and several of them correct course rather than add features.
```

- **Chronological order** comes from the commit graph, not from filenames.
- **Session boundaries** are preserved explicitly. A session is one continuous
  conversation; the boundary marks where the original context was reset. The
  agent is told to treat each boundary as a fresh start that still inherits
  everything built before it.
- **Outcomes** travel with their prompts, so the agent knows which attempts
  landed and which were abandoned.

### For the agent

If you have been handed a `gitprompt` repository, start here:

```console
$ gitprompt replay          # read this, top to bottom, then begin
$ gitprompt stats           # how big is this history
$ gitprompt timeline        # skim the shape first
$ gitprompt log-prompt      # prompts without commit noise
$ gitprompt show <sha>      # one commit
$ gitprompt show <prompt-id>  # one prompt, with its session and outcome
```

## Sessions

A session is one sitting, one conversation, one stretch of work before the
context was reset.

```console
$ gitprompt session start -t "Refactor the parser"
$ gitprompt session list
$ gitprompt session show s_1790354733_42fkw2
$ gitprompt session end -n "left off mid-refactor"
$ gitprompt session use s_1790354733_42fkw2   # resume an earlier one
```

Sessions are what make a replay faithful rather than merely complete. Without
them a history is a wall of prompts; with them it has chapters.

## Recording prompts

```console
$ gitprompt prompt "the text"                 # record one
$ gitprompt capture < prompt.txt              # from a file or a pipe
$ gitprompt prompt "the text" --model gpt-5   # note which model it was for
$ gitprompt prompt "the text" -t parser -t wip  # tag it
$ gitprompt outcome <id> "what happened"      # attach a result to a prompt
```

`prompt` takes `-m TEXT` / `-F FILE` to supply the text, and `-s SESSION` to
file it under a session other than the current one. When stdin is piped, the
piped text is used — which is how you record a prompt without retyping it.

## Remotes and hosting

Three transports, all interchangeable:

| URL form | Transport | Notes |
| --- | --- | --- |
| `/path/to/repo`, `file:///path` | local | fastest; a plain shared directory |
| `gp://host:port/repo` | HTTP | served by `gitprompt serve`; no dependencies |
| `https://github.com/you/repo.git` | git-mirror | GitHub hosting, via a carrier repo |

### Hosting on GitHub

GitHub stores git repositories, so `gitprompt` uses one as a *carrier*: your
objects and a `refs.json` are committed to a branch named `gitprompt/store`
inside an ordinary git repository. Authentication, transfer and resumption are
handled by real `git`, so whatever already works for your GitHub account works
here too.

```console
$ gitprompt remote add origin https://github.com/you/my-project-prompts.git
$ gitprompt push -u origin main
$ gitprompt clone https://github.com/you/my-project-prompts.git
```

The prompts end up browsable on GitHub like any other files, while the full
object history lives on the store branch.

### Serving over HTTP

```console
$ gitprompt serve --port 8765
$ gitprompt clone gp://127.0.0.1:8765/ /tmp/copy
```

## Commands

```
start a working area     init, clone, config
record prompts           session, prompt, capture, outcome, add, commit
reconstruct a project    replay, timeline, log-prompt
examine history          status, log, show, diff, reflog
grow, mark, tweak        branch, checkout, switch, merge, tag, reset
collaborate              remote, push, fetch, pull, serve
plumbing                 hash-object, cat-file, ls-tree, write-tree,
                         commit-tree, rev-parse, update-ref, symbolic-ref,
                         count-objects, verify-objects, check-ref-format,
                         for-each-ref
other                    stats, gc, fsck, help, version
```

Every command takes `--help`:

```console
$ gitprompt help replay
$ gitprompt help --plumbing
```

## Compatibility with git

The object model is git's, exactly:

```
object   = "<type> <length>\0" <payload>
id       = sha1(object)
storage  = zlib-deflated, at objects/<id[0:2]>/<id[2:]>
```

`git cat-file`, `git fsck` and friends can read a `gitprompt` store's
`blob`, `tree`, `commit` and `tag` objects. Two object types are extensions:

- **`prompt`** — one prompt, with its session, sequence number, author and
  outcome. Prompt entries appear in trees under mode `100640`, which git reads
  as an ordinary file and gitprompt recognises as a prompt.
- **`session`** — one session: title, start and end times, notes.

The working tree, index, refs, refspecs, reflog, packed-refs and the
fast-forward rules are all modelled on git's, including the refusal to
force-delete a branch's commits without `--force`.

## Merging

`merge` is a path-level three-way merge. Because a prompt history merges far
more often than a codebase does, a path changed on only one side is simply
taken, and two branches that added different prompts merge without a word.
Genuine divergence on the same prompt is a conflict:

```console
$ gitprompt merge side
Automatic merge failed; fix conflicts and commit the result.
Conflicting paths:
  both modified: prompts/001-the-shared-prompt.md
```

The file is left on disk with `<<<<<<<` / `|||||||` / `=======` / `>>>>>>>`
markers, each block a complete prompt file, so a resolved file keeps its id and
session. Edit it, `gitprompt add` it, and `gitprompt commit` concludes the
merge — with both parents, so the join is recorded in the history rather than
flattened into an ordinary commit.

```console
$ gitprompt status          # You have unmerged paths. — UU in --short
$ gitprompt merge --no-commit side      # prepare it, commit later
$ gitprompt merge --abort                # discard it, back to HEAD
```

`merge --ff-only` refuses rather than quietly making a merge commit.

## Status

Working: the object store, index, refs, commit graph, all three transports,
sessions, outcomes, replay, merge with conflict resolution, and the command set
above.

Not yet: a VS Code extension, `merge --squash`, and richer commit-record
metadata.

## Tests

`tests/smoke.sh` is an end-to-end regression suite — 125 assertions covering the
object store, history, replay, branches, merges and conflicts, all three
transports and CLI behaviour. It builds everything in a temporary sandbox and
needs no network.

```console
$ bash tests/smoke.sh
```

## License

GPL-2.0-only, the same licence as git.
