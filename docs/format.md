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

Objects are always written loose. There is no packfile support; `gc` removes
unreachable loose objects but does not pack them.

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
<blank line>
<message>
```

`gp-session` is the one header git does not define. It names the prompting
session the commit was made in. git ignores unknown headers, so a commit
carrying it is still an ordinary commit: `git log`, `git fsck` and `git clone`
all accept it.

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
a count, then one fixed-size record per path:

```
ctime seconds, ctime ns, mtime seconds, mtime ns   (4 bytes each, big-endian)
device, inode, mode, uid, gid, size                (4 bytes each)
20 raw bytes of the object id
2 bytes of flags: the low 12 bits hold the path length
the path, NUL-terminated, the record padded with NULs to a multiple of 8 bytes
```

then the whole buffer's SHA-1. All integers big-endian.

The file is at `.gitprompt/index` rather than `.git/index`, so a gitprompt
repository and a git repository can share a working tree.

**Unmerged stages are not used.** git represents a conflict by storing three
entries for one path, distinguished by the stage bits in the flags. gitprompt's
index is a plain path-to-object map with no stage field, so a merge in progress
is recorded in `MERGE_CONFLICTS` instead (see §6). A path's conflicted state is
therefore invisible to `git` reading the same index.

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

Both `tags` and `attachments` are inline bracketed lists, comma-separated.

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

`id` and `title` are always present; `title` falls back to `untitled session`.
`ended_at` and `notes` appear only once set. `ended_at` present means the session
is closed.

Session ids are `s_<epoch>_<6 characters>`; prompt ids are `p_` followed by
8 characters, where the characters are drawn from `a`–`z0`–`9`. Uniqueness
comes from the epoch, the process id and an in-process counter hashed together
— not from a cryptographic random source, because ids only have to distinguish
prompts within one repository.

A file's prefix says which it is: `p_` is a prompt, `s_` is a session. Nothing
else needs to be inspected to tell them apart.

## 6. Repository state files

Inside `.gitprompt/`:

| file | meaning |
| --- | --- |
| `config` | git's INI syntax; `[core] repositoryformatversion = 0`, `[gitprompt] promptDir = prompts`, and `[remote "<name>"] url = ...` |
| `SESSION` | the id of the current prompting session, or absent when none is open |
| `gitprompt-seq` | the last prompt sequence number handed out |
| `MERGE_HEAD` | the id of the revision being merged in; present only during an unfinished merge |
| `MERGE_MSG` | the message the concluding commit should default to |
| `MERGE_CONFLICTS` | the paths left in conflict, one per line |

`MERGE_HEAD` is what makes an unfinished merge a fact on disk rather than a
matter of memory: `merge --abort` uses it to restore the tree, the commit that
concludes the merge uses it to record the second parent, and a second `merge`
refuses while it exists. It is written by `merge` on both the conflict path and
the `--no-commit` path, and removed by a successful concluding commit and by
`--abort`.

`MERGE_CONFLICTS` is gitprompt's substitute for the index's three unmerged
stages. A path leaves the file when it is staged, which is the same thing `git
add` does to declare a conflict resolved, and `MERGE_HEAD` deliberately outlives
it: the resolution is staged, but the merge is not finished until it is
committed.

## 7. What a gitprompt repository looks like to git

```
$ git --git-dir=.gitprompt log --oneline
$ git --git-dir=.gitprompt ls-tree -r --name-only HEAD
prompts/0001-write-a-tokenizer-first.md
prompts/sessions/s_1790354733_42fkw2.md
$ git --git-dir=.gitprompt show HEAD:prompts/0001-write-a-tokenizer-first.md
```

All of it works, unmodified, because there is nothing special to read: the
prompts are blobs in trees at mode `100644` and the history is ordinary commits.
The only thing git cannot see is the state in §6, which lives outside the object
store.
