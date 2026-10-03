# How it is tested

The three suites, what each one is for, and what the two of them have
caught.

## The numbers

The end-to-end suite passes: **1039 checks, 0 failures** — 909 in
`test/smoke.sh`, 94 in `test/surface.sh` and 36 in `test/restore.sh`.

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
can be made from, a change traced back to the prompt that asked for it -- line
by line with `blame`, hunk by hunk with `--prompt-hunks`, and the commit-level
fallback for a prompt recorded without a snapshot -- `status` and `diff` on a
move, `clean` in each of its three modes together with the collapse of a
directory it reports whole, the pathspec that decides where that collapse lands,
and the store it will not touch however many `-f` it is given -- the commit
editor, finding lines with `grep` in the work tree, in the
index and in a revision, the pattern syntax under each of the default, `-E` and
`-F`, and the three exit statuses, halving a range with `bisect` — the order it
probes in, a probe that cannot be judged, a script judging for it, and a refusal
to start over local changes — per-command option validation, that the
replay plan and
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
produces. `bisect` is compared as the walk it is rather than as its answer: the
same eight commits are halved by both, and the probes have to come back in the
same order with the same widths. `clean` is compared on one fixture built to
give each branch of its decision something to decide, in each mode and under
each shape of pathspec and `-e`, with each tool run on its own copy so that the
store each one skips is its own. What needs git itself is skipped rather than
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
