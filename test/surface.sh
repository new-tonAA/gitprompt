#!/bin/sh
#
# surface.sh -- does gitprompt do git's work, on prompts?
#
#   sh test/surface.sh
#   GP=/path/to/gitprompt sh test/surface.sh
#
# Every check here is the same operation git does, run against a history of
# prompts rather than a history of code: nothing is asserted about a command
# merely existing, each one has to produce the thing git produces.  smoke.sh
# asks each command in depth; this asks the whole surface at once, in the order
# a user would meet it, so that a command that works on its own but not in the
# sequence it is actually used in is caught here.
#
# Where a check cannot be made without git itself -- reading the store with
# git's own reader, a remote, a clone -- it is skipped rather than faked when
# git is not on PATH.  Everything is POSIX: this runs on the macOS runner too,
# where sed and grep are BSD and `\|` is not an alternation.
#
# It found two things smoke.sh did not (a merge that carried no prompts, and a
# reflog that forgot the past after a checkout); both are fixed and have
# regression checks in smoke.sh as well, which is what a battery like this is
# for.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
GP=${GP:-$root/gitprompt.exe}
[ -f "$GP" ] || GP=$root/gitprompt
[ -f "$GP" ] || { echo "no gitprompt binary at $GP; run make first"; exit 2; }

case "${WORK:-}" in
"") work=$root/build/surface ;;
*)  work=$WORK ;;
esac
rm -rf "$work"
mkdir -p "$work" || exit 2

pass=0
fail=0
skipped=0

ok()   { pass=$((pass + 1)); printf 'ok   %s\n' "$1"; }
bad()  { fail=$((fail + 1)); printf 'FAIL %s\n' "$1"; [ $# -gt 1 ] && printf '     %s\n' "$2"; return 0; }
skip() { skipped=$((skipped + 1)); printf 'skip %s\n' "$1"; }
chk()  { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "want [$2] got [$3]"; fi; }
has()  { case "$3" in *"$2"*) ok "$1" ;; *) bad "$1" "no [$2] in: $3" ;; esac; }
rc_is() { # rc_is NAME RC CMD...
	name=$1; want=$2; shift 2
	"$@" > "$work/out" 2>&1; got=$?
	chk "$name" "$want" "$got"
}
say() { printf '\n-- %s\n' "$1"; }

# the prompt ids a commit carries, from the header `show` prints
pcount() { "$GP" show "$1" | sed -n 's/^Prompts: //p' | grep -oE 'p_[a-z0-9]+' | wc -l | tr -d ' '; }

have_git=0
command -v git >/dev/null 2>&1 && have_git=1

if [ "$have_git" = 1 ]; then
	git init --bare -q "$work/remote.git" || exit 2
	git --git-dir="$work/remote.git" symbolic-ref HEAD refs/heads/main
fi

repo=$work/r
mkdir -p "$repo" || exit 2
cd "$repo" || exit 2

# ---------------------------------------------------------------- the object model
say "the object model"
"$GP" init . >/dev/null
"$GP" config user.email s@example.com
"$GP" config user.name "Surface"
"$GP" session start -t "the surface" >/dev/null
"$GP" prompt -m "write the first file" >/dev/null
printf 'one\n' > a.txt
"$GP" add a.txt >/dev/null
has "add puts a file in the index" "A  a.txt" "$("$GP" status --short)"
"$GP" prompt -m "and then a second" >/dev/null
"$GP" commit -m "first" >/dev/null
chk "commit makes a commit" "1" "$("$GP" log --oneline | wc -l | tr -d ' ')"
head40=$("$GP" rev-parse HEAD)
chk "rev-parse gives a 40 digit id" "40" "$(printf '%s' "$head40" | wc -c | tr -d ' ')"
chk "cat-file says what it is" "commit" "$("$GP" cat-file -t HEAD)"
tree=$("$GP" cat-file -p HEAD | awk '/^tree/ { print $2 }')
chk "the commit names a tree" "40" "$(printf '%s' "$tree" | wc -c | tr -d ' ')"
chk "write-tree rebuilds the same tree from the index" "$tree" "$("$GP" write-tree)"
has "ls-tree lists the tree" "a.txt" "$("$GP" ls-tree HEAD)"
has "and the prompt directory with it" "prompts" "$("$GP" ls-tree HEAD)"
has "ls-files lists the index" "a.txt" "$("$GP" ls-files)"
blob=$("$GP" hash-object -w a.txt)
chk "hash-object stores a blob" "blob" "$("$GP" cat-file -t "$blob")"
chk "and its id is the content's" "$blob" "$("$GP" hash-object a.txt)"
"$GP" update-ref refs/heads/marker "$head40"
has "update-ref moves a ref" "refs/heads/marker" "$("$GP" for-each-ref)"
chk "symbolic-ref reads HEAD" "refs/heads/main" "$("$GP" symbolic-ref HEAD)"
rc_is "check-ref-format accepts a good name" 0 "$GP" check-ref-format refs/heads/fine
if "$GP" check-ref-format 'refs/heads/two..dots' >/dev/null 2>&1; then
	bad "check-ref-format refuses a bad name" "it accepted refs/heads/two..dots"
