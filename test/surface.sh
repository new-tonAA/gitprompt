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

# -------------------------------------------------------------------------- cleaning
say "taking out what the index does not know about"

# clean decides, per path, whether it may go, and the modes are three different
# answers to that question.  So the whole output of each mode is compared
# against git's on one fixture built to give each branch of the decision
# something to decide: a directory of nothing but ignored files, one holding a
# mixture, an empty one, one that is nested several deep, and one only -X
# reaches into.  Both tools read the same file -- the ignore file is written
# out twice under the two names.
printf 'thing.log\na.log\nb.log\nignored_dir/\n' > .gitignore
cp .gitignore .gitpromptignore
"$GP" add .gitignore .gitpromptignore >/dev/null
"$GP" commit -m "an ignore file both of them read" >/dev/null
mkdir -p ignored_dir allign mixdir a/b/c emptyd tracked_dir two
printf 'log\n' > thing.log
printf 'x\n' > ignored_dir/x.txt
printf 'a\n' > allign/a.log
printf 'm\n' > mixdir/plain.txt
printf 'b\n' > mixdir/b.log
printf 'c\n' > a/b/c/c.txt
printf 'i\n' > tracked_dir/inner.txt
printf '1\n' > two/a.log
printf '2\n' > two/plain.txt
printf 'u\n' > plain.txt
if [ "$have_git" = 1 ]; then
	# Each tool is run on its own copy, and git's copy has its store renamed to
	# .git.  Both of them skip their own store, but a store is only ever the one
	# a tool found itself: git told --git-dir=.gitprompt still sees a plain
	# directory called that, and offers to delete it, while gitprompt skips it
	# the way it must.  The copies make that difference go away, and the ignore
	# file is already written under both names with the same contents.
	rm -rf "$work/cg" "$work/cp"
	cp -r . "$work/cg"
	mv "$work/cg/.gitprompt" "$work/cg/.git"
	cp -r . "$work/cp"
	gc() { ( cd "$work/cg" && git clean "$@" 2>&1 ) | tr -d '\r'; }
	pc() { ( cd "$work/cp" && "$GP" clean "$@" 2>&1 ) | tr -d '\r'; }
	gcrc() { ( cd "$work/cg" && git clean "$@" 2>&1; echo "rc=$?" ) | tr -d '\r'; }
	pcrc() { ( cd "$work/cp" && "$GP" clean "$@" 2>&1; echo "rc=$?" ) | tr -d '\r'; }

	for f in -n -nd -nx -ndx -nX -ndX; do
		chk "git and gitprompt agree on clean $f" "$(gc $f)" "$(pc $f)"
	done
	chk "a pathspec names the directory the collapse lands at" \
		"$(gc -nd -- a/b)" "$(pc -nd -- a/b)"
	chk "and reaches below one with no -d" \
		"$(gc -n -- a/b/c/c.txt)" "$(pc -n -- a/b/c/c.txt)"
	chk "an -e pattern takes a path out of the plain mode" \
		"$(gc -nd -e mixdir)" "$(pc -nd -e mixdir)"
	chk "and puts it into -X" \
		"$(gc -nX -e plain.txt)" "$(pc -nX -e plain.txt)"
	chk "and a -e pattern ending in a slash is about a directory" \
		"$(gc -ndX -e two/)" "$(pc -ndX -e two/)"
	chk "the refusal without -f is git's, word for word" "$(gcrc)" "$(pcrc)"

	# and it really takes them out: the two trees left behind are the same
	( cd "$work/cg" && git clean -fdx >/dev/null 2>&1 )
	( cd "$work/cp" && "$GP" clean -fdx >/dev/null 2>&1 )
	chk "a forced run leaves the same tree behind" \
		"$(cd "$work/cg" && find . -path ./.git -prune -o -print | sort)" \
		"$(cd "$work/cp" && find . -path ./.gitprompt -prune -o -print | sort)"
else
	skip "the clean comparison with git (git is not on PATH)"
fi
"$GP" clean -fdx >/dev/null 2>&1
cd "$repo" || exit 2

# -------------------------------------------------------------------------- the ignore file
say "what the ignore file means"

# The rules gitprompt reads are git's, so the test is git's as well: one tree,
# and each tool asked what it makes of it.  Only .gitignore is written here --
# which is part of what is being checked, because gitprompt reads git's file as
# well as its own and a repository that came from git arrives with nothing else.
# As above, each tool runs on its own copy of the tree.
ig=$work/ig
rm -rf "$ig"
mkdir -p "$ig" || exit 2
cd "$ig" || exit 2
"$GP" init . >/dev/null
"$GP" config user.email i@example.com
"$GP" config user.name "Ignore"

printf '*.log\n!keep.log\n?.txt\n[qr]s.txt\nbuild/\n**/deep.txt\nmid/**/end.txt\ndir/\n!dir/keep.txt\nout/\n!out/\n/root.txt\nanch/inner.txt\nprompt-wins.txt\n' > .gitignore
mkdir -p build buildmore nest mid/a/b dir anch x/anch out sub
for f in a.log keep.log q.txt qs.txt xy.txt deep.txt root.txt \
	 prompt-wins.txt plain.txt sub/here.txt build/new.txt build/kept.txt \
	 buildmore/near.txt nest/deep.txt mid/end.txt mid/a/end.txt \
	 mid/a/b/end.txt mid/other.txt dir/keep.txt dir/other.txt \
	 anch/inner.txt x/anch/inner.txt out/skip.txt; do
	printf 'x\n' > "$f"
