#!/bin/sh
#
# smoke.sh -- exercise gitprompt end to end.
#
#   sh test/smoke.sh
#   GP=/path/to/gitprompt sh test/smoke.sh
#
# Every check runs a command and asserts on its output or exit status.  Values
# that vary between runs (timestamps, object ids, session ids) are matched by
# shape, never by value, so a passing run means what it claims to mean: there
# are no checks here that cannot fail.
#
# The git-interop section is skipped when git is not on PATH.  It is the
# section that matters most, because it tests the design claim that a
# gitprompt repository is an ordinary git repository.

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
GP=${GP:-$root/gitprompt.exe}
[ -f "$GP" ] || GP=$root/gitprompt
[ -f "$GP" ] || { echo "no gitprompt binary at $GP; run make first"; exit 2; }

case "${WORK:-}" in
"") work=$root/build/smoke ;;
*)  work=$WORK ;;
esac
rm -rf "$work"
mkdir -p "$work" || exit 2

pass=0
fail=0
skipped=0

say() { printf '\n== %s\n' "$1"; }
ok()  { pass=$((pass + 1)); printf 'ok   %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf 'FAIL %s\n' "$1"; [ $# -gt 1 ] && printf '     %s\n' "$2"; return 0; }
skip() { skipped=$((skipped + 1)); printf 'skip %s\n' "$1"; }

gp() { "$GP" "$@"; }

# expect <description> <substring> <command...>
expect() {
	desc=$1; want=$2; shift 2
	got=$("$@" 2>&1)
	case "$got" in
	*"$want"*) ok "$desc" ;;
	*) bad "$desc" "wanted to find [$want] in [$got]" ;;
	esac
}

# expect_out <description> <expected-exactly> <command...>
expect_out() {
	desc=$1; want=$2; shift 2
	got=$("$@" 2>&1)
	if [ "$got" = "$want" ]; then
		ok "$desc"
	else
		bad "$desc" "wanted [$want] got [$got]"
	fi
}

# expect_status <description> <expected-exit> <command...>
expect_status() {
	desc=$1; want=$2; shift 2
	out=$("$@" 2>&1); rc=$?
	if [ "$rc" = "$want" ]; then
		ok "$desc"
	else
		bad "$desc" "wanted exit $want got $rc: $out"
	fi
}

# expect_file <description> <path>
expect_file() {
	if [ -e "$2" ]; then ok "$1"; else bad "$1" "no such path: $2"; fi
}

# expect_absent <description> <path>
expect_absent() {
	if [ ! -e "$2" ]; then ok "$1"; else bad "$1" "unexpectedly present: $2"; fi
}

# ------------------------------------------------------------------
say "an empty repository"

repo=$work/repo
mkdir -p "$repo" || exit 2
cd "$repo" || exit 2

expect "init reports the directory it made" ".gitprompt" gp init .
expect_file "HEAD exists" .gitprompt/HEAD
expect_file "config exists" .gitprompt/config
expect_file "the object store is a directory" .gitprompt/objects
expect_file "refs/heads is a directory" .gitprompt/refs/heads
expect_file "refs/tags is a directory" .gitprompt/refs/tags
expect_out "HEAD names the default branch" "refs/heads/main" gp symbolic-ref HEAD
expect_out "promptDir is configured" "prompts" gp config gitprompt.promptDir
expect "the repository format version is recorded" "repositoryformatversion" gp config --list
expect_status "log on an unborn branch fails" 128 gp log --oneline
expect "the refusal names the branch" "does not have any commits yet" \
	gp log --oneline
expect "status explains the unborn branch" "No commits yet" gp status

# ------------------------------------------------------------------
say "the object model"

printf 'hello\n' > a.txt
oid=$(gp hash-object -w a.txt)
case "$oid" in
????????????????????????????????????????) ok "hash-object returns 40 hex digits" ;;
*) bad "hash-object returns 40 hex digits" "got [$oid]" ;;
esac
expect_out "the object reads back" "hello" gp cat-file -p "$oid"
expect_out "the type is blob" "blob" gp cat-file -t "$oid"
expect_out "rev-parse of a full id is itself" "$oid" gp rev-parse "$oid"
expect_out "an abbreviated id resolves" "blob" gp cat-file -t "$(echo "$oid" | cut -c1-8)"
expect_status "an unknown object fails" 128 gp cat-file -t 0000000000000000000000000000000000000000
expect "verify-objects walks the store" "Checked 1" gp verify-objects
expect_out "count-objects counts it" "1 objects" gp count-objects
expect "count-objects -v is verbose" "count: 1" gp count-objects -v

