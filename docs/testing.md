# How it is tested

The three suites, what each one is for, and what the two of them have
caught.

## The numbers

The end-to-end suite passes: **1549 checks, 0 failures** — 1361 in
`test/smoke.sh`, 152 in `test/surface.sh` and 36 in `test/restore.sh`.

```console
$ make test
```

`test/smoke.sh` covers the object model, sessions and prompts, the dates a
prompt and a session can be given, a task recorded in sessions that were
interleaved and returned to, committing, reconstruction (ordering, session
boundaries, and the flat chronology), the plan `rerun` would execute, the agent
conversations it maps sessions to, the command line a machine gives an agent with
`gitprompt.agent.<name>.<field>` — including an agent the table never heard of,
a piece emptied away, and a program that is not on `PATH` — and what `response`
and `rerun --record` keep
and how every rendering shows it, branches, tags, history editing, the reflog of
where HEAD has been, the commits a range of revisions reaches, merges
including conflicts
and `--abort`, merges that follow a file that moved, commits replayed with
`cherry-pick`, `rebase` and `revert` including their conflicts, empty results and
`--continue`/`--skip`/`--abort`, work set aside and put back with `stash`
including the untracked files, the index kept, a clash and the entry a branch
can be made from, a change traced back to the prompt that asked for it -- line
by line with `blame`, hunk by hunk with `--prompt-hunks`, and the commit-level
fallback for a prompt recorded without a snapshot -- `status` and `diff` on a
move, `clean` in each of its three modes together with the collapse of a
directory it reports whole, the pathspec that decides where that collapse lands,
and the store it will not touch however many `-f` it is given -- what the ignore
file means, rule by rule: a glob matching a name at any depth, the anchoring a
slash gives a pattern, `**` standing for zero directories as happily as for
three, `!` putting a path back and failing to put back a file whose directory is
already out, a deeper file overriding a shallower one, a trailing space not
being part of the pattern, and a tracked path never being ignored however well
its name fits -- the paths `add` refuses for being named while ignored, in the
wording git uses, and `-f` staging them instead -- the commit
editor, finding lines with `grep` in the work tree, in the
index and in a revision, the pattern syntax under each of the default, `-E` and
`-F`, and the three exit statuses, halving a range with `bisect` — the order it
probes in, a probe that cannot be judged, a script judging for it, and a refusal
to start over local changes — per-command option validation, that the
replay plan and
a recorded date read the same from any clock, local remotes, serving
over `gp://`, packed object stores, and git interoperability — the last being
the section that matters most, since a gitprompt repository is meant to be an
ordinary git repository. As part of it, `git verify-pack` checks the packs `gc`
and `repack` write against git's own index, `git ls-files` checks the index
gitprompt wrote against git's own reader, and `git shortlog` is diffed against
`gitprompt shortlog` line for line, which is the one summary command whose
output is git's byte for byte. A tree is handed out with `archive`, and what is
checked is that `tar` and `unzip` read what it wrote rather than that gitprompt
does: the entries come back with their contents, a prefix lands on every name,
the archive of a tree is the archive of the commit that points at it, and an
unreadable option or an unknown format is refused. A note is put on a commit, a
blob, and an object that is about to be collected: the note reads back, the list
names the object it belongs to, the ref over it is a commit whose tree holds the
note under the object's own name, an overwrite is refused without `-f`, the note
on the doomed object survives `gc` and is dropped by `notes prune` once nothing
is left to hang it on, the ref outlives the last note removed, an editor is
driven to write and then to empty one, and a message nobody gives and a
subcommand nobody has are both refused. A patch is applied to each of the
three layers in turn: a work tree it changes and an index it leaves alone, an
index `--cached` touches and a work tree it does not, and both together under
`--index`. A hunk is found after the file has moved on above it; a hunk whose
preimage is gone fails, and leaves both the file it failed on and an earlier
file in the same patch untouched, since a patch that cannot be applied whole is
not applied at all. A patch creates a file, deletes one and renames one, and a
mode change lands in the index. `--check` says a patch would apply and writes
nothing. A path that climbs out of the work tree is refused, and `--unsafe-paths`
anchors it inside rather than letting it above. A binary patch, a combined diff,
input that is not a patch and input that is empty are each refused. A second
working directory is made, listed, worked in, locked, moved and taken away: what
a linked one is made of is checked piece by piece — the `.git` file and the
registration it names, the `commondir` back to the store, and a HEAD and an
index of its own with no second store beside them — a commit made in one is read
from the other because the objects and the refs are shared, while the conflict a
merge stops on in one is not a merge in the other because that state is the
directory's, the branch one directory holds is refused in the second place that
would check it out, and a directory with uncommitted work in it is not removed
until `-f` says to. A repository nested in another is added, committed, cloned
and brought back: the gitlink in the tree and the entry in `.gitmodules` are
checked against the store the path points at, the submodule reads as a
repository of its own in its own directory, a clone carries the record and not
the checkout until `update` runs, `--remote` follows the submodule's branch
where a plain `update` stays on the commit the parent recorded, an update over
the submodule's own uncommitted work is refused until `-f`, `sync` puts back a
url that has drifted from `.gitmodules`, `foreach` runs with the submodule's
directory as its cwd and its name, path and commit in the environment and
leaves no script behind, and `deinit` refuses a submodule that holds either a
changed tracked file or one its index never had, clearing it only under `-f`
while its store stays. A work tree is told to hold only part of the index: the
paths a pattern list names are the ones on disk, the ones it leaves out are
taken away and marked in the index so that `ls-files -t` calls them `S` where a
path kept is `H`, `status` is clean and `commit -a` leaves their entries alone
rather than reading their absence as a deletion, a branch switch carries the
same sparse state across, a later line takes an earlier pattern back, `init`
writes the pattern list git's own `init` writes, and `disable` puts every path
back and removes the switch from the configuration.