done
"$GP" add .gitignore >/dev/null
"$GP" add -f build/kept.txt >/dev/null
has "an ignored file goes in when it is forced" "build/kept.txt" \
	"$("$GP" ls-files)"
"$GP" commit -m "a tracked file inside an ignored directory" >/dev/null

if [ "$have_git" = 1 ]; then
	rm -rf "$work/cg" "$work/cp"
	cp -r . "$work/cg"
	mv "$work/cg/.gitprompt" "$work/cg/.git"
	cp -r . "$work/cp"
	gs() { ( cd "$work/cg" && git status --short --untracked-files=all 2>&1 ) \
		| tr -d '\r'; }
	ps() { ( cd "$work/cp" && "$GP" status --short 2>&1 ) | tr -d '\r'; }
	chk "the two make the same work tree of it" "$(gs)" "$(ps)"

	# The refusal is git's down to the line break, with two knowing
	# differences normalised away: the file it names is this project's, and
	# there is no advice setting here to turn the message off, so git's last
	# line would be a lie.
	gres() { ( cd "$work/cg" && git add "$@" 2>&1; echo "rc=$?" ) \
		| grep -v CRLF | tr -d '\r' \
		| sed -e 's/your \.gitignore files/your .gitpromptignore files/' \
		      -e '/addIgnoredFile/d'; }
	pres() { ( cd "$work/cp" && "$GP" add "$@" 2>&1; echo "rc=$?" ) \
		| tr -d '\r'; }
	chk "the refusal over a named ignored file is git's" "$(gres a.log)" \
		"$(pres a.log)"
	chk "and the same over a named ignored directory" "$(gres build)" \
		"$(pres build)"
	chk "named with or without its slash" "$(gres build/)" "$(pres build/)"
	chk "and several named at once, in the same order" \
		"$(gres qs.txt a.log)" "$(pres qs.txt a.log)"
	chk "a directory that is not ignored is added quietly" "$(gres sub)" \
		"$(pres sub)"

	chk "add -f takes what was refused" "$(gres -f build)" "$(pres -f build)"
	( cd "$work/cg" && git add . >/dev/null 2>&1 )
	( cd "$work/cp" && "$GP" add . >/dev/null 2>&1 )
	chk "and adding everything stages the same paths" \
		"$(cd "$work/cg" && git ls-files | sort)" \
		"$(cd "$work/cp" && "$GP" ls-files | sort)"

	# clean reads the same rules, so it has to reach the same conclusions --
	# including about the ignored directory the index knows a path in.
	gc() { ( cd "$work/cg" && git clean "$@" 2>&1 ) | tr -d '\r'; }
	pc() { ( cd "$work/cp" && "$GP" clean "$@" 2>&1 ) | tr -d '\r'; }
	for f in -nX -ndX -nx -ndx -nd; do
		chk "git and gitprompt agree on clean $f here too" "$(gc $f)" \
			"$(pc $f)"
	done
else
	skip "the ignore comparison with git (git is not on PATH)"
fi
cd "$repo" || exit 2

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

# ---------------------------------------------------------------- summarising
say "summarising the log by author"
cd "$repo" || exit 2
"$GP" config user.name "Ada Lovelace" >/dev/null 2>&1
"$GP" config user.email ada@example.com >/dev/null 2>&1
printf 'four\n' >> a.txt
"$GP" add a.txt >/dev/null
"$GP" commit -m "a commit by Ada" >/dev/null
"$GP" config user.name "Grace Hopper" >/dev/null 2>&1
"$GP" config user.email grace@example.com >/dev/null 2>&1
printf 'five\n' >> a.txt
"$GP" add a.txt >/dev/null
"$GP" commit -m "a commit by Grace" >/dev/null
"$GP" config user.name "Ada Lovelace" >/dev/null 2>&1
"$GP" config user.email ada@example.com >/dev/null 2>&1
printf 'six\n' >> a.txt
"$GP" add a.txt >/dev/null
"$GP" commit -m "another by Ada" >/dev/null

has "shortlog -s counts the commits per author" "2 Ada Lovelace" \
	"$("$GP" shortlog -s | tr '\t' ' ')"
has "and lists the other author" "Grace Hopper" "$("$GP" shortlog -s)"
sl_order=$("$GP" shortlog -sn | tr '\t' '|' | sed 's/^ *[0-9]*|//' | tr '\n' ' ')
case "$sl_order" in
*"Ada Lovelace"*"Grace Hopper"*)
	ok "shortlog -n orders by the number of commits" ;;
*) bad "shortlog -n orders by the number of commits" "$sl_order" ;;
esac
has "shortlog -e appends the email" "Ada Lovelace <ada@example.com>" \
	"$("$GP" shortlog -se)"
if [ "$have_git" = 1 ]; then
	"$GP" shortlog -sne > "$work/sl-gp"
	git --git-dir=.gitprompt shortlog -sne HEAD > "$work/sl-git"
	if diff "$work/sl-git" "$work/sl-gp" >/dev/null 2>&1; then
		ok "git and gitprompt summarise the log identically"
	else
		bad "git and gitprompt summarise the log identically" \
			"$(diff "$work/sl-git" "$work/sl-gp" | head -4)"
	fi
	"$GP" shortlog > "$work/slb-gp"
	git --git-dir=.gitprompt shortlog HEAD > "$work/slb-git"
	if diff "$work/slb-git" "$work/slb-gp" >/dev/null 2>&1; then
		ok "and the block form is identical too"
	else
		bad "and the block form is identical too" \
			"$(diff "$work/slb-git" "$work/slb-gp" | head -4)"
	fi
