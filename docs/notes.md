# Notes

The decisions behind individual commands, and the places where the
agreement with git is not exact.

## Following a file that moved

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

## Tracing a line back to its prompt

A commit binds code to prompt, but only as a whole: the commit says which
prompts it carries, not which block of the change each one asked for. Two
prompts that touch the same file are one diff, and no reading of the result
recovers the split. So gitprompt writes it down as the prompts are given: each
prompt file records, in its `snapshot:`, the work tree as it stood **before**
that prompt's change. A run of prompts is then a chain of states — the parent's
tree, the first prompt's snapshot, the second's, up to the commit's own tree —
and the step between two of them is one prompt's work. The last prompt's step
ends at the commit's tree.

None of this is inferred. A commit whose prompts predate snapshots, or whose
snapshots are gone, has no chain, and the tools say so rather than guess.

`gp blame <file>` reads the chain back line by line. It walks the file the way
git's blame does — diffing each version against its parent, carrying unmatched
lines back — and lands every line on the commit that introduced it; inside that
commit it lands the line on the block whose step added it, and names the prompt.
The first column is the prompt, the second the abbreviated commit, then the line
number and the text. A prompt id with a `?` is the commit's as a whole, because
that commit keeps no snapshots and the block behind the line is not known, and a
`-` means no prompt claims the line at all.

`gp show --prompt-hunks` and `gp diff --prompt-hunks` annotate the ordinary diff:
before each `@@` hunk header comes a `prompt <id>` line naming the prompt whose
step it belongs to, or `prompt (none)` for a step no prompt owns. The prompt
files themselves never appear — a prompt file is not in the index, so it cannot
be in a snapshot, and its block would say nothing about the code.

`gp show <prompt-id>` completes the picture from the other side: it lists every
commit that carries the prompt, oldest first, as `<abbrev> <date> <subject>`.

Snapshots are tree objects, so they cost what any tree costs and share the
objects they have in common with their neighbours. A tree named only by text in
a prompt file would otherwise be unreachable, and `gc` would prune it once it
was older than the grace period — so both `fsck` and `gc` read the prompt files
they walk past and treat the snapshot a prompt names as a root. What that buys
is the one thing the feature cannot do without: a snapshot a user can still
check out from after a `gc`.

## The commit message, when there is no `-m`

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

## Summarising the log by author

`shortlog` groups commits by the name on the author line, and the grouping key
is the name *alone* unless `-e` is given — two commits made by the same person
under two addresses are one group by default and two groups with `-e`, which is
how the command tells a contributor who has committed from two machines. The
block form lists each group's subjects oldest first, because git reverses the
walk it read; `-s` prints only the counts, `-n` orders the groups by size
instead of by name, and both orders are git's exactly, down to the six-wide
count column and the tab after it. That is why this is the one summary command
the suite diffs against real `git shortlog` line for line rather than checking
its parts.

The name and the email have to come apart the same way everywhere, so
`parse_ident` in `src/object.c` is the single place that does it: `shortlog`
groups on its halves, and `log`'s `Author:` line is rendered from them, so the
two cannot disagree about where a name ends.

## A merge is three-way, line by line

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

## A replay is a merge read the other way

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
replay puts `sequencer/` (§7 of [format.md](format.md)), and `commit`
refuses while either is there: an interrupted replay is finished by
`cherry-pick`/`rebase --continue`, `--skip` or `--abort`, not by committing by
hand.

## An undo is that same merge, the other way round

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

## A conflict answered once is answered from then on

A conflict met twice costs two people the same typing twice, which is what
`rerere` is for: the first time a conflict is settled, what the file became is
written down, and the next time the same two sides are met the answer is put
back. The record is keyed by the conflict and not by the path — by a hash of the
two sides' bytes — so two files that met the same conflict share one answer, and
a file that was renamed between the two meetings is still recognised. The labels
(`<<<<<<< HEAD`, `>>>>>>> side`) are stripped before the hash for the same
reason: they name where each side came from, which is exactly what differs
between two meetings of one conflict, while the sides' own text is what is the
same. What the entry holds is the conflict as it was written (`preimage`), the
file as it was left once settled (`postimage`), and the text it originally
carried (`thisimage`).

Putting an answer back is not substituting text into a diff. The recorded answer
begins with the text that came before the first conflict and ends with the text
that came after the last, so both are required to be there, byte for byte; the
stretches between conflicts are then looked up in the answer, and the bytes
skipped over on the way are that conflict's resolution — a file with two
conflicts is answered as two. The last one has nothing after it to search for,
so it is measured from the end instead. If any of that does not line up the tool
gives up and files the conflict afresh rather than guessing, which is what keeps
a resolution from being applied to text it was not written for.

