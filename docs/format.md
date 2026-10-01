# The gitprompt storage format

Everything gitprompt writes is described here, precisely enough to implement
against. The short version is: **object for object, gitprompt's repository is a
git repository.** Only git's four object types are used — blob, tree, commit,
tag — with git's encodings, git's ids, and git's file modes. The parts specific
to gitprompt are the directory the store lives in, the layout of the prompt
files inside it, and three small state files.

The store is `.gitprompt/`, not `.git/`. Nothing else about the layout changes,
which is why `git --git-dir=.gitprompt` can read, clone and push it.

## 1. Objects

An object is named by `sha1("<type> <length>\0" + payload)`, exactly as in git,
and stored zlib-deflated at

```
.gitprompt/objects/<first two hex digits>/<remaining 38 hex digits>
```

Objects are written loose, unless the store already has them in a pack -- an
object is only written if neither form holds it. Reads look in the loose
directories first and in `objects/pack` after that, because `git fetch` run
against this store leaves its objects packed, and a reader that only knew the
loose form could not read back what it had just fetched.

`gc` packs: it writes the reachable objects into one pack, named
`pack-<sha1 of the pack>.{pack,idx}`, then removes the loose copies the pack now
duplicates. It writes the whole reachable set each time rather than only the
objects that are not packed yet, so the store settles at one pack instead of
gaining one per gc; the pack that leaves behind is deleted when every object in
it is in the new one. A pack holding *anything* unreachable is left alone --
those objects may be all a rewritten branch or a fetch left of them, and no
other copy exists to read them from. Unreachable loose objects are pruned once
they are older than two weeks, and anything younger is left alone -- packing it
would keep it forever, which is the opposite of what a grace period is for.

## 1a. Packfiles

Both files git writes are read, and only version 2 of the index is written.

**`.pack`** — `"PACK"`, version 2 as a 4-byte big-endian integer, the object
count likewise, then each object's header and deflated data in offset order,
then the SHA-1 of everything before it.

An object header is a varint whose first byte carries the type in bits 4-6
(`1` commit, `2` tree, `3` blob, `4` tag, `6` offset delta, `7` reference
delta) and the low 4 bits of the inflated size; each continuation byte adds 7
more bits, least significant first.

A delta is two varints, the source size and the target size, then copy and
insert commands:

```
0x80  copy: four optional little-endian offset bytes (bits 0-3) then three
      optional little-endian size bytes (bits 4-6); a size of zero means
      0x10000
0x01-0x7f  insert that many literal bytes
0x00  reserved
```

An offset delta names its base by distance back in the same pack, encoded as a
big-endian varint where each continuation adds one; a reference delta names it
by id, and the base may then be in any pack or loose. Chains are followed
recursively, and a chain deeper than 200 is refused as corrupt.

**`.idx`** — version 2: the magic `\xff t O c`, version 2, a 256-entry fanout
table of cumulative big-endian counts by first id byte, the sorted 20-byte
ids, their crc32 over the object's bytes in the pack, and their 4-byte
offsets. An offset with its top bit set is not an offset but an index into the
8-byte overflow table that follows. Then the pack's checksum and the index's
own.

Version 1 of the index is also read: the fanout, then `(offset, id)` pairs.
git writes v2 today, but a repository old enough to hold a v1 index is exactly
the kind this should not refuse. `.rev` files are ignored.



The four types:

**blob** — the payload is the file contents, verbatim, no transformation.
Prompts are blobs.

**tree** — a concatenation of entries, each

```
<mode> <name>\0<20 raw bytes of the object id>
```

with `mode` in ASCII octal and no leading zero: `100644` for a regular file,
`100755` for an executable, `120000` for a symlink, `40000` for a subtree
(note: `40000`, not `040000`). Entries are sorted by name, with a subtree
compared as though its name ended in `/`. Subtrees are ordinary trees, not
commits, and carry no id of their own.

**commit** — a header block, a blank line, then the message:

```
tree <40 hex digits>
parent <40 hex digits>          (zero or more)
author <name> <email> <epoch> <±hhmm>
committer <name> <email> <epoch> <±hhmm>
gp-session <session id>         (optional; gitprompt's own header)
gp-prompt <prompt id>           (optional, repeated; gitprompt's own header)
<blank line>
<message>
```