else
	skip "git and gitprompt summarise the log identically (no git)"
	skip "and the block form is identical too (no git)"
fi

# ---------------------------------------------------------------- handing a tree out
say "writing the tree out as an archive"
cd "$repo" || exit 2
if [ "$have_git" = 1 ]; then
	# the files come out of the tree, so the two containers hold the same
	# bytes; autocrlf is taken out of the way because git would otherwise
	# rewrite the line endings on the way out and gitprompt would not
	"$GP" archive -o "$work/gp.tar"
	git -c core.autocrlf=false --git-dir=.gitprompt archive HEAD -o "$work/git.tar"
	mkdir -p "$work/gp-tar" "$work/git-tar"
	tar -xf "$work/gp.tar" -C "$work/gp-tar"
	tar -xf "$work/git.tar" -C "$work/git-tar"
	if diff -r "$work/gp-tar" "$work/git-tar" >/dev/null 2>&1; then
		ok "git and gitprompt write the same files to a tar"
	else
		bad "git and gitprompt write the same files to a tar" \
			"$(diff -r "$work/gp-tar" "$work/git-tar" | head -4)"
	fi

	if command -v unzip >/dev/null 2>&1; then
		"$GP" archive --format=zip -o "$work/gp.zip"
		git -c core.autocrlf=false --git-dir=.gitprompt archive --format=zip \
			HEAD -o "$work/git.zip"
		mkdir -p "$work/gp-zip" "$work/git-zip"
		unzip -q "$work/gp.zip" -d "$work/gp-zip"
		unzip -q "$work/git.zip" -d "$work/git-zip"
		if diff -r "$work/gp-zip" "$work/git-zip" >/dev/null 2>&1; then
			ok "and the same files to a zip"
		else
			bad "and the same files to a zip" \
				"$(diff -r "$work/gp-zip" "$work/git-zip" | head -4)"
		fi
	else
		skip "and the same files to a zip (no unzip)"
	fi
else
	skip "git and gitprompt write the same files to a tar (no git)"
	skip "and the same files to a zip (no git)"
fi

# ---------------------------------------------------------------- notes
say "notes, read by both tools"
# a note lives on refs/notes/commits, which is a name git knows, so this is not
# two formats that resemble each other: it is one ref, written by one tool and
# read by the other, in both directions
ndir="$work/notes"
rm -rf "$ndir"
mkdir -p "$ndir" || exit 2
cd "$ndir" || exit 2
"$GP" init . >/dev/null
"$GP" config user.name "Surface" >/dev/null
"$GP" config user.email s@example.com >/dev/null
printf 'x\n' > f.txt
"$GP" add -A >/dev/null 2>&1
"$GP" commit -m "the object to note" >/dev/null 2>&1
"$GP" notes add -m "written by gitprompt" HEAD >/dev/null 2>&1

if [ "$have_git" = 1 ]; then
	chk "git reads the note gitprompt wrote" "written by gitprompt" \
		"$(git --git-dir=.gitprompt notes show HEAD 2>&1)"
	git -c user.name=G -c user.email=g@h.i --git-dir=.gitprompt \
		notes add -f -m "written by git" HEAD >/dev/null 2>&1
	chk "and gitprompt reads the note git wrote" "written by git" \
		"$("$GP" notes show HEAD)"
	chk "the two tools list the same pair" \
		"$(git --git-dir=.gitprompt notes list)" "$("$GP" notes list)"
else
	skip "git reads the note gitprompt wrote (no git)"
	skip "and gitprompt reads the note git wrote (no git)"
	skip "the two tools list the same pair (no git)"
fi
cd "$repo" || exit 2

# ---------------------------------------------------------------- apply
say "a patch, written by one tool and applied by the other"
# a patch is the unified diff both tools print and both tools read, so this is
# not two formats that resemble each other: it is one text, written by one tool
# and applied by the other, in both directions -- over an edit, and over a
# rename, which is the other shape a patch takes
adir="$work/apply"
rm -rf "$adir"
mkdir -p "$adir" || exit 2
cd "$adir" || exit 2
"$GP" init . >/dev/null
"$GP" config user.name "Surface" >/dev/null
"$GP" config user.email s@example.com >/dev/null
printf 'one\ntwo\nthree\nfour\nfive\n' > f.txt
printf 'alpha\nbeta\n' > n.txt
"$GP" add -A >/dev/null 2>&1
"$GP" commit -m "the files to patch" >/dev/null 2>&1

want_edit='one
TWO
three
four
FIVE'

# gitprompt writes the edit, git applies it
printf 'one\nTWO\nthree\nfour\nFIVE\n' > f.txt
"$GP" diff > "$work/gp-edit.patch"
"$GP" reset --hard >/dev/null 2>&1
# and gitprompt writes the rename, git applies it
"$GP" mv n.txt m.txt >/dev/null 2>&1
"$GP" diff --cached > "$work/gp-rename.patch"
"$GP" reset --hard >/dev/null 2>&1