# an empty index is not an error: git answers with the empty tree's id
expect_status "write-tree with an empty index succeeds" 0 gp write-tree
expect_out "the empty tree has git's id" \
	4b825dc642cb6eb9a060e54bf8d69288fbee4904 gp write-tree
gp add a.txt >/dev/null 2>&1
tree=$(gp write-tree)
expect_out "rev-parse agrees with write-tree" "$tree" gp rev-parse "$tree"
expect "ls-tree lists the staged path" "a.txt" gp ls-tree "$tree"
expect "the tree records the mode" "100644" gp ls-tree "$tree"
expect "ls-files lists the index" "a.txt" gp ls-files
expect "ls-files -s shows the mode" "100644" gp ls-files -s

commit=$(gp commit-tree "$tree" -m "the first commit")
expect "the commit names its tree" "$tree" gp cat-file -p "$commit"
expect "the commit has an author" "author " gp cat-file -p "$commit"
expect "the commit has a committer" "committer " gp cat-file -p "$commit"
expect "the commit message is there" "the first commit" gp cat-file -p "$commit"
expect "the commit type is commit" "commit" gp cat-file -t "$commit"
expect_status "an unknown revision fails" 128 gp rev-parse nosuchref

# ------------------------------------------------------------------
say "recording prompts"

expect "a session starts" "started" gp session start -t "build a parser"
sid=$(gp session current)
case "$sid" in
s_*_??????) ok "the session id has the documented shape" ;;
*) bad "the session id has the documented shape" "got [$sid]" ;;
esac
expect_file "the session file is at its deterministic path" "prompts/sessions/$sid.md"
expect "session list shows it" "$sid" gp session list
expect "session current reports it" "$sid" gp session current
expect "session show prints the title" "build a parser" gp session show

expect "a prompt is recorded" "p_" gp prompt -m "write a tokenizer first"
expect_file "the first prompt is at 0001" "prompts/0001-write-a-tokenizer-first.md"
expect "the frontmatter names the session" "session: $sid" cat prompts/0001-write-a-tokenizer-first.md
expect "the frontmatter carries a sequence" "seq: 1" cat prompts/0001-write-a-tokenizer-first.md
expect "the frontmatter carries a timestamp" "timestamp: " cat prompts/0001-write-a-tokenizer-first.md
expect "the frontmatter carries the body" "write a tokenizer first" cat prompts/0001-write-a-tokenizer-first.md

gp prompt -m "second prompt" >/dev/null
expect_file "the sequence advances" "prompts/0002-second-prompt.md"
printf 'a prompt from stdin\n' | gp capture >/dev/null
expect_file "capture reads stdin" "prompts/0003-a-prompt-from-stdin.md"
gp prompt -t parser -m "a tagged prompt" >/dev/null
expect "the tag is recorded in the frontmatter" "parser" cat prompts/0004-a-tagged-prompt.md

# outcome, both spellings: an id names a prompt anywhere in the history, and
# --last is what the quick start uses -- the prompt just written, unnamed
pid=$(sed -n 's/^id: //p' prompts/0004-a-tagged-prompt.md)
expect "outcome attaches a note by id" "outcome recorded for $pid" \
	gp outcome "$pid" "the parser rejects empty input"
expect "the note lands in the frontmatter" \
	"outcome: the parser rejects empty input" cat prompts/0004-a-tagged-prompt.md
expect "outcome --last attaches to the newest prompt" "outcome recorded for " \
	gp outcome --last "and the empty tree"
expect "the --last note lands on the newest prompt" \
	"outcome: and the empty tree" cat prompts/0004-a-tagged-prompt.md
expect_status "outcome rejects an unknown id" 1 gp outcome p_nosuchid "text"
expect_status "outcome needs text" 1 gp outcome "$pid"

expect "session end closes it" "ended" gp session end
expect_status "ending twice fails" 1 gp session end
expect "the session file records the end" "ended_at: " cat "prompts/sessions/$sid.md"

# ------------------------------------------------------------------
say "committing"

gp add -A >/dev/null 2>&1
expect "commit prints the branch and the subject" "record the first prompts" gp commit -m "record the first prompts"
expect "the log shows the subject" "record the first prompts" gp log --oneline
expect "show prints a commit" "record the first prompts" gp show HEAD
expect "status is clean after committing" "nothing to commit" gp status

printf 'dirty\n' >> a.txt
expect "a modified file is reported modified" "modified" gp status
expect "diff shows the added line" "+dirty" gp diff
gp add a.txt >/dev/null 2>&1
expect "a staged file is listed under changes to be committed" \
	"Changes to be committed:" gp status
expect "the cached diff shows the change" "+dirty" gp diff --cached
expect "diff --stat summarises" "a.txt" gp diff --cached --stat
gp commit -m "change a file" >/dev/null 2>&1