`gp-session` and `gp-prompt` are the headers git does not define. `gp-session`
names the prompting session the commit was made in. Each `gp-prompt` names a
prompt the commit carries, in `seq` order, so that the commit is the join
between the code it adds and the prompts that produced it: reading the commit
alone is enough to know which prompts to replay to get that code back. git
ignores unknown headers, so a commit carrying them is still an ordinary
commit: `git log`, `git fsck` and `git clone` all accept it.

The prompts a commit carries are the prompt files it adds or changes, as the
first parent's tree tells them apart -- a commit of code with no new prompt
carries none, and a prompt edited after it was recorded is carried by the
commit that carried the edit, since it is the prompt file in the commit's own
tree that decides, not the working tree. A root commit carries every prompt it
holds, having no parent to differ from. There is no `gp-prompt` on the session
file or the response file: they are not prompts, and only the prompt files
directly under the prompt directory are read this way.

**tag** — an annotated tag: `object`, `type`, `tag`, `tagger`, blank line,
message.

Three or more parents are representable and are read back correctly, though
only `merge` writes a second one.

## 2. Refs

| path | contents |
| --- | --- |
| `refs/heads/<name>` | a branch |
| `refs/tags/<name>` | a tag (may point at a tag object or straight at a commit) |
| `refs/remotes/<remote>/<name>` | the last known position of a remote branch |
| `HEAD` | `ref: refs/heads/main`, or a raw id when detached |
| `logs/<ref>` | the reflog for that ref |
| `packed-refs` | read, never written |

Ref files hold 40 hex digits and a newline. A reflog line is

```
<old id> <new id> <name> <email> <epoch> <±hhmm>\t<message>
```

`packed-refs` is read so that a repository which has been through `git pack-refs`
still resolves, but gitprompt never writes one.

Ref names are validated on creation: components may not begin with `.`, may not
end in `.lock`, may not contain `..`, `~`, `^`, `:`, `?`, `*`, `[`, `\`, space,
or control characters, and may not end in `/` or `.`.

## 3. The index

`.gitprompt/index`, in git's binary index version 2 format — `DIRC`, a version,
a count, then one fixed-size record per entry:

```
ctime seconds, ctime ns, mtime seconds, mtime ns   (4 bytes each, big-endian)
device, inode, mode, uid, gid, size                (4 bytes each)
20 raw bytes of the object id
2 bytes of flags: bits 0-11 the path length, bits 12-13 the merge stage,
                  bit 15 assume-valid
the path, NUL-terminated, the record padded with NULs to a multiple of 8 bytes
```

then the whole buffer's SHA-1. All integers big-endian.

Records are written sorted by path, bytewise, and within one path by stage,
whatever order they were staged in: git refuses to read an index whose entries
are not in path order ("unordered stage entries"), and paths arrive in staging
order, not sorted order.

The file is at `.gitprompt/index` rather than `.git/index`, so a gitprompt
repository and a git repository can share a working tree.

**Unmerged stages are the record of a conflict**, exactly as in git. A merge
that conflicts leaves no stage-0 entry for the path; it leaves stage 1 as the
merge base, stage 2 as our version and stage 3 as theirs, and leaves out any
side that has no version of the file at all — a path one side deleted gets two
entries, not three. `gitprompt status` reads the letters `UU`/`AA`/`DU`/`UD`
straight off which stages are present, and so does `git` reading the same
index: `git ls-files -u` and `git status --short` report the same conflict
gitprompt does. Staging a path writes its stage-0 entry and drops the others,
which is how a conflict is declared resolved; until that happens
`gitprompt commit` refuses, and `gitprompt write-tree` and `gitprompt diff
--cached` have no tree to write from and refuse too.

## 4. Prompt files

A prompt is a blob at `prompts/<NNNN>-<slug>.md`.

- `NNNN` is a zero-padded decimal counter, a *repository-wide* monotone
  sequence. It is also written into the file as `seq`. It is what makes the
  ordering of a prompt history recoverable without reading any commit.
- `<slug>` is the first 40 characters of the prompt text, lowercased, with runs
  of non-alphanumeric characters collapsed to single hyphens and leading and
  trailing hyphens removed. If that yields nothing, the slug is `prompt`.

The file is a frontmatter block, then the prompt body:

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
parent_prompt: p_yyyyyyyy
attachments: [design.png]
---
Build a login page with email and password fields.
```