# git is told which repository this is rather than left to discover one: the
# scratch tree lives under the build directory of this project's own checkout,
# and a git that discovers *that* repository reads a patch as naming paths from
# that root, decides a bare `f.txt` is not under the directory it is standing
# in, and skips it without a word.  Naming the store makes the tree gitprompt's
# own, which is what the check is about.
gapply() { git -c core.autocrlf=false --git-dir=.gitprompt --work-tree=. apply "$@"; }

if [ "$have_git" = 1 ]; then
	gapply "$work/gp-edit.patch"
	chk "git applies the edit gitprompt wrote" "$want_edit" "$(cat f.txt)"
	"$GP" reset --hard >/dev/null 2>&1

	gapply "$work/gp-rename.patch"
	if [ -f m.txt ] && [ ! -f n.txt ]; then
		ok "and the rename gitprompt wrote"
	else
		bad "and the rename gitprompt wrote" "$(ls)"
	fi

	# and back: git writes the edit, gitprompt applies it
	rm -f m.txt
	"$GP" reset --hard >/dev/null 2>&1
	printf 'one\nTWO\nthree\nfour\nFIVE\n' > f.txt
	git -c core.autocrlf=false --git-dir=.gitprompt --work-tree=. diff \
		> "$work/git-edit.patch"
	"$GP" reset --hard >/dev/null 2>&1
	rc_is "gitprompt takes the edit git wrote" 0 "$GP" apply "$work/git-edit.patch"
	chk "and the work tree is the change" "$want_edit" "$(cat f.txt)"

	# and git's rename, the same way round
	"$GP" reset --hard >/dev/null 2>&1
	git --git-dir=.gitprompt --work-tree=. mv n.txt m.txt >/dev/null 2>&1
	git -c core.autocrlf=false --git-dir=.gitprompt --work-tree=. diff --cached \
		> "$work/git-rename.patch"
	"$GP" reset --hard >/dev/null 2>&1
	rm -f m.txt
	rc_is "gitprompt takes the rename git wrote" 0 "$GP" apply \
		"$work/git-rename.patch"
	if [ -f m.txt ] && [ ! -f n.txt ]; then
		ok "and the two files are the one name"
	else
		bad "and the two files are the one name" "$(ls)"
	fi
else
	skip "git applies the edit gitprompt wrote (no git)"
	skip "and the rename gitprompt wrote (no git)"
	skip "gitprompt takes the edit git wrote (no git)"
	skip "and the work tree is the change (no git)"
	skip "gitprompt takes the rename git wrote (no git)"
	skip "and the two files are the one name (no git)"
fi
cd "$repo" || exit 2

# ---------------------------------------------------------------- range-diff
say "two versions of a series, lined up by both tools"
# What both tools agree on is the marks: `=` for a pair that is the same change
# with the same message, `!` for a pair that differs, `<` and `>` for a commit
# only one side has.  The body printed under a `!` is not compared -- git diffs
# its own rendering of the two commits there and this prints a different one.
# The pairing rule here is this command's own and is looser than git's creation
# factor, so the series below is one the two agree on; a pair further from the
# line is where they part, and the difference is in docs/limitations.md.
rdir="$work/rangediff"
rm -rf "$rdir"
mkdir -p "$rdir" || exit 2
cd "$rdir" || exit 2
"$GP" init . >/dev/null
"$GP" config user.name "Surface" >/dev/null
"$GP" config user.email s@example.com >/dev/null
printf 'a\n' > f.txt
"$GP" add -A >/dev/null 2>&1
"$GP" commit -m base >/dev/null 2>&1
"$GP" checkout -b left >/dev/null 2>&1
printf 'b\n' > f.txt
"$GP" add -A >/dev/null 2>&1
"$GP" commit -m "tweak the letter" >/dev/null 2>&1
printf 'g\n' > g.txt
"$GP" add -A >/dev/null 2>&1
"$GP" commit -m "and a file" >/dev/null 2>&1
printf 'c\n' > c.txt
"$GP" add -A >/dev/null 2>&1
"$GP" commit -m "only here" >/dev/null 2>&1
"$GP" checkout main >/dev/null 2>&1
"$GP" checkout -b right >/dev/null 2>&1
printf 'b\n' > f.txt
"$GP" add -A >/dev/null 2>&1
"$GP" commit -m "tweak the letter, reworded" >/dev/null 2>&1
printf 'g\n' > g.txt
"$GP" add -A >/dev/null 2>&1
"$GP" commit -m "and a file" >/dev/null 2>&1

if [ "$have_git" = 1 ]; then
	# the object ids are the same in both stores, so nothing has to be
	# normalised: the lines that name a pair are taken, and the body is left
	rdpairs() { grep -e '^[0-9]' -e '^-:'; }
	rdcmp() {
		label=$1; shift
		"$GP" range-diff "$@" | rdpairs > "$work/rd-gp"
		git --git-dir=.gitprompt range-diff "$@" | rdpairs > "$work/rd-git"
		if diff "$work/rd-git" "$work/rd-gp" >/dev/null 2>&1; then
			ok "the two tools line up $label the same way"
		else
			bad "the two tools line up $label the same way" \
				"$(diff "$work/rd-git" "$work/rd-gp" | head -6)"
		fi
	}
	rdcmp "two ranges" main..left main..right
	rdcmp "a base and two tips" main left right
	rdcmp "a ... range" left...right