`test/surface.sh` asks the other question: not whether each command is right in
depth, but whether the whole surface still is when the commands are used in the
order a user meets them. Every check is the operation git does — commit,
branch, merge, tag, describe, reset, mv, rm, checkout `--`, clone, push, pull —
run against a history of prompts, and each has to produce the thing git
produces. `bisect` is compared as the walk it is rather than as its answer: the
same eight commits are halved by both, and the probes have to come back in the
same order with the same widths. `clean` is compared on one fixture built to
give each branch of its decision something to decide, in each mode and under
each shape of pathspec and `-e`, with each tool run on its own copy so that the
store each one skips is its own. The ignore rules are compared the same way, on
a work tree holding a `.gitignore` and no `.gitpromptignore` at all, so that the
file both tools read is the same file: the untracked list, the words and the
exit status `add` gives back for a path named while ignored, the set `-f` stages
instead, and `clean` agreeing in each of its three modes and under `-d`. What
needs git itself is skipped rather than
faked when git is not on PATH. `archive` is compared on one tree: `git archive`
and `gitprompt archive` are each written out and extracted, tar and zip alike,
and the two sets of files have to be the same files — which is what makes the
container an ordinary one and not a gitprompt format that happens to look like
a tar. `notes` is compared the way it has to be for a note to be worth writing:
one tool writes a note and the other reads it, in both directions, against the
same repository — `git notes show` reading what `gitprompt notes add` wrote, and
`gitprompt notes show` reading what `git notes add` wrote — with the two tools'
lists having to name the same pair. `apply` is compared as the text it is: the
patch `gitprompt diff` writes is handed to `git apply` and the patch `git diff`
writes is handed to `gitprompt apply`, over an edit and over a rename, in both
directions, and the two tools have to leave the same work tree behind. `worktree`
is compared as the layout it is: the registrations are read back by git's own
reader with `git --git-dir=<store> worktree list`, and the two listings have to
hold the same rows once the path column — which is padded to different widths
and names the store differently — is taken off, a locked registration included,
which is what makes the registration an ordinary one rather than a gitprompt
format that happens to look like git's. `submodule` is compared on the two
artifacts that have to be the other tool's: the gitlink and the `.gitmodules`
are written by gitprompt and read by git — `git ls-files -s`, `git config -f`
and `git submodule status` all on gitprompt's output — and, the other way, a
commit git makes is pushed into a gitprompt store and checked out by
`gitprompt submodule add`, which is the sharpest form of the claim that a
gitprompt store is an ordinary git object store. `sparse-checkout` is compared
through the file the two tools share, in both directions: `git ls-files -t` and
`git sparse-checkout list` read the bit and the pattern list gitprompt wrote,
and `gitprompt ls-files -t`, `gitprompt sparse-checkout list` and a `status`
read what `git sparse-checkout set` wrote — which is the check that means
something, since the skip-worktree bit lives in a second flags word that only
version 3 of the index has room for, and a reader that assumed version 2 would
take those two bytes for the first two letters of the path. Two
things `smoke.sh` did not catch were found here — a merge that carried
no prompts, and a reflog that forgot the past after a checkout — which is what
made it worth keeping rather than folding in.

`test/restore.sh` runs the design claim end to end. It builds a history that
crosses two sessions and returns to the first, records it out of clock order,
pushes it to a bare git remote, and restores it on another machine through a
plain `git clone` with no gitprompt store in it: adopting the clone, comparing
the plan it would replay against the one the original would, and handing the
prompts to a stub agent — one at a time, in the order they were written, into
the conversations they came from — with the same replay planned under any clock.