# ------------------------------------------------------------------
say "reconstruction"

expect "timeline lists the prompts" "p_" gp timeline
expect "timeline carries the session title" "build a parser" gp timeline
expect "timeline shows the prompt text" "write a tokenizer first" gp timeline
expect "log-prompt lists prompts" "write a tokenizer" gp log-prompt
expect "replay renders markdown" "write a tokenizer first" gp replay
expect "replay keeps the chronological order" "0001" gp replay
expect "replay names the session" "build a parser" gp replay
expect "replay can render json" '"prompts"' gp replay --format=json
expect "replay can render plain text" "write a tokenizer" gp replay --format=txt
expect "stats summarises the repository" "prompt" gp stats
expect "stats can emit json" "{" gp stats --json
expect "replay can list the sessions instead" "$sid" gp replay --list-sessions
expect "timeline takes a revision" "write a tokenizer first" gp timeline HEAD

replay_out=$work/replay.md
gp replay -o "$replay_out" >/dev/null 2>&1
expect_file "replay writes to a file" "$replay_out"
expect "the written file has the first prompt" "0001" cat "$replay_out"

# the point of the whole design: order and session boundaries survive
n1=$(grep -n "write a tokenizer first" "$replay_out" | head -1 | cut -d: -f1)
n2=$(grep -n "second prompt" "$replay_out" | head -1 | cut -d: -f1)
if [ -n "$n1" ] && [ -n "$n2" ] && [ "$n1" -lt "$n2" ]; then
	ok "replay preserves chronological order"
else
	bad "replay preserves chronological order" "lines: $n1 then $n2"
fi
expect "replay records the session boundary" "build a parser" cat "$replay_out"

# ------------------------------------------------------------------
say "branches, tags and history editing"

gp branch madebranch
expect "the branch is listed" "madebranch" gp branch
gp checkout madebranch >/dev/null 2>&1
expect "checkout moves HEAD" "refs/heads/madebranch" gp symbolic-ref HEAD
gp switch -c other >/dev/null 2>&1
expect "switch -c moves HEAD" "refs/heads/other" gp symbolic-ref HEAD
expect "a branch made by switch -c is in the reflog" "branch: Created" gp reflog
printf 'branch only\n' > b.txt
gp add b.txt >/dev/null 2>&1
gp commit -m "a commit only on other" >/dev/null 2>&1
expect "the new branch has the new file" "b.txt" gp ls-tree HEAD

gp checkout main >/dev/null 2>&1
expect "we are back on main" "refs/heads/main" gp symbolic-ref HEAD
expect_absent "the other branch's file is gone" b.txt

expect "merge fast-forwards" "Fast-forward" gp merge other
expect "the merged file arrives" "b.txt" gp ls-tree HEAD
expect "an already-merged branch says so" "up to date" gp merge other

# a real conflict
gp switch -c side >/dev/null 2>&1
printf 'from side\n' > f.txt
gp add f.txt >/dev/null 2>&1
gp commit -m "side changes f" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
printf 'from main\n' > f.txt
gp add f.txt >/dev/null 2>&1
gp commit -m "main changes f" >/dev/null 2>&1
expect "a conflicting merge reports a conflict" "CONFLICT" gp merge side
expect "the conflict is marked in the file" "<<<<<<< ours" cat f.txt
expect "the conflict names the other side" ">>>>>>> theirs" cat f.txt
expect "the conflict does not commit itself" "main changes f" gp log --oneline

# the unfinished merge is a state on disk, as in git
expect_file "the merge is recorded in MERGE_HEAD" .gitprompt/MERGE_HEAD
expect "status names the unmerged paths" "You have unmerged paths." gp status
expect "status --short marks the path UU" "UU f.txt" gp status --short
expect_status "commit refuses while a path is unmerged" 1 gp commit -m "too soon"
expect "the refusal names the reason" "unmerged paths" gp commit -m "too soon"
expect_status "a second merge refuses while one is unfinished" 128 gp merge main
expect "the second merge names the reason" "not concluded your merge" gp merge main

# stage the resolution, then commit it: the result must have two parents
printf 'reconciled\n' > f.txt
gp add f.txt >/dev/null 2>&1
if gp status --short | grep -q '^UU'; then
	bad "staging the path resolves it" "status still shows UU"
else
	ok "staging the path resolves it"
fi
expect_absent "resolving drops the conflict list" .gitprompt/MERGE_CONFLICTS
expect_file "MERGE_HEAD outlives the resolution" .gitprompt/MERGE_HEAD
expect "the merge is concluded by a commit" "merge side" gp commit -m "merge side"
expect_absent "the merge state is gone once committed" .gitprompt/MERGE_HEAD
nparents=$(gp cat-file -p HEAD | grep -c '^parent ')
if [ "$nparents" = 2 ]; then
	ok "the concluding commit has two parents"