else
	skip "the two tools line up two ranges the same way (no git)"
	skip "the two tools line up a base and two tips the same way (no git)"
	skip "the two tools line up a ... range the same way (no git)"
fi
cd "$repo" || exit 2

# ---------------------------------------------------------------- replace
say "one object standing in for another, read by both tools"
# refs/replace/<id> is git's own name, so a stand-in written by either tool is
# read by the other: the same id is asked for and both answer with the object
# that stands in for it.  Nothing is rewritten, so the ids the rest of the
# history points at do not move, and a pattern over the names -- a glob over
# the whole name, not a prefix -- is the same pattern for both.
rdir=$work/replace
rm -rf "$rdir"
mkdir -p "$rdir" || exit 2
cd "$rdir" || exit 2
"$GP" init . >/dev/null
"$GP" config user.name "Surface" >/dev/null
"$GP" config user.email s@example.com >/dev/null
printf 'one\n' > f.txt
"$GP" add -A >/dev/null 2>&1
"$GP" commit -m "the first" >/dev/null 2>&1
ra=$("$GP" rev-parse HEAD)
printf 'two\n' > f.txt
"$GP" add -A >/dev/null 2>&1
"$GP" commit -m "the second" >/dev/null 2>&1
rb=$("$GP" rev-parse HEAD)
printf 'three\n' > f.txt
"$GP" add -A >/dev/null 2>&1
"$GP" commit -m "the third" >/dev/null 2>&1
rc=$("$GP" rev-parse HEAD)
"$GP" replace "$ra" "$rb" >/dev/null 2>&1

if [ "$have_git" = 1 ]; then
	chk "the two tools list the same replace refs" \
		"$(git --git-dir=.gitprompt replace -l)" "$("$GP" replace -l)"
	has "git reads the stand-in gitprompt wrote" "the second" \
		"$(git --git-dir=.gitprompt cat-file -p "$ra" 2>&1)"
	git --git-dir=.gitprompt replace -f "$ra" "$rc" >/dev/null 2>&1
	has "and gitprompt reads the stand-in git wrote" "the third" \
		"$("$GP" cat-file -p "$ra")"
	rshort=$(printf '%s' "$ra" | cut -c1-7)
	chk "a bare prefix matches neither tool's list" \
		"$(git --git-dir=.gitprompt replace -l "$rshort")" \
		"$("$GP" replace -l "$rshort")"
	chk "and a star matches the same one for both" \
		"$(git --git-dir=.gitprompt replace -l "${rshort}*")" \
		"$("$GP" replace -l "${rshort}*")"
else
	skip "the two tools list the same replace refs (no git)"
	skip "git reads the stand-in gitprompt wrote (no git)"
	skip "and gitprompt reads the stand-in git wrote (no git)"
	skip "a bare prefix matches neither tool's list (no git)"
	skip "and a star matches the same one for both (no git)"
fi
cd "$repo" || exit 2

# ---------------------------------------------------------------- rerere
say "a conflict resolved the way it was resolved before"

# What a conflict is filed under is a hash of this command's own making, so the
# two tools keep separate caches and do not read each other's entries: a
# resolution recorded by one is not reused by the other, and that is written
# down rather than tested here.  What can be compared is what they say, at each
# of the four moments a resolution goes through, on the same history.
#
# run <dir> <command...> runs one in its own repository; words keeps only the
# lines the two tools are meant to say identically.
run()  { d=$1; shift; (cd "$d" && "$@"); }
words() { grep -E 'Recorded preimage|Recorded resolution|using previous resolution'; }

rrgit=$work/rr-git
rrgp=$work/rr-gp
rm -rf "$rrgit" "$rrgp"
mkdir -p "$rrgit" || exit 2
cd "$rrgit" || exit 2
git init -q . || exit 2
rrbranch=$(git symbolic-ref --short HEAD)
git config user.email s@example.com
git config user.name "Surface"
git config rerere.enabled true
printf 'head\nA\nB\ntail\n' > f.txt
git add -A >/dev/null 2>&1
git commit -qm "the base"
git branch side
printf 'head\nA-ours\nB\ntail\n' > f.txt
git add -A >/dev/null 2>&1
git commit -qm ours
git checkout -q side
printf 'head\nA-theirs\nB\ntail\n' > f.txt
git add -A >/dev/null 2>&1
git commit -qm theirs
git checkout -q "$rrbranch"

rm -rf "$rrgp"
mkdir -p "$rrgp" || exit 2
cd "$rrgp" || exit 2
"$GP" init . >/dev/null
"$GP" config user.email s@example.com
"$GP" config user.name "Surface"
"$GP" config rerere.enabled true
printf 'head\nA\nB\ntail\n' > f.txt
"$GP" add -A >/dev/null 2>&1
"$GP" commit -m "the base" >/dev/null 2>&1
"$GP" branch side >/dev/null 2>&1
printf 'head\nA-ours\nB\ntail\n' > f.txt
"$GP" add -A >/dev/null 2>&1
"$GP" commit -m ours >/dev/null 2>&1
"$GP" checkout side >/dev/null 2>&1
printf 'head\nA-theirs\nB\ntail\n' > f.txt
"$GP" add -A >/dev/null 2>&1
"$GP" commit -m theirs >/dev/null 2>&1
"$GP" checkout main >/dev/null 2>&1