The hook is one place, `merge_trees_labeled`, which is the single path every
merge in this tree goes through — `merge`, `pull`, `rebase`, `cherry-pick`,
`revert` and `stash pop` all read their three trees there. That is the payoff of
[A replay is a merge read the other way](#a-replay-is-a-merge-read-the-other-way):
because there is no second merge implementation to teach, one hook covers all of
them. The conflict is filed as the file is written, before the index records the
unmerged stages, so what is recorded is the file the user is looking at.

An answer put back is still a conflict: it is left in the file unmerged, to be
read before it counts, which is what makes this a way of typing less rather than
of deciding for anyone. `rerere.autoupdate` stages it as well, so a run that
finds a remembered answer can be committed without opening the file — but even
then the merge stops where it stopped, with `MERGE_HEAD` written and a non-zero
exit, and waits for `commit`. A merge that met a conflict is a merge that has to
be concluded, whether or not the text of the conflict is still in front of you.

## Setting work aside

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

## An option belongs to its command

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

## Finding a line without a regex library

`grep` needs a pattern engine and there is none to borrow: the compiler this is
developed with ships no POSIX regex for C, and a borrowed one would behave
differently on each of the three runners besides. So `src/regex.c` is the
engine — a parser that compiles a pattern into a tree, and a backtracking
matcher that walks it — written to the contract git's patterns are already
written against: basic by default, extended under `-E`, literal under `-F`, with
`+ ? | ( )` ordinary bytes in a basic pattern and their backslashed forms the
operators.

Which of the three stores is read is the other half of the command, and the same
half in every case: the work tree by default, the index under `--cached`, and a
revision's tree for a `<rev>`, whose lines are printed with the revision as it
was typed in front of the path.

What it cannot do it refuses at compile time and exits 128 rather than answering
something else, and the refusal is the design rather than a gap: interval
expressions `{n,m}`, back references `\1`, the POSIX classes `[[:alpha:]]`, the
word boundaries `\<`, `\>`, `\b` and `\B`, and a repetition of an expression
that can match nothing (`(a*)*`, the one construct that can hang a backtracking
matcher) are errors here where git matches them. git's other options —
`-A/-B/-C`, `-P`, `--and`/`--or`/`--not`, `--untracked`, `-o`, `-H` — are
refused as unknown options rather than quietly ignored.

Two smaller differences. A file with a NUL in it is not text and is passed over
in silence, where git prints `Binary file <path> matches` and counts that as a
hit, so the exit status differs for such a file. And a usage error — no pattern,
an unknown option — exits 1 throughout gitprompt where git exits 128 or 129.
What is held exactly is the status the command exists to report: `0` when
something was printed, `1` when nothing was, `128` for a pattern that will not
compile or an argument that is neither a revision nor a path — which is git's,
and is the thing a script branches on.

## Halving a range

`bisect` is the one command whose answer is a commit but whose work is a
sequence of them, so what it prints on the way is as much of the contract as
where it stops. The range is `bad` less everything reachable from each `good`
and each `skip`, held as `refs/bisect/bad` and one `refs/bisect/good-<id>` or
`refs/bisect/skip-<id>` per commit — the shape git uses, which is what lets git
read a bisection gitprompt began, and the other way round. The probe is the
candidate nearest half way: each is given the number of its ancestors that are
themselves candidates, counted once per commit with a memo, and the one whose
count falls closest to half the range is checked out. A set that leaves two of
them equally far out is broken the way git breaks it, by object id.

On a linear history the walk is deterministic, and `test/surface.sh` asserts the
two are step for step identical there, counts and all; the commit finally named
is the same on any history. What can differ on a history with merges is which
commits were asked about, and — in the skip path, which is where git's sorted
`best_bisection` comes in — the count printed beside them, because the id that
breaks the tie is not gitprompt's.

Everything the command leaves on disk is the storage, and it is all written and
removed together: the refs under `refs/bisect/`, and the `BISECT_*` files §7 of
[`format.md`](format.md) lists. A start that cannot get as far as its first
probe takes them away again rather than leaving a range behind, so a refusal
over a dirty work tree is a refusal and nothing more — worth saying because
the range is written before the probe is tried, which makes this easy to get
wrong.

Three things it does not do exactly as git does:

- **A failed `bisect run` exits 1.** The console reports the same things, but
  the status the command itself exits with is git's own internal value, which
  differs by failure and by platform — 126 for a command that gave 128 or more,
  2 for a range with nothing left to test — where gitprompt answers 1 for every
  way it can fail. What is not skimped is the check in front of it: 126 and 127
  are the shell's own statuses for a command it could not run, so before either
  is read as a verdict the command is run once more at a commit already known
  good, and a command that is broken is reported rather than allowed to narrow
  the range.
- **`visualize` prints the range.** git hands it to gitk, or to `git log
  --graph` when asked. gitprompt lists the commits still to be tested, newest
  first, with the headers `show` prints.
- **A staged change is refused in gitprompt's own words.** `cannot bisect: you
  have staged changes`, with the hint `rebase` gives for the same situation,
  where git lets `checkout` refuse and prints its message. A changed file in the
  work tree — a different case, and the more common one — is refused in git's
  words exactly, down to the list of paths and the `Aborting` that ends it.

## Taking out what the index does not know about

`clean` is the one command here whose work is deletion, so it asks two questions
of every path and refuses up front rather than printing a list and then acting on
it. The first is whether a path may go at all: the index is what makes a file
somebody's work, and a path the index holds is never a candidate. The second is
which candidates this run was asked for, and that is what the three modes choose
between — the untracked-but-not-ignored ones, all of them, or the ignored alone.
`-e` is a third attitude to the same question: an exclude rule that takes a path
out of the plain and `-x` modes and puts it into `-X`, which is why
`clean -nX -e plain.txt` names a file nothing had called ignored. Its pattern is
narrower than one in an ignore file — a whole basename or a whole path, with a
trailing slash meaning a directory and no globs at all — which is a divergence
from git and is listed in [What is not implemented](limitations.md).

A directory is reported whole when everything under it is going, so `-ndx` says
`Would remove allign/` rather than naming its files one at a time; the removal is
recursive either way and the shorter line is the truer one. The collapse is
decided by counting — every file under the directory, and the ones this run wants
— and the three answers are one line when they are all wanted, silence when none
are, and a walk into it when they are mixed. `-d` is what makes an untracked
directory a candidate at all, with two exceptions that are not arbitrary: a
directory holding something the index knows is always walked into, since its
untracked side is exactly what the run is about, and `-X` with no `-d` descends
into a mixed directory, because an ignored file under a directory that is not
itself ignored is reachable no other way.

A directory that is a repository in its own right is left alone however many `-f`
are given. What makes it one is not the name — a stray directory called `.git`
holding nothing but a config file is removed like any other — but the three
things a store has: a `HEAD`, an `objects/` and a `refs/`.

Which names count as ignored is decided where `status` and `add` decide it too;
see [What the ignore file means](#what-the-ignore-file-means). `-i` is
refused as an unknown option, and a pathspec is the path of the directory it
names rather than a wildmatch pattern, so `clean -nd -- a/b` collapses at `a/b/`
even though the only thing under `a/` is `b/` and everything under `b/` is going
too.

## What the ignore file means

A prompt repository collects what nobody meant to keep — editor droppings, build
output, a virtual environment — and the rules for that are git's, because a
repository that is an ordinary git repository will have arrived with a
`.gitignore` and because the rules people know are the ones git taught them.
Both files are read: `.gitignore` first, then `.gitpromptignore`, so inside one
directory the name this project uses has the last word. A file in a
subdirectory is read after the ones above it, so it has the last word over
them.

What the rules mean is mostly what they look like, but the parts that are not
obvious are the parts that get implemented wrong:

- A pattern with a slash in it, leading or in the middle, is anchored to the
  directory its file is in; `anch/q.txt` is not `x/anch/q.txt`, and `/root.txt`
  is not `sub/root.txt`. A pattern with no slash matches a name at any depth.
- A trailing slash means a directory, and a pattern naming a directory — with
  the slash or without it — takes everything under it along.
- `*`, `?` and `[...]` are globs within one path segment, so a star never steps
  over a directory. A double star that is a whole segment stands for any number
  of segments, zero included: `**/deep.txt` matches `deep.txt`, and
  `mid/**/end.txt` matches `mid/end.txt`.
- A leading `!` puts a path back, but only if nothing above it is already out:
  `out/` followed by `!out/` brings the directory back, while `dir/` followed by
  `!dir/keep.txt` cannot, because a file under an excluded directory is excluded
  by that directory and not by its own name.
- The last line in a file that mentions a path wins, and a deeper file wins over
  a shallower one. `\ `, `\#` and `\!` escape; unescaped spaces at the end of a
  line are not part of the pattern.

A path the index knows is never ignored, whatever the rules say about its name.
That is what git does — `git check-ignore` will not name a tracked path either —
and it is why the check lives inside the ignore layer rather than at each call
site: a file can be committed and only afterwards matched by a rule, a `.o` file
tracked before anyone thought to ignore build output, and a caller that asked
the rules alone would stop seeing it. The cost is that the layer needs the
index, which `index_get` answers by walking every entry; so the paths are read
once, sorted, and searched. Writing the index drops that copy, or a command that
wrote it and then walked the work tree would be told about the index as it was.

One more thing follows from the tracked-file rule, and it is why the walk does
not prune: a directory with a rule against it is still walked into when the
index holds something inside it, because `status` has to report the changes to
that file. `clean` descends into one for the same reason — it names
`build/new.txt` rather than folding the directory into a line, which is what git
does and what the suite compares it against.

`add` names the paths it refuses: `add build` on an ignored directory stops with
git's two lines — the paths, and the hint about `-f` — and `-f` stages what the
walk left out, following the spec itself since the walk never offered it. A
directory that is not itself ignored is not a refusal, and neither is `add .`:
only a path named outright is refused, so an ignored file never turns every
`add .` into an error. Git prints a third line offering
`advice.addIgnoredFile`; there is no such setting here, so it is not printed.

The edges are not implemented and are listed in
[What is not implemented](limitations.md): `[[:alpha:]]` character classes,
`core.excludesFile`, `.gitprompt/info/exclude`, `status --ignored` and
`clean -i`.

## The store

The pack reader is exercised against packs git wrote, not only against the ones
`gc` writes itself: `gc` and `repack` write whole objects, so a pack they made
has no deltas in it. Packs written by `git repack` and by `git pack-objects`,
with offset deltas and reference deltas, chains several deep, have been read
back object by object and re-hashed.

The packed-store section also pins down when a pack may be dropped. A second
`gc` of an unchanged store rewrites the same pack under the same name, and
deleting that as superseded would take every object with it; a pack holding an
object no ref reaches is kept, because an object a fetch left packed has no
loose copy to fall back on; and every reader — `replay`, `timeline`,
`log-prompt`, `stats` — is run against a packed store, since wanting an object's
contents without its type is a different path through the store from wanting
both.

## `repack` and `prune`, which are the two halves of `gc`

`gc` was one command doing two things: pack what the refs reach, and delete the
unreachable loose objects. Splitting it into `repack`, which only packs, and
`prune`, which only deletes, is what git's own two commands are, and each half is
now usable on its own — `repack` to pack without pruning the objects a run of
experiments just made, `prune` to drop them without rewriting the pack.

The halves keep `gc`'s two rules. `repack` never deletes an object: it packs the
reachable set (or, without `-a`, only the loose part of it, which is what a plain
`git repack` adds), and the loose copies it does remove are the ones the pack it
just wrote already holds. `prune` never touches a pack: an unreachable object
inside one is left where it is, and the way to reclaim it is `repack -A -d`,
which wrote the pack, followed by this. `repack -d` is the piece that removes the
pack a later repack supersedes, since that needs the list of what everything in
the old pack is now also in.

`--expire` takes `now` and `<n>.<unit>.ago`, and nothing else. That is what git
takes for the common cases and it is deliberately not a date parser: an
expression this cannot read is refused rather than guessed at, so `gc`'s
fourteen-day grace period (`repack`'s job to preserve, `prune`'s to apply) is
never quietly replaced by a number nobody asked for.

## Handing a tree out

`archive` writes the files of a tree into a tar or a zip, and the containers are
ordinary ones: the check that matters is that `tar -xf` and `unzip` read them,
and that the files they hold are byte for byte the files `git archive` of the
same tree holds. The question `archive` answers is not about the repository at
all — it is "give me the code as of this commit, as a file" — so it reads the
tree and writes a stream, and touches nothing.

The zip entries are stored, not deflated. A deflated entry needs a *raw* deflate
stream, and the only deflate in this tree is zlib's, which wraps its stream in a
zlib header that a zip reader will not accept; a stored entry is a format `unzip`
reads exactly. A zip from here is therefore larger than one `git archive`
writes, and that is the honest trade against writing an entry no reader would
take. The tar is ustar, which is the oldest and most widely read form of it and
the one `tar` on all three runners writes; it has a 100-byte name field, so a
path that does not fit is refused rather than written as the pax extension git
would add, since a long name is a thing to be told about rather than a thing to
find out about at extraction.

What is *not* written is the directory entries: `load_tree_flat` visits the
files of a tree and not its directories, so the tar has no `sub/` line before
`sub/file`, which is the one way its listing reads differently from git's. The
extracted files are the same either way; what a directory entry would also carry
is an empty directory, which git puts in the archive and this does not.

The entry timestamp is the commit's, not the file's: git stores no per-file
mtime, so the time in the header is the one thing about a tree that is a date,
and every entry gets it. The mode comes from the tree, so an executable file is
`0755` in the tar and a symlink is written as a symlink.

## A note is a blob with the object's name

A note in git is not a special kind of object and it is not an entry in any
index. It is an ordinary blob, in an ordinary tree, hanging off an ordinary
commit that a ref points at — and that is the whole of it. Which ref, which
name inside the tree, and what the blob holds is all convention, and the
convention is the format. `notes` is written to match it rather than to invent
a store of its own, because the point of a note is that the other tool can read
it.

The ref is `refs/notes/commits`. Its value is a commit — git writes a commit
whose message says a note was added or removed, and this does too, though the
message is the only part of that commit nobody reads. The commit's tree holds
one blob per annotated object, and the *name* of the blob is the object's full
40-character hex id. There is no extension and no suffix: the path
`ab12…ef` under the notes tree is the note for the object `ab12…ef`.

Git fans that path out once a notes tree grows large, putting the note for
`ab12…ef` at `ab/12…ef` — a directory named by the first two hex digits. Both
shapes are the same note; a reader that only knows one shape will miss notes the
other tool wrote. So the reader here takes either, and the writer always writes
the flat form: at the scale a person keeps notes, the fanout saves nothing, and
a tree that is flat is one somebody can read with `cat-file` and a hex id they
already have. A tree git has fanned out is still read correctly, and the next
`notes add` over one of those objects lifts that note back to the top level
rather than leaving a second copy behind — which is why the write removes both
paths before it adds one.

Removing the last note leaves the ref standing over an empty tree, rather than
deleting the ref. That is git's behaviour and it is the useful one: the
difference between "this repository has never had notes" and "this repository
had notes once and does not now" is a difference in what the ref is, not in
whether it is there, and keeping it means a second `notes add` does not have to
decide which of those it is.

`notes prune` is not `prune`. The first drops a note whose *annotated object*
has gone away — the note is intact, but there is nothing left for it to hang on,
and it is that object the garbage collector took, not the note. It walks the
notes tree, asks whether each object still exists, and rewrites the tree only if
some did not. `gitprompt prune` and `gitprompt gc` do not touch notes at all:
reachability is walked from the refs, `refs/notes/commits` among them, so an
ordinary prune cannot reach a note to delete it.

The editor that `notes edit` opens is the same one `commit` opens, from the same
configuration and the same command line, because "which editor this machine
uses" is one answer and not one per command. It is shared by name —
`repo_editor_command` and `repo_run_editor` — so that a `notes edit` and a
`commit` on one machine cannot disagree about it.

## Applying a patch

A patch is a text, and the text is git's unified diff — the one `git diff` and
`gitprompt diff` both print, and the one `git apply` and `gitprompt apply` both
read. That is the point of having the command at all: a patch mailed between
people, or produced by one of the two tools and applied by the other, is the
same text either way, and the surface suite checks exactly that round trip in
both directions. Nothing about the format is gitprompt's own.

A hunk names the lines it replaces, and the search for those lines is exact. It
tries the position the hunk's header gives, then a little further on, then a
little before; what it does not do is accept a *near* match. Git's `apply` can
be asked to fuzz a hunk into place by ignoring leading and trailing context
lines, and this cannot, because a fuzz match is a hunk landing somewhere the
person who wrote the patch did not look — the failure that follows is quieter
and worse than the one that refusing produces. Searching around the stated
position is not fuzz: the lines are still the lines, only the offset moved,
which is what happens when a file has grown above the change since the patch
was taken.

Nothing is written until every hunk of every file has matched. A patch is a
whole idea, and half of one applied to the work tree — the first file changed,
the second refused — is a state nobody asked for and that nothing in the patch
describes. So the hunks are matched against content read in memory first, and
only when the last one has matched does the first byte of any file change. The
cost is holding the files in memory; the benefit is that a failed `apply` is a
no-op, and the two error lines it prints name the file and line that did not
match.

The layer is chosen the way git chooses it. By default the patch is applied to
the work tree and the index is left alone, so the change shows up as unstaged
work. `--cached` applies it to the index only, and `--index` to both. When the
index is one of the layers it is the *preimage* the hunks are matched against,
not the file on disk — which is why `--cached` can take a patch the work tree
has already moved past, and why it is the useful form for a patch that came out
of `diff --cached` in the first place. `--check` runs the same match and stops
before the write, so that what it says would apply and what a real run does
apply cannot drift apart; they are one code path with one step left out.

A mode change has nowhere to live but the index. That a file is executable is
not a fact a work tree can state on every system this runs on — Windows has no
such bit — so a patch carrying `old mode`/`new mode` is a patch for the index,
and the work tree simply does not record it.

A path in a patch is a path in a text written by somebody else, so it is checked
before it is used: an absolute path, a path with `..` in it, a drive letter, or
an empty component is refused, and `--unsafe-paths` does not turn that off. What
it does is allow the path on the terms this repository already works in — the
path is normalised against the root and the leading `..` components are dropped,
so `../evil.txt` becomes `evil.txt` inside the work tree. Git, given the same
option, would write the file above the work tree. That is the one place this
deliberately disagrees: a version control tool that manages prompts should not
be talked into writing outside the project by a string in a patch.

Where those paths are read from is the other small disagreement. Git reads a
patch relative to where it was run: from a subdirectory, `f.txt` is understood
to mean that subdirectory's file, and a patch naming a path outside it is
skipped. Here the work tree's root is the reference point whichever directory
the command was run from, so `f.txt` is the same file everywhere, and a patch
taken at the top and applied from below lands where it says. That is the
simpler rule to hold onto for a tool whose patches are usually handed between
trees, though it does mean a patch git would have skipped is applied rather
than ignored.

The rest is what it declines to guess at. A binary patch, a combined diff
(`@@@`) and a diff3 conflict body (`|||||||`) each have a real grammar and each
is refused rather than half-read, along with the options that would change what
the text means (`-p<n>`, `-R`, `--3way`) — a patch that means something other
than what this can carry is better rejected than applied approximately.

## Two versions of the same series

A series that was rebased, or reworked after review, is the same work written
twice: the commits line up one for one, most of them still carrying the change
they carried, a few carrying something else. `range-diff` is the command that
shows the lining up, and the whole of it is two questions — how do you decide
two commits are the same change, and how do you decide which of one side's
commits answers to which of the other's.

The first is answered by a patch id, and the one here is built rather than
borrowed. It hashes the paths, the modes, the lines taken out and the lines put
in, and leaves out the position of each hunk and the blob names on either side,
because those are the parts that move when a commit is replayed onto a new base
while the change itself stays put. Git's `patch-id` is a different number over
the same idea, and the two are never compared: the ids are only ever weighed
against other ids from the same run, which is why using our own costs nothing.
What is deliberately *not* left out is the commit message. Two commits that
carry the same change into the same files are still different commits if they
say different things, and calling them a pair is right; calling them the same
pair, with the message difference invisible, is not.

The second question is a heuristic in either tool, and this one is stated
plainly: a commit is paired with the next one whose patch has at least half its
changed lines in common. Git weighs a cost against a creation factor instead,
and the two rules draw the line in slightly different places — a pair that is
near the line can come out `!` here and `<`/`>` there, which is the one place
the two tools visibly disagree. That is why the surface suite compares the two
only on a series whose pairs both rules agree about, and says so where it does
it. Both tools do agree on the ends of the scale: the same patch with the same
message is a pair, and a commit the other side does not have at all is not.

Both pairing passes run forward only, so the pairs never cross — a commit's
partner always sits further along the series than the commit before it did.
That is what a series *is*, an order, and it is also what lets the report
interleave the two sides in a single column: the next left, the next right the
pairing left alone, then the next pair, each taken in turn. A left is never
answered by a right that sits before its neighbour's.

The body printed under a `!` is a diff of the two commits as this command holds
them — the message first, then the patch — so that a pair whose change is the
same one told twice still shows what actually differs, which is then the
wording. Git prints its own rendering of the two commits in that place, with a
metadata block and the message quoted as markdown headings, and reproducing
that would be imitating a text this tool does not otherwise produce. The two
bodies answer the same question and are not the same text, and the surface
suite compares the marks and not the bodies for exactly that reason.

The `...` form is read here rather than through the revision parser. `A...B`
means the symmetric difference to the rest of git — both sides at once — and a
range-diff wants something else from the same spelling: the merge base of the
two tips as the base, and a range from there to each tip. So the two ends are
resolved and the base is worked out with the merge-base machinery, which is a
few lines, rather than borrowing a reading of the same characters that answers
a different question.

## Reading one object in place of another

A replaced object is a problem about *where* to do the substitution. The rest of
the tree never asks the object store a question the store cannot answer
differently for one id than for another -- a commit's tree, a tree's entries, a
blob's bytes -- so there is no repository-wide resolution step to hang this on,
and a command-by-command approach would mean every reader that walks parents or
reads a tree having to remember to look at `refs/replace` first. Missing one
would not fail loudly; it would quietly read the history as though the
replacement were not there, which is the bug that is hardest to notice.

So the substitution is made at the single funnel: `odb_read`, and with it
`odb_exists`, `odb_type_of` and `odb_resolve_prefix`. Fifteen files go through
it and none of them has to know. `repo_open` names the directory
(`<gitdir>/refs/replace`) and the maintenance commands clear it again, which is
the only asymmetry in the design and a deliberate one.

That asymmetry is `gc`, `repack`, `prune` and `fsck`. A reachability walk that
followed a replacement would mark the replacement's tree, its blobs and its
parents, and leave the replaced object's own unmarked -- and the pack that
followed would then write the replacement's bytes under the replaced id. Every
other reader in the repository would still see the original, so the result is
not "the replacement is used", it is a store that has lost an object some other
reader is entitled to. git's own `fsck` and `prune` do not follow replace refs
for the same reason, which is what makes this a fidelity choice rather than an
invention. Reading a replacement happens in `odb_read`, and that is untouched.

Two things the walk has to be safe against are then free. The read stops at five
hops, as git's does, because a longer chain is already a mistake; and a ref
whose value is the id it is filed under ends the walk where it stands, so a
self-replacement cannot spin. Neither is reachable through the commands here --
`replace` refuses both -- but a hand-written ref is a file, and the read is what
has to survive it.

## A second working directory

Everything in the store is one thing for the repository: the objects, the refs,
the configuration, the reflogs, the session and the `rr-cache`. Only two files
are about *where you are standing* rather than *what is in the repository* --
`HEAD`, which says what the next commit will be a child of, and `index`, which
says what has been staged. git leans on that split to let one repository be
worked in from several directories at once, and the layout is a small dance of
pointers: the main store gains `worktrees/<name>/` holding that directory's
`HEAD` and `index`, a `commondir` file (literally `../..`) naming the store, and
a `gitdir` file naming the directory's own `.git`; the directory itself gains a
`.git` **file** reading `gitdir: <main store>/worktrees/<name>`. Nothing is
copied, so a commit made in one is a branch the other already reads.

That is why `struct repo` grew a second directory. `gpdir` is the common store
and `wt_dir` is the directory that owns `HEAD` and the index, which are the same
thing until a worktree exists and different afterwards; `ref_store.head_dir`
then routes the one name `HEAD` to `wt_dir` while every other ref resolves
against `gpdir`. The split is not only about those two files. `MERGE_HEAD`,
`MERGE_MSG`, the message files, the sequencer, the bisect state and
`REPLACE_EDIT` all say something about work in progress, and two directories
that shared them would show a merge in one as a merge in the other -- so they
follow `HEAD` into the directory, and the checks that stop a merge in one
directory from colouring the other are the tests for it.

Discovery had to change with it, and in a way that is easy to get wrong in the
other direction. A repository is found by `.gitprompt` -- a directory -- or by a
`.git` **file**, which is what a linked worktree has. A `.git` directory is
deliberately not accepted: that is a git repository, and cloning a prompt history
with plain `git` leaves one behind, which is a directory of prompts with no store
rather than a repository, and the commands have to say so. Reading the file means
resolving a name that may be relative and may contain `..`, from a directory that
is the one being examined, so `collapse_dots` does the folding without touching
the disk. It also has to fold `\` to `/` before it does: on Windows a whole path
like `C:\a\b` is otherwise one component, and the first `..` would take the drive
letter with it.

Two consequences are worth stating. One branch has one HEAD, so two directories
cannot both have it checked out; that is refused by `worktree add` and by
`checkout` in a directory that is not the holder, from one question -- which
directory holds this branch -- asked by both. And a directory's `HEAD` being a
file of its own is what `worktree list` reads to print the branch beside each
path, where a detached one has no branch to print.

## A repository inside a repository

A submodule stands on the same `.git` file a linked worktree does, and that is
the whole trick: a directory whose `.git` names another store is a repository of
its own, and gitprompt had learned to read that file for `worktree`. What a
submodule adds is a way to have the store *be* part of the tree. The tree holds
a gitlink -- an entry with mode `160000` whose object name is a commit in a
repository the tree knows nothing else about -- `.gitmodules` holds the name and
the url under `submodule.<name>.path` and `.url`, and the store itself lives at
`<store>/modules/<name>`, created by `init --bare` like any other store. The path
is a checkout of it with `core.worktree` pointed at the path, so the two agree
about where the files are, and the name is the path, as git has it, so that
`libs/foo` and `other/foo` are two submodules and not one.

The gitlink is why nothing in the tree layer reads a submodule's entry: a commit
is not in this object store and looking it up would fail or, worse, find an
unrelated object of the same name. So the tree code carries the mode through
without opening it, and only the submodule commands know to open the store the
path names and ask *it* for the commit. `ls-tree` prints the entry, `status`
prints it as a change to be committed the way git does, and `checkout` makes the
empty directory and stops.

`update` is the one that crosses over, and it is the one place where the two
kinds of remote meet. A submodule's url is usually another repository -- on this
machine a path beside the parent, and `.gitmodules` keeps it relative so that a
clone of the parent still finds it. Fetching one is the same call the parent's
own `fetch` makes, so a submodule that is a local path is read directly and one
that is an `https` or `ssh` url goes through git, exactly as `remote` does. A
local path is read by opening it as a gitprompt store, so a bare git repository
at a path is not a submodule source here; where git would clone one, gitprompt
says the path is not a repository. What it *can* always read is a store git
wrote into, because a gitprompt store is an ordinary git object store -- `git
push` into one is how the two tools are checked against each other. What
`update` then checks out is the commit *the parent's index records*, not the
submodule's branch tip, which is the point of a submodule: a checkout of the
parent gives you the submodule at the commit the parent was tested with. That is
also why `update` will not proceed over a changed tracked file in the submodule,
and because the answer to "would this lose work" belongs in one place rather
than three, `deinit` and `worktree remove` both ask `worktree_is_clean`, which
counts an untracked file as much as a changed one.

`foreach` runs a command per submodule, and had to be built on `system` rather
than a `spawn` variant. The variables git sets -- `name`, `sm_path`,
`displaypath`, `sha1`, `toplevel` -- go into the environment, but `path` does
not, because on Windows a variable's name is matched without regard to case and
setting `path` would take the `PATH` with it. `git` leaves `path` out for the
same reason.

## Only part of the index in the work tree

Sparse checkout is easy to misread as "some paths are not tracked". They are all
tracked. A sparse work tree is a work tree that has been told to keep only part
of what the index holds, and the difference between the two has to be recorded
somewhere or every command that compares the index to the work tree would read
the missing paths as deletions.

Git records it in the index itself, as a per-entry bit -- skip-worktree, the
same bit `update-index --skip-worktree` sets by hand. Every entry stays; the bit
on it says this work tree is not the place that path lives. That is what makes
`status` quiet about it, what keeps `commit -a` from staging a deletion, and
what `ls-files -t` prints as `S` instead of `H`. It is also why nothing here
thins the index: the index is the list of what is tracked, and it does not get
shorter.

The bit has no room in version 2 of the index. The version 2 entry is a fixed
62 bytes and then the name, so there is nowhere for a second flags word to go;
version 3 puts one between the flags and the name, and the length of the name
that follows is what shifts. That is the whole of the difference, and it is a
difference that matters to whoever reads the file: the path in a version 3 entry
starts at 64, not 62, and a reader that assumes 62 reads the second flags word as
the first two letters of the name. The suite checks both directions -- git reads
the bit gitprompt set, and gitprompt reads the bit git set -- because a bug here
is invisible until someone else opens the file.

The patterns are the other half, and they are git's: one to a line in
`info/sparse-checkout`, `#` for a comment, a leading `!` to exclude, a trailing
`/` for a directory only. What is worth saying twice is the default. A path that
no line names is *not* in the work tree, so an empty pattern file is an empty
work tree, and a file of nothing but negations excludes everything they do not
name. The last line that matches decides, which is what makes a list read as a
series of corrections. It also explains what `init` writes: not an empty file,
which would take the whole work tree away, but two lines -- everything at the
top, then no directories -- which is what git writes and what leaves you with
the top-level files and nothing else.

Only the pattern list is here. Git's cone mode is a way of asking for that same
list in terms of directories, and expanding it is a convenience gitprompt does
not have, so a large cone has to be written out by hand. And the bit is only
honored where the work tree is walked: a checkout, a status, a `commit -a`, a
branch switch. The rest of git's sparse machinery -- a sparse index, `add
--sparse`, the `--sparse` option on the commands that take a pathspec -- is not
here, so a marked path is one nothing writes to, not one that has a second class
of behavior everywhere.

## The credential helper protocol

A credential helper is a program named in `credential.helper`, and the protocol
between git and it is small enough to state completely. Git runs the program
with the operation as its one argument — `get` for a `fill`, `store` for an
`approve`, `erase` for a `reject` — hands it `key=value` lines on its standard
input, and reads the answer off its standard output in the same form. The
request ends at the first blank line, and what the helper is handed does not
carry one: a helper that reads to end of file and a helper that reads to the
blank line both work, and git itself sends the second. The name has three
shapes, and no others: `!` and then a shell line, run as it stands; a path with
a slash in it, run as it stands; and a bare word, run as `git credential-<word>`,
which is how `store` and `cache` are named.

There are three rules that are easy to get wrong, and the suite holds them. The
answer is read whatever the helper's exit status was -- a helper that fails
loudly and prints nothing is the same as one that succeeds and prints nothing.
A later helper is the later word on a field, so two helpers each supplying half
a credential compose. And an empty value resets the list to nothing, which is
how a repository turns off a helper its global file turned on; a `helper` key
with no value at all is not the same thing, and is a parse error.

Why have this at all, when `https` and `ssh` are handed to real git and it
brings its own helpers? Because the protocol is the interface, and this tree can
speak it. A helper written for git is driven by gitprompt the same way, byte for
byte, and a helper gitprompt's output feeds is one git can read back -- the
surface suite runs both directions against a real store. It also means a helper
is not a thing this tool has, but a thing it can run: `credential-cache` and
`credential-store` are git's own programs, and what is here is the piece that
knows how to call them.

On Windows the calling is the hard part, and it is worth writing down. The
helper needs its operation as an argument *and* the credential on its standard
input, so the line to run has a space in it and a redirect in it -- and the C
runtime's `_spawnlp` cannot pass that. It splits the line on its own terms, so
`sh -c "helper get < file"` arrives as `sh -c helper` with everything after the
first word quietly gone. The way around it is to write the command into a small
script file and ask `sh` to run the file, which is what the code does; bytes
travel through files because there is no other way to be at both ends of a
program. `bisect run` reaches the shell the same way and for the same reason:
its command line is written into a file too, and the one argument left on the
command line is that file's name, quoted where the runtime would not quote it.
That last part matters twice over -- the chain is `_spawnlp` and not `system`
so that a spawn that never happened is still distinguishable from a status,
since 126 and 127 are exactly what the shell says when it cannot run a command
and `verify_good` is the thing that decides whether such an answer is
believed.

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

## Serving the store

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

## Offline, and on this machine

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

## How this machine runs an agent

`rerun` and `attach` have to turn a name — `claude`, `codex`, `dsh` — into a
command line, and there is no single right answer to that. How an agent is
installed is a fact about the machine the history is being replayed on rather
than about the history, and the same agent is installed differently from one
machine to the next: through `npx`, wrapped in a script, pinned to an older
version whose flags have moved. `rerun` derives the conversation ids from the
session ids precisely so that the same history replays into the same
conversations on whatever machine it lands on, and that promise is worth little
if the command line it hands them to is the one the author happened to have.

So the table of agents is a set of defaults rather than the last word, and every
piece of how one is driven can be replaced by
`gitprompt.agent.<name>.<field>`. There are seven fields for a replay: `command`,
the command line that begins a session; `resume`, the flag the conversation id
follows to continue one; `newSession`, the flag the id follows to name a new
one; `modelFlag` and `permissionFlag`, the flags a model name and a permission
mode follow; `modes`, the comma-separated values that permission flag accepts;
and `modeDefault`, which of them to use when nobody asks. `attach` reads two
more: `context`, the file the agent reads the history out of, and `readOnly`, the
whole command line that puts the agent in the mode where reading is all it does.

A key that is absent falls back to the built-in. A key set to the **empty
string** is not the same thing: it takes the piece away, and an agent with no
`resume` is one that cannot be told which conversation to continue — which is
exactly the state `codex` and `dsh` are in, and why `rerun` refuses them instead
of starting a string of unrelated sessions and calling it a replay. They are
named in the refusal so that the reason can be said out loud. An agent the table
has never heard of is usable too, as long as `gitprompt.agent.<name>.command`
says how this machine runs it; it needs `resume` and `newSession` as well before
it can be replayed into, since those are what make a replay a replay rather than
a first run.

Two pairs have to move together. `modes` and `modeDefault` describe one flag:
replace the values an agent accepts and `modeDefault` has to name one of them, or
the run stops on mode validation before it starts. And `context` and `readOnly`
describe one agent: an agent whose read-only invocation is not known is not
refused, it is attached to without the read-only line, and the document says
which key would have supplied it.

A value that begins with a dash needs the option terminator, because our own
`config` reads a dash-leading second argument as an option:

```console
$ gitprompt config gitprompt.agent.claude.modelFlag -- -m
```

real git takes it either way, so this is one of the small places where gitprompt
is the stricter of the two. The keys live in gitprompt's own config files and
nowhere else — the local `<repo>/.gitprompt/config` and the global
`$HOME/.gitpromptconfig` — so a field name containing a dot is never something
git is asked to parse.

Last, the first word of `command` is looked up before anything runs, through the
same shell that is about to run it — `command -v` on Unix, `where` on Windows —
so `PATHEXT`, the working directory and quoting behave exactly as they will when
the agent is started, and a missing program is reported as a missing program
rather than as a confusing failure at the first prompt. The check happens only
when a run is actually about to begin, never for the plan, so reading what a
replay would do costs nothing.