Every key except `id`, `session`, `seq`, `timestamp` and `author` is omitted when
empty. `session` is written as `-` when the prompt belongs to no session, so the
line is always present and column-stable. `seq` and `timestamp` are the two
fields that make the file self-describing: a file read in isolation still knows
where it belongs in the sequence and when it was written.

A `timestamp` — and a session's `started_at` and `ended_at` — is the wall clock
at a written offset, `YYYY-MM-DDTHH:MM:SS±HH:MM`, with the offset the date was
given in. The clock and the offset describe one instant together, so a reader
must apply the offset, not the offset where the reader happens to be: reading
`10:00+08:00` as ten in the morning local time lands eight hours from what was
written on a machine that keeps UTC. A date written without an offset is the
writer's local time and is stored with the writer's offset; a bare day is
midnight that way. `Z` is accepted and written back as `+00:00`. The epoch with
an offset after it (`1700000000 +0800`), which is what a commit object carries,
is accepted wherever a date is.

Both `tags` and `attachments` are inline bracketed lists, comma-separated.

What an agent answered the prompt is not in this file: it is an object of its
own, named after this prompt's id (§6).

## 5. Session files

A session is a blob at `prompts/sessions/<id>.md`. It has frontmatter and no
body:

```
---
id: s_1790354733_42fkw2
title: Scaffold the login page
started_at: 2026-09-26T00:31:02+08:00
ended_at: 2026-09-26T01:14:47+08:00
author: Your Name <you@example.com>
notes: Session context was reset twice.
---
```

`id` and `title` are always present. A session started with `-t` keeps the
title it was given. One started without one is named after the first prompt
recorded into it -- that prompt's first line, runs of blanks collapsed and cut
to 60 characters -- written at the moment the prompt is recorded, so the file a
clone reads says what the conversation was about rather than leaving the one
line that answers that question empty. Later prompts never rename it. Until the
first prompt arrives the title is the placeholder `untitled session`.
`ended_at` and `notes` appear only once set. `ended_at` present means the session
is closed.

A session that was left and returned to carries a `segments` line as well:

```
segments: 2026-09-10T09:00:00+08:00..2026-09-10T12:00:00+08:00, 2026-09-10T15:00:00+08:00..
```

It is one `start..end` pair per stretch, comma-separated, each end written as a
date in the same form the other fields use and omitted while that stretch is
still the open one. `segments` appears only when there is more than one, so a
session that was never left has the plain `started_at`/`ended_at` pair and
nothing else, and a file with no `segments` and a file written before this field
existed are the same thing.

Where it is present it is authoritative: `started_at` is its first start and
`ended_at` is its last end, absent while that stretch is open, and a reader that
knows nothing of `segments` still gets the session's overall span from those two.
A writer derives the pair from the stretches rather than writing it separately,
so the two cannot disagree in a file gitprompt wrote.

A session is left either by `session end` or by another session being started or
used, and the stretch that ends is closed at the instant the next one opens, so
consecutive stretches meet exactly and no time is counted twice or lost.

Session ids are `s_<epoch>_<6 characters>`; prompt ids are `p_` and response
ids are `r_`, each followed by 8 characters, where the characters are drawn from
`a`–`z0`–`9`. Uniqueness comes from the epoch, the process id and an in-process
counter hashed together — not from a cryptographic random source, because ids
only have to distinguish prompts within one repository.

A file's prefix says which it is: `p_` is a prompt, `s_` is a session, `r_` is a
response. Nothing else needs to be inspected to tell them apart.

## 6. Response files

A response is what an agent answered a prompt, and it is a blob at
`prompts/responses/<prompt id>.md` — named after the prompt it answers.

