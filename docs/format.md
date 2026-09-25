# On-disk format

Two things need to survive a clone for a project to be reconstructable: the
prompt files in the working tree, and the object graph behind them. This
document describes both, and the guarantees each one provides.

## Ordering

Ordering is the whole problem. A directory of markdown files has no inherent
sequence, and a filesystem sorts lexically, which breaks the moment a project
passes nine prompts or spans two sessions. `gitprompt` therefore carries the
order in three independent places, so that no single mechanism has to be
trusted:

1. **The commit graph.** Prompts are committed in the order they were recorded,
   and `replay` walks every reachable ref — not just HEAD — so prompts on an
   abandoned branch still appear in the timeline. It also folds in prompt files
   that are not yet committed, so a session does not read as empty before its
   first commit.
2. **`seq` in the frontmatter.** A per-session counter, starting at 1. It makes
   a single prompt file self-describing: you can read one file, out of context,
   and know where it sat in its conversation.
3. **The filename prefix.** `prompts/003-fix-the-empty-password-case.md`. This
   survives even a tool that ignores every other field — `ls` in a plain shell
   produces the right order.

The filename prefix is *repository-wide* and monotonic; `seq` is *per-session*.
They disagree on purpose: the prefix orders the project, `seq` orders the
conversation. Both appear in the file.

At replay time, sessions are ordered by their start time, and prompts within a
session are ordered by `timestamp`, with `seq` and then `id` breaking ties. The
timestamps are the primary key rather than the graph because two prompts can
land in one commit, and the graph cannot order what shares a node.

## Prompt files

Prompts live in the prompt directory, `prompts/` by default (configurable with
`gitprompt.promptDir`), and are ordinary markdown:

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

The frontmatter is a deliberately small subset of YAML — scalars, inline lists
(`[a, b]`), block lists and block scalars, and no nesting — so it can be parsed
and written without a dependency, and so a hand edit cannot silently change its
meaning.

| field | meaning |
| --- | --- |
| `id` | stable identifier for this prompt (`p_…`), unique within the repo |
| `session` | the session it belongs to (`s_…`), or `-` if unattributed |
| `seq` | position within that session, from 1 |
| `timestamp` | when the prompt was recorded, ISO 8601 with offset |
| `author` | `Name <email>` from the configured identity |
| `model` | which model the prompt was written for, if noted |
| `tags` | inline list, for grouping unrelated to sessions |
| `outcome` | what this prompt produced, once known |
| `parent_prompt` | the prompt this one was written in reaction to |
| `attachments` | paths to files carried along with the prompt |

Everything after the closing `---` is the prompt text, verbatim.

Fields are written in the fixed order above, and only when set. Unknown fields
are preserved, not dropped, so a file edited by hand or by a future version
round-trips.

## The repository

```
.gitprompt/
├── HEAD              # symref: ref: refs/heads/main
├── SESSION           # id of the current session
├── config            # local configuration
├── description       # for the web UI and for humans
├── hooks/            # pre-commit, post-commit, ...
├── index             # staging area (binary, magic "GPIX")
├── info/exclude      # ignore patterns, like .gitignore
├── logs/             # reflogs: HEAD, refs/heads/*, refs/remotes/*
├── objects/xx/yyyy…  # zlib-deflated objects, addressed by sha1
├── refs/
│   ├── heads/        # branches
│   ├── tags/         # tags
│   ├── remotes/      # remote-tracking refs
│   └── sessions/     # one ref per session, pointing at its last prompt
└── remotes/<name>/mirror/   # cache for transports that need one
```

This mirrors git's layout closely enough that the habits transfer: `HEAD` is a
symref, `index` is a staged snapshot, `logs/` is a reflog, refs live under
`refs/`, and objects are content-addressed.

## Objects

```
object = "<type> <length>\0" <payload>
id     = sha1(object)
file   = objects/<id[0:2]>/<id[2:36]>, zlib-deflated
```

Four types are git's, unchanged: `blob`, `tree`, `commit`, `tag`. A `blob` tree
entry is mode `100644`. Two types are extensions:

Both extension payloads are **canonical JSON** — sorted keys, tight
separators, UTF-8 — so that the sha1 of an object is a meaningful content hash
that does not change when the same data is written twice.

### `prompt`

```json
{
  "v": 1,
  "id": "p_vw8xgbp7",
  "body": "Build a login page with email and password fields.",
  "session": "s_1790354733_42fkw2",
  "seq": 1,
  "timestamp": "2026-09-26T00:45:33+08:00",
  "author": {"name": "Your Name", "email": "you@example.com"},
  "outcome": "produced login.html; test_login fails on empty password"
}
```

Only `v`, `id` and `body` are always present; every other key appears when it
has a value. `parent_prompt` records which prompt this one was written in
reaction to, which is how a branching conversation stays reconstructable rather
than being flattened into a line.

A prompt is an object in its own right, not merely a staged file, so it is
addressed by sha1 and can be referenced from a commit, a ref or a reflog
without ambiguity.

In a tree, a prompt entry uses mode **`100640`** where a plain file uses
`100644`. Git reads `100640` as an ordinary file and will happily check it out
and diff it; `gitprompt` recognises it as a prompt and can recover the prompt
object without re-parsing the file. This is why the prompt files are useful to
non-gitprompt tools: they degrade to normal markdown.

### `session`

```json
{
  "v": 1,
  "id": "s_1790354733_42fkw2",
  "title": "Build a login page",
  "started_at": "2026-09-26T00:45:33+08:00",
  "prompts": ["3a1f9c…", "b7e204…"]
}
```

`prompts` lists the sha1 of each prompt in the session, in order. That makes a
session self-sufficient for reconstruction: read the session object and you
have the membership and the order without walking the commit graph. `ended_at`,
`notes`, `author` and `model` appear when set.

A session is a first-class object so that a boundary is recorded at the moment
it happened, rather than being inferred later by looking for gaps in
timestamps. Inference is exactly what fails across sessions and machines — a
prompt recorded at 00:45 and the next at 00:46 may belong to two sittings, and
timestamps alone cannot tell you that.

`refs/sessions/<id>` points at the session object, which keeps it reachable —
and therefore transferred by `push` and `fetch` — without requiring a commit to
reference it.

## Reconstruction

`gitprompt replay` scans the trees reachable from every ref, picks out the
entries whose type is `prompt` (the `100640` mode), reads each prompt object,
and attributes it to the commit that first introduced it. Prompts are then
grouped by session and ordered as described above. The result is written as
markdown with a preamble addressed to the agent, as JSON, or as one directory
per session (`--layout`).

A replay works from any ref — a branch, a tag, a commit — and does not depend
on the working tree being checked out or clean. `gitprompt replay <sha>`
reconstructs the project as of that moment.

Two things are deliberately forgiving. A prompt whose session object is missing
is still replayed, grouped under a synthetic entry, rather than dropped: a
missing session is a metadata gap, not a reason to lose the prompt. And a
prompt whose file gained an outcome after the last commit is counted once, not
twice — uncommitted files are matched to committed prompts by prompt id.