else
	ok "check-ref-format refuses a bad name"
fi
if [ "$have_git" = 1 ]; then
	has "git itself can read the store" "first" \
		"$(git --git-dir=.gitprompt log --oneline --all | head -3)"
else
	skip "git itself can read the store (git is not on PATH)"
fi

# ---------------------------------------------------------------- history and branches
say "history and branches"
"$GP" checkout -b feature >/dev/null 2>&1
"$GP" branch | grep '^\*' > "$work/br"
chk "checkout -b switches to a new branch" "* feature" "$(cat "$work/br")"
"$GP" prompt -m "the feature prompt" >/dev/null
printf 'two\n' >> a.txt
"$GP" add a.txt >/dev/null
"$GP" commit -m "work on the feature" >/dev/null
chk "the branch has diverged" "2" "$("$GP" log --oneline | wc -l | tr -d ' ')"
"$GP" checkout main >/dev/null 2>&1
chk "and main did not move" "1" "$("$GP" log --oneline | wc -l | tr -d ' ')"
chk "the file came back to main's content" "one" "$(cat a.txt)"

"$GP" merge feature -m "join the feature" > "$work/merge.log" 2>&1
rc=$?
chk "merge succeeds" "0" "$rc"
has "a fast-forward merge says so" "Fast-forward" "$(cat "$work/merge.log")"
chk "and moves the branch onto the other" "2" \
	"$("$GP" log --oneline | wc -l | tr -d ' ')"
chk "and it carries the prompt it was made from" "1" "$(pcount HEAD)"
chk "the work is in the tree" "one
two" "$(cat a.txt | tr -d '\r')"

# a real merge, with a second parent, on work that does not collide
"$GP" checkout -b mtwo main >/dev/null 2>&1
"$GP" prompt -m "the side branch's prompt" >/dev/null
printf 'b\n' > b.txt
"$GP" add b.txt >/dev/null
"$GP" commit -m "the side branch" >/dev/null
"$GP" checkout main >/dev/null 2>&1
printf 'three\n' >> a.txt
"$GP" add a.txt >/dev/null
"$GP" commit -m "the mainline" >/dev/null
"$GP" merge mtwo --no-ff -m "a real merge" > "$work/noff.log" 2>&1
rc=$?
chk "merge --no-ff succeeds" "0" "$rc"
chk "and makes a second parent" "2" \
	"$("$GP" cat-file -p HEAD | grep -c '^parent')"
# the merge brings the side branch's prompt file into main's tree, and the first
# parent -- the branch the merge was made on -- does not hold it, so the merge
# carries it: the rule every commit follows, read against that first parent
chk "the merge commit carries the prompt it brought in" "1" "$(pcount HEAD)"
has "and it does not drop the one it merged in" "the side branch's prompt" \
	"$("$GP" log-prompt)"
"$GP" branch -d mtwo >/dev/null 2>&1