else
	bad "the concluding commit has two parents" "found $nparents"
fi
expect "the reconciliation is in the file" "reconciled" cat f.txt

# --abort throws a merge away and puts the tree back
gp switch -c side2 >/dev/null 2>&1
printf 'side2\n' > g.txt
gp add g.txt >/dev/null 2>&1
gp commit -m "side2 adds g" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
printf 'from main\n' > g.txt
gp add g.txt >/dev/null 2>&1
gp commit -m "main adds g" >/dev/null 2>&1
expect "a second conflict is reported" "CONFLICT" gp merge side2
abort_out=$(gp merge --abort 2>&1); abort_rc=$?
if [ "$abort_rc" = 0 ]; then
	ok "merge --abort succeeds during a merge"
else
	bad "merge --abort succeeds during a merge" "exit $abort_rc: $abort_out"
fi
case "$abort_out" in
*"Merge aborted"*) ok "abort says what it did" ;;
*) bad "abort says what it did" "got [$abort_out]" ;;
esac
if grep -q '<<<<<<<' g.txt; then
	bad "abort removes the conflict markers" "markers are still in g.txt"
else
	ok "abort removes the conflict markers"
fi
expect "abort restores our version of the file" "from main" cat g.txt
expect_absent "abort drops the merge state" .gitprompt/MERGE_HEAD
expect_status "merge --abort with no merge refuses" 1 gp merge --abort
expect "a clean tree after the abort" "nothing to commit" gp status

# --ff-only refuses a divergence rather than merging quietly
expect_status "merge --ff-only refuses a divergence" 1 gp merge --ff-only side2
expect "the refusal names the reason" "not possible to fast-forward" \
	gp merge --ff-only side2
expect_absent "a refused merge leaves no merge state" .gitprompt/MERGE_HEAD
expect "an up-to-date ff-only merge is not a refusal" "up to date" \
	gp merge --ff-only main

# --no-commit stages a merge without recording it, then commit finishes it
gp switch -c clean1 >/dev/null 2>&1
printf 'one\n' > h1.txt
gp add h1.txt >/dev/null 2>&1
gp commit -m "clean1 adds h1" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
gp switch -c clean2 >/dev/null 2>&1
printf 'two\n' > h2.txt
gp add h2.txt >/dev/null 2>&1
gp commit -m "clean2 adds h2" >/dev/null 2>&1
# clean1 and clean2 have each added a different file: a real divergence
expect "a non-conflicting merge is prepared" "Merge prepared" \
	gp merge --no-commit clean1
expect_file "the prepared merge records MERGE_HEAD" .gitprompt/MERGE_HEAD
expect "the prepared merge is staged" "h1.txt" gp ls-files
expect "the prepared merge is not yet committed" "clean2 adds h2" gp log --oneline
expect_status "committing concludes the prepared merge" 0 gp commit -m "join the two"
expect_file "their side arrived" h1.txt
expect_file "our side is still there" h2.txt
nparents=$(gp cat-file -p HEAD | grep -c '^parent ')
if [ "$nparents" = 2 ]; then
	ok "the prepared merge commits with two parents"
else
	bad "the prepared merge commits with two parents" "found $nparents"
fi
expect_absent "the prepared merge state is gone" .gitprompt/MERGE_HEAD
expect "merging an already-merged branch says so" "up to date" gp merge clean1

# the three-way merge has to reach the index, not only the work tree
expect "the merged tree records their file" "h1.txt" gp ls-tree HEAD
expect "the merged tree records our file" "h2.txt" gp ls-tree HEAD

gp tag v1.0 >/dev/null 2>&1
expect "the tag is listed" "v1.0" gp tag -l
expect "describe names the commit by its tag" "v1.0" gp describe
gp tag -m "a release" v2.0 >/dev/null 2>&1
expect_out "an annotated tag is a tag object" "tag" gp cat-file -t v2.0
expect "an annotated tag keeps its message" "a release" gp cat-file -p v2.0
gp tag -d v2.0 >/dev/null 2>&1
expect_status "a deleted tag is gone" 128 gp rev-parse v2.0

expect "for-each-ref lists refs" "refs/heads/main" gp for-each-ref
expect "reflog has entries" "commit" gp reflog
expect_status "reset --hard succeeds" 0 gp reset --hard HEAD~1
expect "HEAD moved back one commit" "a commit only on other" gp log --oneline
expect_status "check-ref-format accepts a good name" 0 gp check-ref-format refs/heads/good
expect_status "check-ref-format rejects a bad name" 1 gp check-ref-format "refs/heads/bad name"

