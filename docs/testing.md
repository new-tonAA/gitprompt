# How it is tested

The three suites, what each one is for, and what the two of them have
caught.

## The numbers

The end-to-end suite passes: **1138 checks, 0 failures** — 994 in
`test/smoke.sh`, 108 in `test/surface.sh` and 36 in `test/restore.sh`.

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
and `repack` write against git's own index, and `git ls-files` checks the index
gitprompt wrote against git's own reader.

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
faked when git is not on PATH. Two things `smoke.sh` did not catch were found here — a merge that carried
no prompts, and a reflog that forgot the past after a checkout — which is what
made it worth keeping rather than folding in.

`test/restore.sh` runs the design claim end to end. It builds a history that
crosses two sessions and returns to the first, records it out of clock order,
pushes it to a bare git remote, and restores it on another machine through a
plain `git clone` with no gitprompt store in it: adopting the clone, comparing
the plan it would replay against the one the original would, and handing the
prompts to a stub agent — one at a time, in the order they were written, into
the conversations they came from — with the same replay planned under any clock.