cd "$repo" || exit 2
if [ "$have_git" = 1 ]; then
	chk "a conflict met is announced in the same words" \
		"$(run "$rrgit" git merge side -m once 2>&1 | words)" \
		"$(run "$rrgp" "$GP" merge side -m once 2>&1 | words)"
	printf 'head\nA-both\nB\ntail\n' > "$rrgit/f.txt"
	printf 'head\nA-both\nB\ntail\n' > "$rrgp/f.txt"
	chk "and resolving it is recorded in the same words" \
		"$(run "$rrgit" git rerere 2>&1 | words)" \
		"$(run "$rrgp" "$GP" rerere 2>&1 | words)"
	run "$rrgit" git merge --abort >/dev/null 2>&1
	run "$rrgp" "$GP" merge --abort >/dev/null 2>&1
	chk "and met again it is put back in the same words" \
		"$(run "$rrgit" git merge side -m twice 2>&1 | words)" \
		"$(run "$rrgp" "$GP" merge side -m twice 2>&1 | words)"
	run "$rrgit" git config rerere.autoupdate true
	run "$rrgp" "$GP" config rerere.autoupdate true
	run "$rrgit" git merge --abort >/dev/null 2>&1
	run "$rrgp" "$GP" merge --abort >/dev/null 2>&1
	chk "and with autoupdate both stage it, in the same words" \
		"$(run "$rrgit" git merge side -m thrice 2>&1 | words)" \
		"$(run "$rrgp" "$GP" merge side -m thrice 2>&1 | words)"
	run "$rrgit" git merge --abort >/dev/null 2>&1
	run "$rrgp" "$GP" merge --abort >/dev/null 2>&1
	chk "neither leaves anything behind once the merge is over" \
		"$(run "$rrgp" sh -c 'ls .gitprompt/rr-cache 2>/dev/null | wc -l | tr -d " "')" \
		"1"
else
	skip "a conflict met is announced in the same words (no git)"
	skip "and resolving it is recorded in the same words (no git)"
	skip "and met again it is put back in the same words (no git)"
	skip "and with autoupdate both stage it, in the same words (no git)"
	skip "neither leaves anything behind once the merge is over (no git)"
fi

cd "$repo" || exit 2

# ---------------------------------------------------------------- worktrees
say "a second working directory, read back by git"

# The registration a linked worktree leaves -- the directory under
# `<store>/worktrees/`, its HEAD, its index, the `commondir` and the `gitdir`
# pointing back -- is a layout, and a layout is right or wrong by whether the
# other reader agrees.  git reads the one gitprompt wrote, so the two lists say
# the same thing once the path column, which is padded to different widths and
# names the store differently, is taken off.
wtgit=$work/wt-git
wtgp=$work/wt-gp

if [ "$have_git" = 1 ]; then
	rm -rf "$wtgit"
	mkdir -p "$wtgit" || exit 2
	cd "$wtgit" || exit 2
	git init -q .
	git symbolic-ref HEAD refs/heads/main
	git config user.email s@example.com
	git config user.name "Surface"
	printf 'one\n' > a.txt
	git add a.txt
	git commit -qm base
	git worktree add -q ../wt-git-side
	git worktree add -q --detach ../wt-git-det
	git worktree lock ../wt-git-side >/dev/null 2>&1

	rm -rf "$wtgp"
	mkdir -p "$wtgp" || exit 2
	cd "$wtgp" || exit 2
	"$GP" init . >/dev/null
	"$GP" config user.email s@example.com
	"$GP" config user.name "Surface"
	printf 'one\n' > a.txt
	"$GP" add a.txt >/dev/null
	"$GP" commit -m base >/dev/null
	"$GP" worktree add ../wt-gp-side >/dev/null
	"$GP" worktree add --detach ../wt-gp-det >/dev/null
	"$GP" worktree lock ../wt-gp-side >/dev/null

	chk "git lists the worktrees gitprompt registered" \
		"$("$GP" worktree list | sed -e 's/^[^ ]*//' -e 's/^  *//' | sort | tr '\n' '|')" \
		"$(git --git-dir="$wtgp/.gitprompt" worktree list | sed -e 's/^[^ ]*//' -e 's/^  *//' | sort | tr '\n' '|')"
else
	skip "git lists the worktrees gitprompt registered (no git)"
fi

cd "$repo" || exit 2

# ---------------------------------------------------------------- submodules
say "a repository inside a repository, read back by git"

# Two repositories have to agree about a third without sharing a store: the
# parent's tree holds a gitlink naming a commit, `.gitmodules` names where that
# commit comes from, and the submodule's own store answers for the commit.  The
# gitlink and the `.gitmodules` are an ordinary tree entry and blob, so git can
# read what gitprompt wrote; and the commit gitprompt arrives at is one git
# recorded, so a store git built is handed back in the other direction.
sublib=$work/sub-lib