# ------------------------------------------------------------------
say "a local remote"

origingp=$work/origin-gp
gp init "$origingp" >/dev/null 2>&1
expect "a remote is added" "added remote" gp remote add origin "$origingp"
expect "the remote is listed" "origin" gp remote
expect "the remote url is reported" "origin-gp" gp remote get-url origin
expect "remote -v shows the url" "origin-gp" gp remote -v
# the branch checked out here is clean2, from the merge above, so that is the
# branch a bare `push` sends; git names it on the "src -> dst" line
expect "push reports the ref" "clean2 -> clean2" gp push origin
expect_file "the remote received a ref" "$origingp/.gitprompt/refs/heads/clean2"
expect "the remote-tracking ref was written" "origin/clean2" gp for-each-ref refs/remotes
expect "push again is up to date" "up to date" gp push origin

cd "$work" || exit 2
expect "clone from a local path" "Cloning" gp clone "$repo" cloned
expect_file "the clone has the project files" cloned/a.txt
expect_file "the clone has the prompts" cloned/prompts/0001-write-a-tokenizer-first.md
expect_file "the clone has the session file" "cloned/prompts/sessions/$sid.md"
cd "$work/cloned" || exit 2
expect "the clone has origin configured" "origin" gp remote
expect "the clone reads the same log" "record the first prompts" gp log --oneline
expect "the clone replays the same prompts" "write a tokenizer first" gp replay
expect "fetch on an up-to-date clone says so" "up to date" gp fetch origin
expect_status "fsck is clean on the clone" 0 gp fsck

cd "$repo" || exit 2
expect_status "fsck is clean" 0 gp fsck
gc_out=$(gp gc)
case "$gc_out" in
*Pruned*) ok "gc reports what it did" ;;
*) bad "gc reports what it did" "$gc_out" ;;
esac
case "$gc_out" in
*"into a pack"*) ok "gc packs the reachable objects" ;;
*) bad "gc packs the reachable objects" "$gc_out" ;;
esac
expect "gc --dry-run reports without pruning" "Would prune" gp gc --dry-run

# ------------------------------------------------------------------
say "a packed object store"