"$GP" tag v1 >/dev/null 2>&1
has "tag names a commit" "v1" "$("$GP" tag -l)"
chk "describe finds the nearest tag" "v1" "$("$GP" describe HEAD)"
has "diff --stat shows what the merge brought in" "b.txt" \
	"$("$GP" diff --stat HEAD~1 HEAD)"

n=$("$GP" reflog | wc -l | tr -d ' ')
if [ "$n" -ge 3 ]; then
	ok "reflog lists what HEAD has pointed at"
else
	bad "reflog lists what HEAD has pointed at" "$n entries"
fi
has "and says what moved it" "merge" "$("$GP" reflog)"

# git keeps HEAD's reflog in its own file, so a switch adds to the record
# rather than hiding it; this is the one place the battery expects git's
# behaviour and reports what it finds instead
"$GP" checkout -b reflogprobe >/dev/null 2>&1
n2=$("$GP" reflog | wc -l | tr -d ' ')
if [ "$n2" -ge "$n" ]; then
	ok "reflog still shows the past after a branch switch"
else
	bad "reflog still shows the past after a branch switch" \
		"$n entries before the switch, $n2 after"
fi
"$GP" checkout main >/dev/null 2>&1
"$GP" branch -d reflogprobe >/dev/null 2>&1

"$GP" reset --soft HEAD~1 >/dev/null 2>&1
has "reset --soft moves HEAD back to the first parent" "mainline" \
	"$("$GP" log --oneline | head -1)"
chk "and the merge is no longer reachable" "3" \
	"$("$GP" log --oneline | wc -l | tr -d ' ')"
has "and the merged work is left staged" "b.txt" "$("$GP" status --short)"
"$GP" commit -m "the merge again" >/dev/null

# a conflict is reported and can be taken back
"$GP" checkout -b other HEAD~1 >/dev/null 2>&1
printf 'theirs\n' > a.txt
"$GP" add a.txt >/dev/null
"$GP" commit -m "a different line" >/dev/null
"$GP" checkout main >/dev/null 2>&1
printf 'ours\n' > a.txt
"$GP" add a.txt >/dev/null
"$GP" commit -m "our line" >/dev/null
"$GP" merge other -m "should not apply" > "$work/conflict.log" 2>&1
rc=$?
if [ "$rc" = 0 ]; then
	bad "a conflicting merge is refused" "it exited 0"
else
	ok "a conflicting merge is refused"
fi
has "and it says what is in the way" "a.txt" "$(cat "$work/conflict.log")"
"$GP" merge --abort >/dev/null 2>&1
chk "merge --abort leaves the work tree with our line" "ours" "$(cat a.txt | tr -d '\r')"
"$GP" branch -d other >/dev/null 2>&1
chk "branch -d deletes a branch" "0" "$("$GP" branch | grep -c 'other')"

# ---------------------------------------------------------------- the work tree
say "the work tree"
printf 'mv me\n' > old.txt
"$GP" add old.txt >/dev/null
"$GP" commit -m "add old.txt" >/dev/null
"$GP" mv old.txt new.txt >/dev/null 2>&1
chk "mv renames the file on disk" "1" \
	"$([ -f new.txt ] && [ ! -f old.txt ] && echo 1 || echo 0)"
has "and stages it" "new.txt" "$("$GP" status --short)"
"$GP" commit -m "rename it" >/dev/null
"$GP" rm new.txt >/dev/null 2>&1
chk "rm takes the file away" "1" "$([ ! -f new.txt ] && echo 1 || echo 0)"
chk "and takes it out of the index" "0" "$("$GP" ls-files | grep -c new.txt)"
"$GP" commit -m "remove it" >/dev/null

printf 'edited by hand\n' > a.txt
"$GP" checkout -- a.txt >/dev/null 2>&1
chk "checkout -- brings a file back from the index" "ours" \
	"$(tail -1 a.txt | tr -d '\r')"
printf 'not wanted\n' > a.txt
"$GP" reset --hard >/dev/null 2>&1
chk "reset --hard throws the hand edit away" "ours" \
	"$(tail -1 a.txt | tr -d '\r')"