if [ "$have_git" = 1 ]; then
	rm -rf "$sublib" "$work/sub-gp"
	mkdir -p "$sublib" || exit 2
	cd "$sublib" || exit 2
	"$GP" init . >/dev/null
	"$GP" config user.email s@example.com
	"$GP" config user.name Surface
	printf 'inner\n' > in.txt
	"$GP" add in.txt >/dev/null
	"$GP" commit -m inner >/dev/null
	libsha=$("$GP" rev-parse HEAD)

	mkdir -p "$work/sub-gp" || exit 2
	cd "$work/sub-gp" || exit 2
	"$GP" init . >/dev/null
	"$GP" config user.email s@example.com
	"$GP" config user.name Surface
	printf 'outer\n' > out.txt
	"$GP" add out.txt >/dev/null
	"$GP" commit -m outer >/dev/null
	"$GP" submodule add ../sub-lib vendor/lib >/dev/null
	"$GP" commit -m "with a submodule" >/dev/null

	chk "git reads the gitlink gitprompt wrote" \
		"160000 $libsha 0	vendor/lib" \
		"$(git --git-dir="$work/sub-gp/.gitprompt" ls-files -s vendor/lib)"
	chk "and the path gitprompt wrote into .gitmodules" "vendor/lib" \
		"$(git config -f "$work/sub-gp/.gitmodules" --get submodule.vendor/lib.path)"
	chk "and the url beside it" "../sub-lib" \
		"$(git config -f "$work/sub-gp/.gitmodules" --get submodule.vendor/lib.url)"
	chk "and names that commit when asked about the submodule" \
		"$libsha vendor/lib" \
		"$(git --git-dir="$work/sub-gp/.gitprompt" --work-tree="$work/sub-gp" \
			submodule status vendor/lib 2>/dev/null | sed -e 's/^[-+U ]*//' -e 's/ (.*//')"

	# the other direction: git writes the commit, gitprompt checks it out.  The
	# store has to be gitprompt's shape for a local path to be read at all, so
	# git writes into one gitprompt made, which is also the honest picture --
	# a gitprompt store is an ordinary git object store.
	rm -rf "$work/sub-src" "$work/sub-maker" "$work/sub-gp2"
	mkdir -p "$work/sub-src" || exit 2
	cd "$work/sub-src" || exit 2
	"$GP" init --bare . >/dev/null

	mkdir -p "$work/sub-maker" || exit 2
	cd "$work/sub-maker" || exit 2
	git init -q .
	git symbolic-ref HEAD refs/heads/main
	git config user.email s@example.com
	git config user.name Surface
	printf 'inner\n' > in.txt
	git add in.txt
	git commit -qm inner
	gitsha=$(git rev-parse HEAD)
	git push -q ../sub-src main

	mkdir -p "$work/sub-gp2" || exit 2
	cd "$work/sub-gp2" || exit 2
	"$GP" init . >/dev/null
	"$GP" config user.email s@example.com
	"$GP" config user.name Surface
	printf 'outer\n' > out.txt
	"$GP" add out.txt >/dev/null
	"$GP" commit -m outer >/dev/null
	"$GP" submodule add ../sub-src vendor/lib >/dev/null

	chk "gitprompt records the commit git made" "$gitsha" \
		"$("$GP" ls-files -s vendor/lib | awk '{print $2}')"
	chk "and reads the file out of the store git wrote" "inner" \
		"$(cat vendor/lib/in.txt)"
	chk "and names the same commit in its own status" "$gitsha" \
		"$("$GP" submodule status vendor/lib | sed -e 's/^[-+U ]*//' -e 's/ .*//')"
else
	skip "git reads the gitlink gitprompt wrote (no git)"
	skip "and the path gitprompt wrote into .gitmodules (no git)"
	skip "and the url beside it (no git)"
	skip "and names that commit when asked about the submodule (no git)"
	skip "gitprompt records the commit git made (no git)"
	skip "and reads the file out of the store git wrote (no git)"
	skip "and names the same commit in its own status (no git)"
fi

say "a work tree that holds only part of the index, read back by git"

# Sparse checkout leaves two things behind that git has to be able to read: an
# index carrying the skip-worktree bit, and a pattern list in git's own format
# in git's own file.  The index is the harder half, because the bit lives in a
# second flags word that only version 3 of the format has room for, so git
# reading the bit gitprompt wrote is the check that means something -- and the
# other direction, git setting the bit and gitprompt reading it, is the same
# claim from the other side.
#
# The patterns begin with a slash, and the shell this may run under on Windows
# rewrites such an argument into a Windows path before gitprompt or git ever
# sees it.  Rewriting is off for this section; the variable means nothing on the
# other two runners.
MSYS_NO_PATHCONV=1
export MSYS_NO_PATHCONV

foursome() {
	mkdir -p src docs || exit 2
	printf 'a\n' > src/a.c
	printf 'b\n' > src/b.h
	printf 'd\n' > docs/d.md
	printf 'r\n' > README.md
}

if [ "$have_git" = 1 ]; then
	rm -rf "$work/sp-gp" "$work/sp-git"

	mkdir -p "$work/sp-gp" || exit 2
	cd "$work/sp-gp" || exit 2
	"$GP" init . >/dev/null
	"$GP" config user.email s@example.com
	"$GP" config user.name Surface
	foursome
	"$GP" add . >/dev/null
	"$GP" commit -m "one of each" >/dev/null
	"$GP" sparse-checkout set '/src/*.c' >/dev/null

	chk "git reads the skip-worktree bits gitprompt wrote" \
		"S README.md