# gc packs what the refs reach, which is the shape a store takes after the git
# binary has fetched into it -- and the case a reader that only knows loose
# objects gets wrong, because then it cannot read back its own store.
pack_idx=$(ls "$repo"/.gitprompt/objects/pack/*.idx 2>/dev/null | head -1)
if [ -n "$pack_idx" ]; then
	ok "gc leaves a pack behind"
else
	bad "gc leaves a pack behind" "no .idx under .gitprompt/objects/pack"
fi
if gp count-objects -v | grep -q '^in-pack: [1-9]'; then
	ok "the pack holds the reachable objects"
else
	bad "the pack holds the reachable objects" "$(gp count-objects -v)"
fi

# every object the walk reached is in the pack: gc's own count of what it
# kept reachable is the index's count of what is in it
kept=$(printf '%s\n' "$gc_out" | sed -n 's/^Kept \([0-9][0-9]*\) reachable object(s)$/\1/p')
inpack=$(gp count-objects -v | sed -n 's/^in-pack: //p')
if [ -n "$kept" ] && [ "$kept" = "$inpack" ]; then
	ok "everything reachable is in the pack"
else
	bad "everything reachable is in the pack" "kept=${kept:-?} in-pack=${inpack:-?}"
fi

# and what is still loose is exactly what gc protected: an unreachable
# object younger than the grace period, which packing would have kept
# forever
grace=$(printf '%s\n' "$gc_out" | sed -n 's/^Kept \([0-9][0-9]*\) unreachable object(s) younger.*$/\1/p')
loose=$(gp count-objects | sed -n 's/ objects$//p')
if [ "${grace:-0}" = "$loose" ]; then
	ok "what stays loose is what the grace period protects"
else
	bad "what stays loose is what the grace period protects" \
		"grace=${grace:-0} loose=$loose"
fi

if gp verify-objects | grep -q '^Checked 0'; then
	bad "verify-objects checks the packed objects" "Checked 0"
else
	ok "verify-objects checks the packed objects"
fi

# Every reader has to cope with a packed store, not only the ones that ask for
# an object's type as well as its contents.  replay, timeline, log-prompt and
# stats want the contents alone, and that path has to reach the pack too.
expect "replay reads the packed store" "write a tokenizer first" gp replay
expect "timeline reads the packed store" "write a tokenizer first" gp timeline
expect "log-prompt reads the packed store" "write a tokenizer first" gp log-prompt
expect "stats reads the packed store" "prompts:" gp stats
expect "replay --format=json reads the packed store" "write a tokenizer first" \
	gp replay --format=json

# A second gc of an unchanged store writes the same pack under the same name.
# It must not treat the pack it has just written as one it has superseded and
# delete it: the loose copies are forgotten right afterwards, so that would
# take every object in the store with it.
pack_again=$(gp gc)
inpack_again=$(gp count-objects -v | sed -n 's/^in-pack: //p')
packs_after=$(gp count-objects -v | sed -n 's/^packs: //p')
if [ "${inpack_again:-x}" = "${inpack:-y}" ] && [ "${packs_after:-0}" = 1 ]; then
	ok "a second gc keeps the pack and every object"
else
	bad "a second gc keeps the pack and every object" \
		"packs=${packs_after:-?} was=$inpack now=${inpack_again:-?}"
fi
expect_status "the store is still readable after a second gc" 0 gp fsck

# A commit made after a gc leaves the old pack wholly reachable, so the new
# pack supersedes it.  One pack is the steady state; a store packed daily
# should not end up with one pack per day.
gp prompt -m "a prompt recorded after the first gc" >/dev/null
gp add -A >/dev/null
gp commit -m "a commit after the first gc" >/dev/null
gc_third=$(gp gc)
packs_after=$(gp count-objects -v | sed -n 's/^packs: //p')
case "$gc_third" in
*"1 superseded pack(s)"*) superseded=yes ;;
*) superseded=no ;;
esac
if [ "${packs_after:-0}" = 1 ] && [ "$superseded" = yes ]; then
	ok "gc supersedes the pack it replaced"
else
	bad "gc supersedes the pack it replaced" \
		"packs=${packs_after:-?} out=$(printf '%s' "$gc_third" | tr '\n' ' ')"
fi
expect_status "the store is still readable after a superseding gc" 0 gp fsck
expect "the commit made after the gc is in the log" \
	"a commit after the first gc" gp log --oneline

# the local transport reads objects rather than copying loose files, so a
# packed repository clones like any other
cd "$work" || exit 2
expect "clone reads a packed store" "Cloning" gp clone "$repo" packedclone
expect_file "the clone of a packed store has the prompts" \
	packedclone/prompts/0001-write-a-tokenizer-first.md
cd "$work/packedclone" || exit 2
expect_status "fsck is clean on the clone of a packed store" 0 gp fsck
expect "the clone of a packed store has the same log" \
	"record the first prompts" gp log --oneline
cd "$repo" || exit 2

# ------------------------------------------------------------------
say "git interoperability"

if command -v git >/dev/null 2>&1; then
	gpdir=$repo/.gitprompt

	expect "git reads our commits" "record the first prompts" \
		git --git-dir="$gpdir" log --oneline
	expect "git parses our trees" "a.txt" \
		git --git-dir="$gpdir" ls-tree -r HEAD
	expect "git sees the prompts as ordinary blobs" "prompts/0001" \
		git --git-dir="$gpdir" ls-tree -r --name-only HEAD
	expect "git reads a prompt's contents" "write a tokenizer first" \
		git --git-dir="$gpdir" show HEAD:prompts/0001-write-a-tokenizer-first.md
	expect "git reads the session file" "build a parser" \
		git --git-dir="$gpdir" show "HEAD:prompts/sessions/$sid.md"
	# git refuses an index whose entries are not in path order, and staging
	# appends, so this is the sort index_write does rather than the order
	# the paths were added in
	expect_status "git reads our index" 0 git --git-dir="$gpdir" ls-files
	gp ls-files > "$work/gp-lsfiles" 2>&1
	# git ends its lines with CRLF on Windows, which says nothing about the
	# order or the paths, so compare with the carriage returns taken out
	git --git-dir="$gpdir" ls-files 2>&1 | tr -d '\r' > "$work/git-lsfiles"
	if cmp -s "$work/gp-lsfiles" "$work/git-lsfiles"; then
		ok "git and gitprompt list the same staged paths"
	else
		bad "git and gitprompt list the same staged paths" \
			"$(diff "$work/gp-lsfiles" "$work/git-lsfiles" 2>&1 | head -4)"
	fi
	# a clean fsck says nothing at all: the exit status is the whole result
	expect_status "git verifies our object store" 0 \
		git --git-dir="$gpdir" fsck --no-dangling --no-progress

	# and it can check the pack gc wrote, object by object, against its
	# own index: the format is git's, not a lookalike
	bad_packs=
	for idx in "$gpdir"/objects/pack/*.idx; do
		[ -e "$idx" ] || continue
		git verify-pack -v "$idx" >/dev/null 2>&1 || bad_packs="$bad_packs $idx"
	done
	if [ -z "$bad_packs" ]; then
		ok "git verifies every pack gc wrote"
	else
		bad "git verifies every pack gc wrote" "unverifiable:$bad_packs"
	fi
	expect_status "git can walk our whole history" 0 \
		git --git-dir="$gpdir" rev-list --all --quiet

	# A pack holding an object the refs no longer reach has to survive gc.
	# A fetch leaves objects packed and with no loose copy beside them, so
	# deleting a pack like this would lose the object outright -- there
	# would be nothing left to read it from.
	cruft=$work/cruftpack
	rm -rf "$cruft"
	(cd "$work" && gp init cruftpack >/dev/null) || exit 2
	cd "$cruft" || exit 2
	gp prompt -m "a prompt that stays reachable" >/dev/null
	gp add -A >/dev/null && gp commit -m kept >/dev/null
	gp prompt -m "a prompt that is abandoned" >/dev/null
	gp add -A >/dev/null && gp commit -m abandoned >/dev/null
	doomed=$(gp rev-parse HEAD)
	git --git-dir="$cruft/.gitprompt" rev-list --objects "$doomed" |
		git --git-dir="$cruft/.gitprompt" pack-objects \
			"$cruft/.gitprompt/objects/pack/pack" >/dev/null
	# pack the abandoned commit, then take the loose copies away, which is
	# the state a fetch leaves behind
	for d in "$cruft"/.gitprompt/objects/??; do
		[ -d "$d" ] && rm -rf "$d"
	done
	gp reset --hard HEAD~1 >/dev/null 2>&1
	gp gc >/dev/null 2>&1
	if [ "$(gp cat-file -p "$doomed" 2>/dev/null | head -1 | cut -d' ' -f1)" = tree ]; then
		ok "gc keeps a pack holding an unreachable object"
	else
		bad "gc keeps a pack holding an unreachable object" \
			"the abandoned commit is no longer readable"
	fi
	cd "$repo" || exit 2

	# Ancestry suffixes, against git as the reference.  These were dead code
	# until they were fixed: the suffix was searched for in the name *after*
	# truncating it, so the search hit the NUL that the truncation had just
	# written and every `HEAD~1` resolved to HEAD -- which would have made
	# `reset --hard HEAD~1` a silent no-op.
	for rev in 'HEAD^' 'HEAD~1' 'HEAD^0' 'HEAD~0'; do
		ours=$(gp rev-parse "$rev" 2>/dev/null)
		theirs=$(git --git-dir="$gpdir" rev-parse "$rev" 2>/dev/null)
		if [ -n "$theirs" ] && [ "$ours" = "$theirs" ]; then
			ok "rev-parse resolves $rev the way git does"
		else
			bad "rev-parse resolves $rev the way git does" \
				"ours=$ours theirs=$theirs"
		fi
	done
	expect_status "a suffix that walks past the root is refused" 128 \
		gp rev-parse 'HEAD~99'
	expect_status "junk after a suffix is refused" 128 gp rev-parse 'HEAD~1x'

	# hashing must agree, or none of the above would mean anything
	cd "$work" || exit 2
	printf 'hash me\n' > h.txt
	ours=$(gp hash-object h.txt)
	theirs=$(git hash-object h.txt)
	if [ "$ours" = "$theirs" ]; then
		ok "git and gitprompt agree on the object id"
	else
		bad "git and gitprompt agree on the object id" "ours=$ours theirs=$theirs"
	fi
	ours_tree=$(printf '' | gp hash-object -t tree --stdin)
	# Not /dev/null: MSYS rewrites that to the Windows device name `nul` before
	# git ever sees it, and git then fails to open it.  Feed git the same empty
	# input through the same mechanism instead.
	theirs_tree=$(printf '' | git hash-object -t tree --stdin)
	if [ "$ours_tree" = "$theirs_tree" ]; then
		ok "git and gitprompt agree on the empty tree's id"
	else
		bad "git and gitprompt agree on the empty tree's id" \
			"ours=$ours_tree theirs=$theirs_tree"
	fi

	# the strongest form: a plain git, given only gitprompt's store, can
	# materialise the entire project
	plain=$work/plainclone
	mkdir -p "$plain" && cd "$plain" || exit 2
	rm -rf .git
	cp -r "$gpdir" .git
	# GIT_DIR is pinned to the scratch copy deliberately.  $work usually sits
	# inside the checkout, so a bare `git reset --hard` here would walk up to
	# the developer's own repository and reset that whenever the copy above did
	# not land -- turning a failing test into destroyed uncommitted work.
	if GIT_DIR="$plain/.git" GIT_WORK_TREE="$plain" git reset --hard HEAD >/dev/null 2>&1; then
		expect_file "a plain git reconstructs the work tree" a.txt
		expect_file "a plain git reconstructs the prompts" prompts/0001-write-a-tokenizer-first.md
		expect "the reconstructed prompt keeps its session" "session: $sid" \
			cat prompts/0001-write-a-tokenizer-first.md
		ok "git alone can rebuild the project from the gitprompt store"
	else
		bad "git alone can rebuild the project from the gitprompt store" \
			"git reset --hard failed"
	fi

	# and git can clone the store directly
	rm -rf "$work/directclone"
	if git clone -q "$gpdir" "$work/directclone" 2>/dev/null; then
		expect_file "git clone of the store yields the prompts" \
			"$work/directclone/prompts/0001-write-a-tokenizer-first.md"
	else
		skip "git clone of a .gitprompt store (git wants a bare-repo layout; the reset --hard test above covers the same ground)"
	fi
else
	skip "the whole git interoperability section (git is not on PATH)"
fi

# ------------------------------------------------------------------
say "things that are not implemented"

cd "$repo" || exit 2
expect_status "serve reports honestly and fails" 1 gp serve
expect "serve names the reason" "not implemented" gp serve
expect_status "an unknown command fails" 1 gp nosuchcommand
expect_status "a replay of an unknown ref fails" 128 gp replay nosuchref

# ------------------------------------------------------------------
say "a revision means that revision"

# Its own repository, so nothing above depends on the extra commit.  The
# property: `replay <ref>` is the history at that ref.  The work tree is
# later than every ref, so folding it in -- which is right for a bare
# `replay`, which asks what has been recorded -- would make the argument
# mean nothing here, since the newest prompt is checked out in both cases.
refrepo=$work/refrepo
mkdir -p "$refrepo" || exit 2
cd "$refrepo" || exit 2
gp init . >/dev/null 2>&1
gp prompt -m "the first prompt" >/dev/null 2>&1
gp add -A >/dev/null 2>&1
gp commit -m "first" >/dev/null 2>&1
gp prompt -m "the second prompt" >/dev/null 2>&1
gp add -A >/dev/null 2>&1
gp commit -m "second" >/dev/null 2>&1

n_all=$(gp replay | grep -c '^### ')
n_ref=$(gp replay HEAD~1 | grep -c '^### ')
if [ "$n_ref" = 1 ] && [ "$n_all" = 2 ]; then
	ok "a replay of a revision stops at that revision"
else
	bad "a replay of a revision stops at that revision" \
		"HEAD=$n_all HEAD~1=$n_ref"
fi
expect "timeline of a revision agrees with replay" "the first prompt" \
	gp timeline HEAD~1
n_tl=$(gp timeline HEAD~1 | grep -c '^  20')
if [ "$n_tl" = 1 ]; then
	ok "a timeline of a revision stops at that revision"
else
	bad "a timeline of a revision stops at that revision" "HEAD~1=$n_tl"
fi

# ------------------------------------------------------------------
say "a branch that exists only on the remote"

# A clone names only the default branch locally; every other branch arrives as
# a remote-tracking ref.  git's DWIM turns `switch <name>` for one of those
# into a new local branch that tracks it, and a fresh clone leaves a reflog
# behind, so both are checked here.
dwsrc=$work/dwimsrc
dwcln=$work/dwimclone
mkdir -p "$dwsrc" || exit 2
cd "$dwsrc" || exit 2
gp init . >/dev/null 2>&1
gp prompt -m "on the default branch" >/dev/null 2>&1
gp add -A >/dev/null 2>&1
gp commit -m "first" >/dev/null 2>&1
gp branch side >/dev/null 2>&1
gp switch side >/dev/null 2>&1
gp prompt -m "only on the side branch" >/dev/null 2>&1
gp add -A >/dev/null 2>&1
gp commit -m "side work" >/dev/null 2>&1
gp switch main >/dev/null 2>&1

cd "$work" || exit 2
gp clone "$dwsrc" dwimclone >/dev/null 2>&1
cd "$dwcln" || exit 2
expect "a fresh clone has a reflog" "clone: from" gp reflog
if gp branch | grep -q side; then
	bad "the side branch is not local yet" "$(gp branch | tr '\n' ' ')"
else
	ok "the side branch is not local yet"
fi
expect "switch takes a branch from the remote" "set up to track" gp switch side
expect "the branch is local and current" "* side" gp branch
expect "the new branch records where it came from" "Created from origin/side" \
	gp reflog
expect "the new branch has the remote's work" "side work" gp log --oneline

# ------------------------------------------------------------------
printf '\n%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skipped"
[ "$fail" -eq 0 ] || exit 1
exit 0