# -------------------------------------------------------------------------- searching
say "searching"
# A pattern language is the one part of grep the user has already learned
# somewhere else, so what is asserted is not that a pattern works but that it
# means what git means by it: basic by default, extended under -E, literal under
# -F -- and that the exit status is the one a script would branch on.
printf 'alpha beta\nGamma delta\nfoo+bar\nfoobar\n' > s.txt
"$GP" add s.txt >/dev/null
"$GP" commit -m "a file to search" >/dev/null
chk "grep finds a line and names the file" "s.txt:alpha beta" \
	"$("$GP" grep alpha -- s.txt)"
chk "and -n numbers it" "s.txt:1:alpha beta" "$("$GP" grep -n beta -- s.txt)"
chk "a + is an ordinary byte in a basic pattern" "s.txt:foo+bar" \
	"$("$GP" grep 'foo+bar' -- s.txt)"
chk "so -E is what makes it repeat" "s.txt:foobar" \
	"$("$GP" grep -E 'fo+bar' -- s.txt)"
chk "-F reads the pattern as nothing but bytes" "s.txt:foo+bar" \
	"$("$GP" grep -F 'foo+bar' -- s.txt)"
chk "-i ignores case" "s.txt:Gamma delta" "$("$GP" grep -i gamma -- s.txt)"
rc_is "a hit is exit 0" 0 "$GP" grep alpha -- s.txt
rc_is "nothing found is exit 1" 1 "$GP" grep zzzz -- s.txt
rc_is "a pattern that will not compile is exit 128" 128 "$GP" grep 'a\{2\}' -- s.txt
if [ "$have_git" = 1 ]; then
	G="git --git-dir=.gitprompt --work-tree=."
	chk "git reads the same lines from the same tree" \
		"$($G grep -n alpha -- s.txt | tr -d '\r')" \
		"$("$GP" grep -n alpha -- s.txt | tr -d '\r')"
	chk "and reads a basic pattern the same way" \
		"$($G grep -n 'foo+bar' -- s.txt | tr -d '\r')" \
		"$("$GP" grep -n 'foo+bar' -- s.txt | tr -d '\r')"
	chk "and names the file once under -l" "$($G grep -l alpha -- s.txt)" \
		"$("$GP" grep -l alpha -- s.txt)"
	rc_is "and both call nothing found exit 1" 1 $G grep zzzz -- s.txt
else
	skip "the grep comparison with git (git is not on PATH)"
fi

# -------------------------------------------------------------------------- halving
say "halving a range to find the first bad commit"

# bisect answers with a commit, but it earns that answer by looking at a
# sequence of them, and two implementations that settle on the same commit can
# still have asked about different ones on the way.  So the walk itself is put
# beside git's, over the same eight commits: the ids and the commit dump after
# the answer come out, and what is left is how the range was halved.
if [ "$have_git" = 1 ]; then
	bsg=$work/bs-git
	bsr=$work/bs-gp

	build_bs() {
		mkdir -p "$2" || exit 2
		cd "$2" || exit 2
		"$1" init . >/dev/null 2>&1
		"$1" config user.name "Surface" >/dev/null 2>&1
		"$1" config user.email s@example.com >/dev/null 2>&1
		i=1
		while [ $i -le 8 ]; do
			if [ $i -lt 5 ]; then printf 'fine %d\n' "$i" > f.txt
			else printf 'broken %d\n' "$i" > f.txt; fi
			"$1" add f.txt >/dev/null 2>&1
			"$1" commit -m "step $i" >/dev/null 2>&1
			i=$((i + 1))
		done
	}

	# one probe at a time until it says which commit it settled on
	walk() {
		o=$("$1" bisect start HEAD HEAD~7 2>&1)
		printf '%s\n' "$o"
		n=0
		while [ $n -lt 12 ]; do
			n=$((n + 1))
			if grep -q '^broken' f.txt; then step=bad; else step=good; fi
			o=$("$1" bisect "$step" 2>&1)
			printf '%s\n' "$o"
			case "$o" in
			*"is the first"*commit*) break ;;
			esac
		done
	}

	# two repositories built at different times have different ids, and the
	# dump of the commit it names carries a date, so both are taken out.  The
	# sentence itself is matched by shape rather than by wording: git began
	# quoting the term -- "is the first 'bad' commit" -- somewhere between 2.49
	# and 2.55, and what is being compared here is the walk, not the prose.
	strip_ids() {
		sed -n '1,/is the first .*commit/p' |
		sed 's/^\[[^]]*\]/[<id>]/' |
		sed 's/^[0-9a-f][0-9a-f]* is the first .*commit/<id> is the first bad commit/' |
		tr -d '\r'
	}

	build_bs git "$bsg"
	walk git | strip_ids > "$work/bs-git.txt"
	build_bs "$GP" "$bsr"
	walk "$GP" | strip_ids > "$work/bs-gp.txt"
	cd "$repo" || exit 2
	chk "git and gitprompt halve the same history the same way" \
		"$(cat "$work/bs-git.txt")" "$(cat "$work/bs-gp.txt")"