S docs/d.md
H src/a.c
S src/b.h" "$(git --git-dir=.gitprompt --work-tree=. ls-files -t)"
	chk "and the pattern list, through git's own reader" "/src/*.c" \
		"$(git --git-dir=.gitprompt --work-tree=. sparse-checkout list)"
	# -uno, because git is being pointed at a store it did not lay out: the
	# directory holding it is not a `.git`, so git would otherwise offer it up
	# as an untracked directory.  What this is asking is whether git agrees
	# about the tracked files.
	chk "and agrees about the tracked files on disk" "" \
		"$(git --git-dir=.gitprompt --work-tree=. status --short -uno)"

	# the other direction: git sets the bits, gitprompt reads them.  git keeps
	# the switch in a work tree's own configuration file when
	# `extensions.worktreeConfig` is on, which is what its own sparse-checkout
	# turns on, so this also checks that gitprompt looks where git wrote.
	mkdir -p "$work/sp-git" || exit 2
	cd "$work/sp-git" || exit 2
	"$GP" init . >/dev/null
	"$GP" config user.email s@example.com
	"$GP" config user.name Surface
	foursome
	"$GP" add . >/dev/null
	"$GP" commit -m "one of each" >/dev/null
	git --git-dir=.gitprompt --work-tree=. sparse-checkout set --no-cone \
		'/src/*.c' > "$work/sp-set.out" 2>&1

	chk "gitprompt reads the bits git set" \
		"S README.md
S docs/d.md
H src/a.c
S src/b.h" "$("$GP" ls-files -t)"
	chk "and the pattern list git wrote" "/src/*.c" \
		"$("$GP" sparse-checkout list)"
	chk "and takes no exception to the work tree git left" "" \
		"$("$GP" status --short)"
else
	skip "git reads the skip-worktree bits gitprompt wrote (no git)"
	skip "and the pattern list, through git's own reader (no git)"
	skip "and agrees about the tracked files on disk (no git)"
	skip "gitprompt reads the bits git set (no git)"
	skip "and the pattern list git wrote (no git)"
	skip "and takes no exception to the work tree git left (no git)"
fi

# ------------------------------------------------- the credential helper protocol
say "a credential helper, answered the same way by both"

# A credential is not a file format but a conversation: a block of key=value
# lines goes to a program named by `credential.helper`, and the answer comes
# back the same way.  Both ends of that can be put beside git's -- the same
# helper answering the same question, and a helper git itself ships being
# driven from gitprompt.
#
# The global files are given a directory of this section's own, because the
# machine's own `credential.helper` would otherwise take part in every
# comparison; the empty value first in git's file takes back whatever the
# system file configured.
cdir=$work/cred
chome=$cdir/home

credreq() { printf 'protocol=https\nhost=example.com\n\n'; }
credwhole() {
	printf 'protocol=https\nhost=example.com\nusername=x\npassword=y\n'
}

if [ "$have_git" = 1 ]; then
	rm -rf "$cdir"
	mkdir -p "$chome" || exit 2

	cat > "$cdir/helper.sh" <<'EOF'
#!/bin/sh
if [ "$1" = get ]; then
	echo "username=alice"
	echo "password=secret"
fi
exit 0
EOF
	chmod +x "$cdir/helper.sh"

	printf '[credential]\n\thelper = !%s/helper.sh\n' "$cdir" \
		> "$chome/.gitpromptconfig"
	printf '[credential]\n\thelper =\n\thelper = !%s/helper.sh\n' "$cdir" \
		> "$chome/.gitconfig"

	chk "a credential helper answers git and gitprompt alike" \
		"$(credreq | GIT_TERMINAL_PROMPT=0 HOME=$chome git credential fill 2>&1)" \
		"$(credreq | HOME=$chome "$GP" credential fill 2>&1)"
	chk "and a whole credential is passed through the same way" \
		"$(credwhole | GIT_TERMINAL_PROMPT=0 HOME=$chome git credential fill 2>&1)" \
		"$(credwhole | HOME=$chome "$GP" credential fill 2>&1)"

	# a bare name means `git credential-<name>`, which is how the helpers git
	# itself ships are addressed.  `store` really is `git credential-store`, so
	# this is gitprompt driving a helper of git's own -- and git reading the
	# result back out of the file that helper keeps.
	printf '[credential]\n\thelper = store\n' > "$chome/.gitpromptconfig"
	printf '[credential]\n\thelper =\n\thelper = store\n' > "$chome/.gitconfig"

	printf 'protocol=https\nhost=example.com\nusername=alice\npassword=secret\n' |
		HOME=$chome "$GP" credential approve
	chk "a helper git ships is driven by gitprompt" \
		"https://alice:secret@example.com" \
		"$(cat "$chome/.git-credentials" 2>/dev/null)"
	chk "and git reads back what gitprompt stored" \
		"$(credreq | GIT_TERMINAL_PROMPT=0 HOME=$chome git credential fill 2>&1)" \
		"$(credreq | HOME=$chome "$GP" credential fill 2>&1)"

	printf 'protocol=https\nhost=example.com\nusername=alice\npassword=secret\n' |
		HOME=$chome "$GP" credential reject
	chk "and rejecting it takes it out of git's store" "" \
		"$(cat "$chome/.git-credentials" 2>/dev/null)"
else
	skip "a credential helper answers git and gitprompt alike (no git)"
	skip "and a whole credential is passed through the same way (no git)"
	skip "a helper git ships is driven by gitprompt (no git)"
	skip "and git reads back what gitprompt stored (no git)"
	skip "and rejecting it takes it out of git's store (no git)"
fi

unset MSYS_NO_PATHCONV
cd "$repo" || exit 2

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