```
---
id: r_xxxxxxxx
prompt: p_yyyyyyyy
session: s_1790354733_42fkw2
timestamp: 2026-09-26T00:47:11+08:00
model: claude-sonnet-5
---
Created login.html. Validation misses empty input.

I put the check in the submit handler, so a paste into the field still slips
through.
```

The body is the answer's text, verbatim, and is everything after the closing
`---`; the frontmatter carries only what is needed to place it. `prompt` is the
id of the prompt being answered and is what the two are joined on — reading
either side alone is enough to know the answer belongs to that prompt, and the
file's name repeats it so that an answer can be found without opening every
file. `session` is copied from that prompt, so an answer is placed in a
conversation even when the prompt it belongs to is not at hand. `model` names
the agent that answered and is omitted when it is not known. The form is the
prompt file's, with the same rules: `timestamp` is the wall clock at a written
offset (§4), and `session` is written as `-` when there is none.

Being a separate file is the point of the design, and it is not a formatting
convenience:

- A prompt file is text somebody may have written by hand. There is no
  delimiter inside it that could be trusted to mean "the answer starts here",
  so an answer cannot live in it.
- The two arrive at different times. The prompt is recorded as it is said; the
  answer exists only once an agent has replied, and may never.
- It is a tree of ordinary files, so a clone, a push and a `checkout` carry the
  answers with the prompts, and `git --git-dir=.gitprompt ls-tree` shows them
  (§8).

Re-answering replaces the file rather than adding a second one, since the name
is the prompt: a prompt has one answer at a time, and the last one recorded is
the one the history shows. An answer whose prompt is not in the history is
ignored, the way a session no prompt names is not shown.

## 7. Repository state files

Inside `.gitprompt/`:

| file | meaning |
| --- | --- |
| `config` | git's INI syntax; `[core] repositoryformatversion = 0`, `[gitprompt] promptDir = prompts`, and `[remote "<name>"] url = ...` |
| `SESSION` | the id of the current prompting session, or absent when none is open |
| `gitprompt-seq` | the last prompt sequence number handed out |
| `MERGE_HEAD` | the id of the revision being merged in; present only during an unfinished merge |
| `MERGE_MSG` | the message the concluding commit should default to |
| `COMMIT_EDITMSG` | the buffer an editor was given, left behind afterwards as git leaves it |
| `RERUN_MSG` | the prompt being handed to an agent during `rerun`, and removed when the run ends |

`RERUN_MSG` exists only while `rerun` is running and is not part of the history:
it is how a prompt reaches an agent's standard input without being put on a
command line, where its quotes and newlines would have to be escaped. A run
killed part way leaves it behind, and the next run overwrites it.

`MERGE_HEAD` is what makes an unfinished merge a fact on disk rather than a
matter of memory: `merge --abort` uses it to restore the tree, the commit that
concludes the merge uses it to record the second parent, and a second `merge`
refuses while it exists. It is written by `merge` on both the conflict path and
the `--no-commit` path, and removed by a successful concluding commit and by
`--abort`.

Which paths are still in conflict is not a file here but the index itself (see
§3): they are the paths with entries above stage 0. A path stops being one when
it is staged, which is the same thing `git add` does to declare a conflict
resolved, and `MERGE_HEAD` deliberately outlives that: the resolution is staged,
but the merge is not finished until it is committed.

## 8. What a gitprompt repository looks like to git

```
$ git --git-dir=.gitprompt log --oneline
$ git --git-dir=.gitprompt ls-tree -r --name-only HEAD
prompts/0001-write-a-tokenizer-first.md
prompts/responses/p_yyyyyyyy.md
prompts/sessions/s_1790354733_42fkw2.md
$ git --git-dir=.gitprompt show HEAD:prompts/0001-write-a-tokenizer-first.md
```

All of it works, unmodified, because there is nothing special to read: the
prompts are blobs in trees at mode `100644` and the history is ordinary commits.
A conflicted index is git's too — `git ls-files -u` in a gitprompt store lists
the same stages, for the same paths, that `gitprompt status` reports — and a
merge stopped half-way is the `MERGE_HEAD` git itself would have left (§7).