else
	skip "the bisect comparison with git (git is not on PATH)"
fi

# ---------------------------------------------------------------- integrity
say "integrity and maintenance"
rc_is "verify-objects passes" 0 "$GP" verify-objects
rc_is "fsck passes" 0 "$GP" fsck
rc_is "gc runs" 0 "$GP" gc
has "count-objects reports something" "object" "$("$GP" count-objects)"
has "stats summarises the history" "prompt" "$("$GP" stats)"

# ---------------------------------------------------------------- remotes
say "remotes"
if [ "$have_git" = 1 ]; then
	"$GP" remote add origin "$work/remote.git" >/dev/null 2>&1
	has "remote -v lists it" "origin" "$("$GP" remote -v)"
	if "$GP" push -u origin main > "$work/push.log" 2>&1; then
		ok "push sends the history"
	else
		bad "push sends the history" "$(cat "$work/push.log")"
	fi
	if "$GP" push --tags > "$work/tags.log" 2>&1; then
		ok "push --tags sends the tag"
	else
		bad "push --tags sends the tag" "$(cat "$work/tags.log")"
	fi

	cd "$work" || exit 2
	"$GP" clone "$work/remote.git" got > "$work/clone.log" 2>&1
	rc=$?
	chk "clone makes a working repository" "0" "$rc"
	cd "$work/got" || exit 2
	chk "with the same history" "$("$GP" log --oneline | wc -l | tr -d ' ')" \
		"$(cd "$repo" && "$GP" log --oneline | wc -l | tr -d ' ')"
	chk "and the prompts with it" "4" "$("$GP" log-prompt --oneline | wc -l | tr -d ' ')"
	has "and the tag came too" "v1" "$("$GP" tag -l)"

	"$GP" config user.email g@example.com
	"$GP" config user.name "Got"
	"$GP" prompt -m "a prompt from the clone" >/dev/null
	printf 'three\n' >> a.txt
	"$GP" add a.txt >/dev/null
	"$GP" commit -m "from the clone" >/dev/null
	"$GP" push >/dev/null 2>&1
	cd "$repo" || exit 2
	"$GP" fetch >/dev/null 2>&1
	if "$GP" pull > "$work/pull.log" 2>&1; then
		ok "pull brings the clone's work back"
	else
		bad "pull brings the clone's work back" "$(cat "$work/pull.log")"
	fi
	has "and the prompt came with it" "a prompt from the clone" "$("$GP" log-prompt)"
else
	skip "the remotes section (git is not on PATH)"
fi

# ---------------------------------------------------------------- the prompt layer
say "what git has no equivalent for"
has "replay reconstructs the history as one document" "write the first file" \
	"$("$GP" replay --flat)"
has "timeline lists it compactly" "write the first file" "$("$GP" timeline)"
rc_is "attach prints what an agent reads" 0 "$GP" attach --dry-run
has "and says what it is" "gitprompt attach: generated" "$("$GP" attach --dry-run)"
has "session show prints the conversation" "write the first file" \
	"$("$GP" session show "$("$GP" session list | sed -n 's/^[* ] *\(s_[^ ]*\).*/\1/p' | head -1)")"

printf '\n%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skipped"
[ "$fail" = 0 ]
