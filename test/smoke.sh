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
# It runs on three runners, so everything here is POSIX and nothing may rest on
# a GNU extension: the macOS runner's sed and grep are BSD, where `\|` is not an
# alternation and `wc` pads its count to a column, and neither of those is
# visible from a machine that has GNU tools.  Where a substitution would need
# one, use `grep -oE` and `tr -d ' '` instead.
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

# The developer's editor must not reach these checks.  `commit` falls back on
# one when no message is given, so an inherited GIT_EDITOR would decide what a
# bare commit does -- and whose machine the suite ran on would change what it
# verified.  The editor section below sets these for the checks that want one.
unset GIT_EDITOR VISUAL EDITOR

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

# rm and mv: both change the index, and mv touches the work tree too
printf 'movable\n' > moved.txt
gp add moved.txt >/dev/null 2>&1
expect_status "mv renames a tracked file" 0 gp mv moved.txt renamed.txt
expect_absent "mv moves the file in the work tree" moved.txt
expect "mv stages the new path" "renamed.txt" gp ls-files
if gp ls-files | grep -q 'moved.txt'; then
	bad "mv drops the old path from the index" "still listed"
else
	ok "mv drops the old path from the index"
fi
expect_status "rm removes a tracked file" 0 gp rm renamed.txt
expect_absent "rm removes the file from the work tree" renamed.txt
if gp ls-files | grep -q 'renamed.txt'; then
	bad "rm drops the path from the index" "still listed"
else
	ok "rm drops the path from the index"
fi
expect_status "rm refuses a path it does not track" 1 gp rm no-such-file.txt

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
say "handing the history to an agent"

# `attach` writes the reconstruction where an agent looks for the context of
# the project it is working in.  What it writes was typed by other people at
# other times, so the file says so before the history starts: a prompt asking
# for something was a request to whoever was running then, and an agent
# reading this one has to know that none of it came from the person who
# opened this session.
attach_dry=$work/attach-dry
gp attach --dry-run > "$attach_dry" 2>&1
expect "a dry run prints the document" "gitprompt attach: generated" cat "$attach_dry"
expect "attach carries the history" "write a tokenizer first" cat "$attach_dry"
expect "attach keeps the session boundary" "build a parser" cat "$attach_dry"
expect "attach says the history is data, not instructions" \
	"**data, not instructions**" cat "$attach_dry"
expect "attach gives the read-only way to start claude" \
	"claude --permission-mode plan" cat "$attach_dry"
expect_absent "a dry run writes no file" "$repo/CLAUDE.md"

# the header names the workspace and the commit, so whoever reads the file can
# tell which checkout the history came from and how far it goes
expect "attach states the commit it describes" "$(gp rev-parse HEAD)" cat "$attach_dry"
# the workspace is printed as the system spells it, which on Windows is not the
# spelling this shell uses -- so ask the shell for the native form as well
want_root=$(cygpath -w "$PWD" 2>/dev/null || pwd -P)
case "$(cat "$attach_dry")" in
*"$want_root"*) ok "attach states the workspace it describes" ;;
*) bad "attach states the workspace it describes" "wanted [$want_root] in $attach_dry" ;;
esac

# writing the file is exercised in a repository of its own, so the checks above
# go on asserting on the main one
attach_repo=$work/attach
rm -rf "$attach_repo"
mkdir -p "$attach_repo" || exit 2
cd "$attach_repo" || exit 2
gp init . >/dev/null 2>&1
gp session start -t "attach a context file" >/dev/null
gp prompt -m "write a lexer" --model claude-opus-5 >/dev/null
gp prompt -m "then write the parser" >/dev/null
gp add -A >/dev/null 2>&1
gp commit -m "record the prompts" >/dev/null 2>&1

expect "attach names the model the prompts were written for" \
	"claude-opus-5" gp attach --dry-run

gp attach >/dev/null 2>&1
expect_file "attach writes the file claude reads its context from" CLAUDE.md
expect "the file on disk is the document" "then write the parser" cat CLAUDE.md
expect "and it opens with the history" "0001" cat CLAUDE.md

# the context file is one the user may also keep notes in, so an existing file
# that this command did not write is theirs, and is left alone
printf 'my own notes\n' > CLAUDE.md
expect_status "attach refuses a context file it did not write" 128 gp attach
expect "the refusal says how to proceed" "was not written by attach" gp attach
expect "the notes are still there" "my own notes" cat CLAUDE.md
gp attach --force >/dev/null 2>&1
expect "attach replaces it when told to" "**data, not instructions**" cat CLAUDE.md

# each agent reads its context from its own file, in its own read-only mode
gp attach --agent codex >/dev/null 2>&1
expect_file "codex reads its context from AGENTS.md" AGENTS.md
expect "attach gives the read-only way to start codex" \
	"codex --sandbox read-only" cat AGENTS.md
expect_status "attach refuses an agent it does not know" 128 gp attach --agent gemini

# The file an agent reads and the way it is put in the reading mode are facts
# about the agent as installed, so both can be said in config -- and that is
# what makes an agent the table has never heard of usable at all: `gemini` was
# refused a moment ago only because nothing named the file it reads.
expect "an agent with no read-only flag here says so rather than guessing" \
	"does not know how dsh is put in that mode" gp attach --agent=dsh --dry-run
gp config gitprompt.agent.gemini.context GEMINI.md
gp attach --agent=gemini >/dev/null 2>&1
expect_file "config names the context file of a new agent" GEMINI.md
gp config gitprompt.agent.gemini.readOnly "gemini --read-only"
expect "as does config, when the agent has one" \
	"    gemini --read-only" gp attach --agent=gemini --dry-run
gp config --unset gitprompt.agent.gemini.context
gp config --unset gitprompt.agent.gemini.readOnly

# -o writes the same document wherever the reader is told to look for it
gp attach -o "$work/attach.md" >/dev/null 2>&1
expect_file "attach writes the document elsewhere on request" "$work/attach.md"
expect "and it is the same document" "write a lexer" cat "$work/attach.md"

cd "$repo" || exit 2

# ------------------------------------------------------------------
say "branches, tags and history editing"

gp branch madebranch
expect "the branch is listed" "madebranch" gp branch
gp checkout madebranch >/dev/null 2>&1
expect "checkout moves HEAD" "refs/heads/madebranch" gp symbolic-ref HEAD
gp switch -c other >/dev/null 2>&1
expect "switch -c moves HEAD" "refs/heads/other" gp symbolic-ref HEAD
# the creation belongs to the branch, which is what it is a fact about; HEAD's
# own log has the move rather than the birth
expect "a branch made by switch -c is in its own reflog" "branch: Created" \
	gp reflog other
expect "and HEAD's reflog has the move onto it" "checkout: moving from" gp reflog
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
expect "the conflict is marked in the file" "<<<<<<< HEAD" cat f.txt
expect "the conflict names the other side" ">>>>>>> side" cat f.txt
expect "the conflict does not commit itself" "main changes f" gp log --oneline

# the unfinished merge is a state on disk, as in git
expect_file "the merge is recorded in MERGE_HEAD" .gitprompt/MERGE_HEAD
expect "status names the unmerged paths" "You have unmerged paths." gp status
# both branches added f.txt, so there is no base version: git's letters are AA
expect "status --short marks the path AA" "AA f.txt" gp status --short
stages_of() { gp ls-files -s "$1" | awk '{print $3}' | tr -d '\r' | sort | tr '\n' ','; }
if [ "$(stages_of f.txt)" = "2,3," ]; then
	ok "the conflict is the index's unmerged stages"
else
	bad "the conflict is the index's unmerged stages" "stages $(stages_of f.txt)"
fi
if command -v git >/dev/null 2>&1; then
	git --git-dir="$repo/.gitprompt" ls-files --stage f.txt 2>/dev/null |
		awk '{print $3}' | tr -d '\r' | sort > "$work/git-stages"
	gp ls-files -s f.txt | awk '{print $3}' | tr -d '\r' | sort > "$work/gp-stages"
	if cmp -s "$work/gp-stages" "$work/git-stages"; then
		ok "git sees the same stages in our index"
	else
		bad "git sees the same stages in our index" \
			"git says [$(tr '\n' ',' < "$work/git-stages")]"
	fi
fi
expect_status "write-tree refuses an unmerged index" 128 gp write-tree
expect_status "diff --cached refuses an unmerged index" 1 gp diff --cached
expect_status "commit refuses while a path is unmerged" 1 gp commit -m "too soon"
expect "the refusal names the reason" "unmerged paths" gp commit -m "too soon"
expect "the refusal lists the path with its letters" "AA f.txt" \
	gp commit -m "too soon"
expect_status "a second merge refuses while one is unfinished" 128 gp merge main
expect "the second merge names the reason" "not concluded your merge" gp merge main

# stage the resolution, then commit it: the result must have two parents
printf 'reconciled\n' > f.txt
gp add f.txt >/dev/null 2>&1
if gp status --short | grep -qE '^(UU|AA|DU|UD|AU|UA|DD)'; then
	bad "staging the path resolves it" "status still shows an unmerged path"
else
	ok "staging the path resolves it"
fi
if [ "$(stages_of f.txt)" = "0," ]; then
	ok "staging leaves one stage-0 entry"
else
	bad "staging leaves one stage-0 entry" "stages $(stages_of f.txt)"
fi
expect_absent "the conflict was never a file of its own" \
	.gitprompt/MERGE_CONFLICTS
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

# a side that deletes what the other side changed keeps only two stages:
# there is no version of the file on the deleting side to record
printf 'base\n' > j.txt
gp add j.txt >/dev/null 2>&1
gp commit -m "main adds j" >/dev/null 2>&1
gp switch -c delmod >/dev/null 2>&1
gp rm j.txt >/dev/null 2>&1
gp commit -m "delmod deletes j" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
printf 'changed\n' > j.txt
gp add j.txt >/dev/null 2>&1
gp commit -m "main changes j" >/dev/null 2>&1
expect "deleting on one side and changing on the other conflicts" "CONFLICT" \
	gp merge delmod
expect "status --short marks it UD" "UD j.txt" gp status --short
if [ "$(stages_of j.txt)" = "1,2," ]; then
	ok "the deleted side leaves two stages"
else
	bad "the deleted side leaves two stages" "stages $(stages_of j.txt)"
fi
expect "our version is still in the work tree" "changed" cat j.txt
gp merge --abort >/dev/null 2>&1
expect "the abort of that merge leaves a clean tree" "nothing to commit" gp status

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

# ------------------------------------------------------------------
say "merging the contents of one file"

# Each case below gets its own repository so that an unfinished merge here
# cannot colour anything above or below, and so the checks can assert on the
# bytes of a file rather than on a phrase in a message.
expect_same() {   # expect_same <description> <got-file> <wanted-file>
	if cmp -s "$2" "$3"; then
		ok "$1"
	else
		bad "$1" "got [$(tr '\n' '/' < "$2" 2>/dev/null)] \
wanted [$(tr '\n' '/' < "$3" 2>/dev/null)]"
	fi
}
mergecase() {   # mergecase <directory>: a fresh repository to build a case in
	rm -rf "$1"
	mkdir -p "$1" || exit 2
	cd "$1" || exit 2
	gp init . >/dev/null 2>&1
}

# two sides editing different lines of one file: a merge that only chose a
# whole file would throw one side's edit away, so both have to arrive
mergecase "$work/merge-content"
printf 'one\ntwo\nthree\nfour\nfive\n' > c.txt
gp add c.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp switch -c side >/dev/null 2>&1
printf 'one\ntwo\nTHREE\nfour\nfive\n' > c.txt
gp add c.txt >/dev/null 2>&1
gp commit -m "side changes the third line" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
printf 'one\ntwo\nthree\nfour\nFIVE\n' > c.txt
gp add c.txt >/dev/null 2>&1
gp commit -m "main changes the last line" >/dev/null 2>&1
expect_status "edits to different lines merge without a conflict" 0 gp merge side
printf 'one\ntwo\nTHREE\nfour\nFIVE\n' > "$work/want-content"
expect_same "the merged file carries both sides' edits" c.txt "$work/want-content"
nparents=$(gp cat-file -p HEAD | grep -c '^parent ')
if [ "$nparents" = 2 ]; then
	ok "the clean merge is recorded with two parents"
else
	bad "the clean merge is recorded with two parents" "found $nparents"
fi

# a conflict is only over the lines that differ: the line both sides agree on
# is not in dispute, so it goes outside the markers rather than into both
# halves of the conflict -- which is what git prints, byte for byte
mergecase "$work/merge-trim"
printf 'a\nb\nc\n' > t.txt
gp add t.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp switch -c side >/dev/null 2>&1
printf 'a\nX\nZ\n' > t.txt
gp add t.txt >/dev/null 2>&1
gp commit -m "side changes the second line and the last" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
printf 'a\nX\nc\n' > t.txt
gp add t.txt >/dev/null 2>&1
gp commit -m "main changes the second line" >/dev/null 2>&1
expect "the last line still conflicts" "CONFLICT" gp merge side
printf 'a\nX\n<<<<<<< HEAD\nc\n=======\nZ\n>>>>>>> side\n' > "$work/want-trim"
expect_same "the line the two sides agree on sits outside the markers" \
	t.txt "$work/want-trim"
gp merge --abort >/dev/null 2>&1

# -X ours and -X theirs settle a conflict for one side instead of stopping
mergecase "$work/merge-favor"
printf 'base\n' > x.txt
gp add x.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp switch -c side >/dev/null 2>&1
printf 'theirs\n' > x.txt
gp add x.txt >/dev/null 2>&1
gp commit -m "side rewrites x" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
printf 'ours\n' > x.txt
gp add x.txt >/dev/null 2>&1
gp commit -m "main rewrites x" >/dev/null 2>&1
expect_status "-X ours merges without stopping" 0 gp merge -X ours side
printf 'ours\n' > "$work/want-ours"
expect_same "-X ours keeps our side of the conflict" x.txt "$work/want-ours"
if grep -q '<<<<<<<' x.txt; then
	bad "-X ours leaves no markers behind" "x.txt still has a marker"
else
	ok "-X ours leaves no markers behind"
fi

# the same conflict again, on a file of its own and the other way round
gp switch -c side2 >/dev/null 2>&1
printf 'theirs\n' > z.txt
gp add z.txt >/dev/null 2>&1
gp commit -m "side2 adds z" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
printf 'ours\n' > z.txt
gp add z.txt >/dev/null 2>&1
gp commit -m "main adds z" >/dev/null 2>&1
expect_status "-X theirs merges without stopping" 0 gp merge -X theirs side2
printf 'theirs\n' > "$work/want-theirs"
expect_same "-X theirs keeps their side of the conflict" z.txt "$work/want-theirs"
expect_status "an unknown -X is refused" 1 gp merge -X bananas side2
expect "the refusal names the option" "unknown strategy option 'bananas'" \
	gp merge -X bananas side2

# --no-ff makes a merge commit even where the branch could have moved up to
# their tip instead, so the fact of the merge is in the history
mergecase "$work/merge-noff"
printf 'start\n' > n.txt
gp add n.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp switch -c side >/dev/null 2>&1
printf 'start\nside\n' > n.txt
gp add n.txt >/dev/null 2>&1
gp commit -m "side appends" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
expect_status "--no-ff merges where a fast-forward was possible" 0 \
	gp merge --no-ff side
nparents=$(gp cat-file -p HEAD | grep -c '^parent ')
if [ "$nparents" = 2 ]; then
	ok "the --no-ff merge is a commit of its own"
else
	bad "the --no-ff merge is a commit of its own" "found $nparents"
fi
printf 'start\nside\n' > "$work/want-noff"
expect_same "the --no-ff merge still has their work" n.txt "$work/want-noff"

# --squash stages the merge and leaves HEAD where it was, so the two sides
# become one ordinary commit rather than a commit with two parents
mergecase "$work/merge-squash"
printf 'one\ntwo\nthree\n' > s.txt
gp add s.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp switch -c side >/dev/null 2>&1
printf 'one\ntwo\nTHREE\n' > s.txt
printf 'added by side\n' > s2.txt
gp add s.txt s2.txt >/dev/null 2>&1
gp commit -m "side adds s2" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
printf 'ONE\ntwo\nthree\n' > s.txt
gp add s.txt >/dev/null 2>&1
gp commit -m "main edits s" >/dev/null 2>&1
expect_status "--squash stages a merge without committing it" 0 \
	gp merge --squash side
if gp log --oneline | grep -q "side adds s2"; then
	bad "--squash leaves the history alone" "their commit is in the log"
else
	ok "--squash leaves the history alone"
fi
expect_absent "--squash writes no merge state" .gitprompt/MERGE_HEAD
expect "the squashed change is in the index" "s2.txt" gp ls-files
expect_file "the squashed change is in the work tree" s2.txt
nparents=$(gp cat-file -p HEAD | grep -c '^parent ')
if [ "$nparents" = 1 ]; then
	ok "--squash leaves HEAD on our side of the fork"
else
	bad "--squash leaves HEAD on our side of the fork" "found $nparents"
fi
expect_status "committing finishes the squash" 0 gp commit -m "squash their work in"
nparents=$(gp cat-file -p HEAD | grep -c '^parent ')
if [ "$nparents" = 1 ]; then
	ok "the squashed result is an ordinary commit"
else
	bad "the squashed result is an ordinary commit" "found $nparents"
fi
printf 'ONE\ntwo\nTHREE\n' > "$work/want-squash"
expect_same "the squash kept their edit and ours" s.txt "$work/want-squash"

# a squash that conflicts leaves an unmerged index with no MERGE_HEAD, and a
# merge cannot start from there: the stages are a question still unanswered
mergecase "$work/merge-squash-conflict"
printf 'base\n' > u.txt
gp add u.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp switch -c side >/dev/null 2>&1
printf 'theirs\n' > u.txt
gp add u.txt >/dev/null 2>&1
gp commit -m "side rewrites u" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
printf 'ours\n' > u.txt
gp add u.txt >/dev/null 2>&1
gp commit -m "main rewrites u" >/dev/null 2>&1
expect_status "a squashed merge conflicts like any other" 1 gp merge --squash side
expect_absent "a conflicted squash writes no merge state" .gitprompt/MERGE_HEAD
expect "the conflict is marked in the file" "<<<<<<< HEAD" cat u.txt
stages=$(gp ls-files -s u.txt | awk '{print $3}' | tr -d '\r' | sort | tr '\n' ',')
if [ "$stages" = "1,2,3," ]; then
	ok "a conflicted squash leaves the stages"
else
	bad "a conflicted squash leaves the stages" "stages $stages"
fi

expect_status "a merge onto an unmerged index is refused" 128 gp merge side
expect "the refusal says the merge cannot start" "Merging is not possible" \
	gp merge side
expect "the refusal names the unresolved conflict" "unresolved conflict" \
	gp merge side
printf 'resolved\n' > u.txt
gp add u.txt >/dev/null 2>&1
expect_status "committing the resolution succeeds" 0 gp commit -m "resolve it"
nparents=$(gp cat-file -p HEAD | grep -c '^parent ')
if [ "$nparents" = 1 ]; then
	ok "the resolved squash commits with one parent"
else
	bad "the resolved squash commits with one parent" "found $nparents"
fi
printf 'resolved\n' > "$work/want-resolved"
expect_same "the resolved file is what was committed" u.txt "$work/want-resolved"

cd "$repo" || exit 2

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
say "a file that moved"

# A rename in the history is a delete and an add, and a merge that reads it
# that way cannot follow an edit made on the far side of it: the path that
# side changed is one this side deleted, which is a conflict rather than
# something to merge into.  So the cases below move a file on one side and
# change it on the other, and expect the two to meet under the new name --
# and where a move cannot be followed, expect the same stages git leaves, so
# that `status` says the same letters.  Each relies on `mergecase` and
# `expect_same` from the merge section above.
#
# A merge here is asked for its output as well as its status, so that a case
# which does not behave can say what it did print.  Running it through this
# keeps the status in $mrc rather than in the $? of the test that reads it.
mergeout() {   # mergeout <name> <command...>: run it, keep the output
	tag=$1; shift
	"$@" > "$work/$tag.out" 2>&1
	mrc=$?
}

# they moved it and left the contents alone; we edited it where it used to be
mergecase "$work/rename-follow"
printf 'one\ntwo\nthree\n' > moved.txt
gp add moved.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp switch -c side >/dev/null 2>&1
gp mv moved.txt renamed.txt >/dev/null 2>&1
gp add -A >/dev/null 2>&1
gp commit -m "side renames it" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
printf 'one\ntwo\nTHREE\n' > moved.txt
gp add moved.txt >/dev/null 2>&1
gp commit -m "main edits it" >/dev/null 2>&1
mergeout rename-follow gp merge side
if [ "$mrc" = 0 ]; then
	ok "a move with no edit on the far side merges"
else
	bad "a move with no edit on the far side merges" \
		"exit $mrc: $(cat "$work/rename-follow.out")"
fi
expect "the move is reported rather than a delete and an add" \
	"Renamed moved.txt -> renamed.txt" cat "$work/rename-follow.out"
printf 'one\ntwo\nTHREE\n' > "$work/want-follow"
expect_same "our edit arrives under the new name" renamed.txt "$work/want-follow"
expect_absent "the old name is gone from the work tree" moved.txt
expect "the new name is in the merged tree" "renamed.txt" gp ls-tree HEAD

# both sides edited it as well: theirs moved it and changed the top line,
# ours changed the bottom one, so the merge of the contents has to happen at
# the name the file now carries
mergecase "$work/rename-edit-both"
printf 'one\ntwo\nthree\n' > moved.txt
gp add moved.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp switch -c side >/dev/null 2>&1
gp mv moved.txt renamed.txt >/dev/null 2>&1
printf 'ONE\ntwo\nthree\n' > renamed.txt
gp add -A >/dev/null 2>&1
gp commit -m "side renames it and edits the top" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
printf 'one\ntwo\nTHREE\n' > moved.txt
gp add moved.txt >/dev/null 2>&1
gp commit -m "main edits the bottom" >/dev/null 2>&1
expect_status "a move the far side also edited merges" 0 gp merge side
printf 'ONE\ntwo\nTHREE\n' > "$work/want-rename-both"
expect_same "both edits arrive at the new name" renamed.txt "$work/want-rename-both"

# they moved it and edited it, we deleted it: git will not choose between a
# change and a deletion, so the move comes back unmerged with the versions
# both sides need -- ours as the deletion, theirs in the tree
mergecase "$work/rename-delete-ours"
printf 'one\ntwo\nthree\n' > moved.txt
gp add moved.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp switch -c side >/dev/null 2>&1
gp mv moved.txt renamed.txt >/dev/null 2>&1
printf 'one\ntwo\nthree\nfour\n' > renamed.txt
gp add -A >/dev/null 2>&1
gp commit -m "side renames it and edits it" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
gp rm moved.txt >/dev/null 2>&1
gp commit -m "main deletes it" >/dev/null 2>&1
mergeout rename-delete-ours gp merge side
if [ "$mrc" = 1 ]; then
	ok "a move onto a deletion is not settled"
else
	bad "a move onto a deletion is not settled" \
		"exit $mrc: $(cat "$work/rename-delete-ours.out")"
fi
expect "the conflict names the move and the deletion" \
	"CONFLICT (rename/delete): moved.txt renamed to renamed.txt in side, but deleted in HEAD." \
	cat "$work/rename-delete-ours.out"
expect "the conflict says which version it left" \
	"Version side of renamed.txt left in tree" \
	cat "$work/rename-delete-ours.out"
stages=$(gp ls-files -s renamed.txt | awk '{print $3}' | tr -d '\r' | sort | tr '\n' ',')
if [ "$stages" = "1,3," ]; then
	ok "the new name holds the base and theirs, not a deletion of ours"
else
	bad "the new name holds the base and theirs, not a deletion of ours" "stages $stages"
fi
printf 'one\ntwo\nthree\nfour\n' > "$work/want-rename-theirs"
expect_same "the version left in the tree is theirs" renamed.txt "$work/want-rename-theirs"

# the mirror image: we moved it, they deleted it, so our move is the version
# left in the tree and the stages are ours and the base
mergecase "$work/rename-mine-delete-theirs"
printf 'one\ntwo\nthree\n' > moved.txt
gp add moved.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp switch -c side >/dev/null 2>&1
gp rm moved.txt >/dev/null 2>&1
gp commit -m "side deletes it" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
gp mv moved.txt ours.txt >/dev/null 2>&1
gp add -A >/dev/null 2>&1
gp commit -m "main renames it" >/dev/null 2>&1
mergeout rename-mine-delete-theirs gp merge side
if [ "$mrc" = 1 ]; then
	ok "our move onto their deletion is not settled either"
else
	bad "our move onto their deletion is not settled either" \
		"exit $mrc: $(cat "$work/rename-mine-delete-theirs.out")"
fi
expect "the mirror conflict names ours as the move" \
	"CONFLICT (rename/delete): moved.txt renamed to ours.txt in HEAD, but deleted in side." \
	cat "$work/rename-mine-delete-theirs.out"
stages=$(gp ls-files -s ours.txt | awk '{print $3}' | tr -d '\r' | sort | tr '\n' ',')
if [ "$stages" = "1,2," ]; then
	ok "our new name holds the base and our version"
else
	bad "our new name holds the base and our version" "stages $stages"
fi
printf 'one\ntwo\nthree\n' > "$work/want-rename-ours"
expect_same "the version left in the tree is ours" ours.txt "$work/want-rename-ours"

# both sides moved it, to different names: neither name is the file, so the
# old name comes back as a stage of its own and the two new names as theirs,
# which is the DD/AU/UA that git prints and `status` has to agree with
mergecase "$work/rename-two-ways"
printf 'one\ntwo\nthree\n' > moved.txt
gp add moved.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp switch -c side >/dev/null 2>&1
gp mv moved.txt theirs.txt >/dev/null 2>&1
gp add -A >/dev/null 2>&1
gp commit -m "side renames it to theirs.txt" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
gp mv moved.txt ours.txt >/dev/null 2>&1
gp add -A >/dev/null 2>&1
gp commit -m "main renames it to ours.txt" >/dev/null 2>&1
mergeout rename-two-ways gp merge side
if [ "$mrc" = 1 ]; then
	ok "two names for one file are not settled"
else
	bad "two names for one file are not settled" \
		"exit $mrc: $(cat "$work/rename-two-ways.out")"
fi
expect "the conflict is a rename/rename" "CONFLICT (rename/rename)" \
	cat "$work/rename-two-ways.out"
stages=""
for p in moved.txt ours.txt theirs.txt; do
	stages="$stages$(gp ls-files -s "$p" | awk '{print $3}' | tr -d '\r' | tr '\n' ','):"
done
if [ "$stages" = "1,:2,:3,:" ]; then
	ok "each name holds the one version it stands for"
else
	bad "each name holds the one version it stands for" "stages $stages"
fi
short=$(gp status --short | tr -d '\r' | sort | tr '\n' '|')
if [ "$short" = "AU ours.txt|DD moved.txt|UA theirs.txt|" ]; then
	ok "status names the two names and the deletion, as git does"
else
	bad "status names the two names and the deletion, as git does" "got [$short]"
fi
printf 'one\ntwo\nthree\n' > "$work/want-rename-theirs-copy"
expect_same "their version is left in the tree" theirs.txt "$work/want-rename-theirs-copy"

# a deletion beside an unrelated addition is not a move: the two files share
# a line, which is under the threshold for reading them as one file that
# moved, and reading them as a move would invent a conflict here
mergecase "$work/not-a-rename"
printf 'alpha\nbeta\ngamma\n' > gone.txt
printf 'keep\n' > keep.txt
gp add gone.txt keep.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp switch -c side >/dev/null 2>&1
gp rm gone.txt >/dev/null 2>&1
gp commit -m "side deletes gone.txt" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
printf 'alpha\nnine\nten\n' > fresh.txt
gp add fresh.txt >/dev/null 2>&1
gp commit -m "main adds an unrelated file" >/dev/null 2>&1
mergeout not-a-rename gp merge side
if [ "$mrc" = 0 ]; then
	ok "an unrelated addition is not read as a move"
else
	bad "an unrelated addition is not read as a move" \
		"exit $mrc: $(cat "$work/not-a-rename.out")"
fi
if grep -q "Renamed" "$work/not-a-rename.out"; then
	bad "nothing is reported as having moved" "$(cat "$work/not-a-rename.out")"
else
	ok "nothing is reported as having moved"
fi
expect_absent "the deleted file stays deleted" gone.txt
printf 'alpha\nnine\nten\n' > "$work/want-fresh"
expect_same "the added file arrives whole" fresh.txt "$work/want-fresh"

# `status` looks for a move as well, since it is the other place a reader is
# owed one rather than a deletion beside an addition -- but only in the index,
# where the move has been staged.  A file moved in the work tree and never
# added is not in the index under either name, and git reads that as a
# deletion and an untracked file.
mergecase "$work/status-rename"
printf 'one\ntwo\nthree\n' > a.txt
gp add a.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp mv a.txt b.txt >/dev/null 2>&1
expect_out "a staged move is a rename in the short report" \
	"R  a.txt -> b.txt" gp status --short
expect "a staged move is a rename in the report" \
	"renamed:    a.txt -> b.txt" gp status
printf 'one\ntwo\nthree\nfour\n' > b.txt
gp add b.txt >/dev/null 2>&1
expect_out "a staged move that was also edited is still a rename" \
	"R  a.txt -> b.txt" gp status --short

# the same rewrite that a merge would not follow is not a rename here either,
# since the line between the two is the same one
printf 'x\ny\nz\nw\n' > b.txt
gp add b.txt >/dev/null 2>&1
expect "a move rewritten past the threshold is a deletion" \
	"D  a.txt" gp status --short
expect "and the path it appeared at is an addition" "A  b.txt" gp status --short

mergecase "$work/status-unstaged-move"
printf 'one\ntwo\nthree\n' > a.txt
gp add a.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
mv a.txt b.txt
expect "an unstaged move leaves the old name deleted" " D a.txt" gp status --short
expect "an unstaged move leaves the new name untracked" "?? b.txt" gp status --short

# the two sections do not interleave however the paths sort, and the word in
# them is padded to the column git pads it to
mergecase "$work/status-sections"
printf 'one\n' > z.txt
printf 'two\n' > gone.txt
printf 'three\n' > a.txt
gp add z.txt gone.txt a.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
printf 'ONE\n' > z.txt
gp add z.txt >/dev/null 2>&1
gp rm gone.txt >/dev/null 2>&1
printf 'THREE\n' > a.txt
sections=$(gp status | tr -d '\r' | grep 'Changes ' | tr '\n' '|')
if [ "$sections" = "Changes to be committed:|Changes not staged for commit:|" ]; then
	ok "the staged section is printed first, as git prints it"
else
	bad "the staged section is printed first, as git prints it" \
		"got [$sections]"
fi
expect "a staged delete is padded to git's column" \
	"deleted:    gone.txt" gp status

# `diff` owes a reader the same thing a merge and `status` do: a move printed
# as the move it was, not as a deletion beside an unrelated addition.  The
# header is git's -- the two names on the `diff --git` line, the score, and the
# pair of `rename` lines -- and the hunks follow only when the contents moved
# as well.
mergecase "$work/diff-rename"
printf 'one\ntwo\nthree\n' > a.txt
gp add a.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp mv a.txt b.txt >/dev/null 2>&1
cat > "$work/want-rename" <<'EOF'
diff --git a/a.txt b/b.txt
similarity index 100%
rename from a.txt
rename to b.txt
EOF
gp diff --cached | tr -d '\r' > "$work/got-rename"
expect_same "a move is reported as a move, not as a delete and an add" \
	"$work/got-rename" "$work/want-rename"
cat > "$work/want-rename-stat" <<'EOF'
 a.txt => b.txt | 0
 1 file changed, 0 insertions(+), 0 deletions(-)
EOF
gp diff --cached --stat | tr -d '\r' > "$work/got-rename-stat"
expect_same "a move that changed no lines is one file changed, nothing added" \
	"$work/got-rename-stat" "$work/want-rename-stat"

# the file moved and was edited, so the same header carries the hunks under it
# -- and the score is the share of lines the two copies still have in common,
# which is not the same estimate git's byte score arrives at
printf 'one\ntwo\nthree\nfour\n' > b.txt
gp add b.txt >/dev/null 2>&1
expect "a move that was edited still reports the move" "rename from a.txt" \
	gp diff --cached
expect "a move that was edited reports a score" "similarity index " \
	gp diff --cached
expect "and the hunks for the edit follow the header" "+four" gp diff --cached
expect "the move is one file changed, with the edit counted" \
	"1 file changed, 1 insertion(+)" gp diff --cached --stat

# where the old name sat does not matter: the pair of paths is reported whole
mergecase "$work/diff-rename-deep"
printf 'one\ntwo\nthree\n' > a.txt
gp add a.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
mkdir -p deep/down
gp mv a.txt deep/down/b.txt >/dev/null 2>&1
printf 'one\ntwo\nthree\nfour\n' > deep/down/b.txt
gp add -A >/dev/null 2>&1
expect "a move into a subdirectory names both paths in full" \
	"rename to deep/down/b.txt" gp diff --cached

# a rewrite that kept too little of the file is not a move, the same line
# being the one a merge and `status` draw
mergecase "$work/diff-rename-not"
printf 'one\ntwo\nthree\n' > a.txt
gp add a.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp mv a.txt b.txt >/dev/null 2>&1
printf 'x\ny\nz\nw\n' > b.txt
gp add b.txt >/dev/null 2>&1
moved=$(gp diff --cached | tr -d '\r')
case "$moved" in
*"rename from"*)
	bad "a move rewritten past the threshold is not reported as a move" \
		"found a rename in [$moved]"
	;;
*)	ok "a move rewritten past the threshold is not reported as a move" ;;
esac
expect "the old name is reported as deleted instead" "+++ /dev/null" \
	gp diff --cached
expect "and the new name as an addition" "--- /dev/null" gp diff --cached

# a move in the work tree that was never staged is not in the index under
# either name, so neither the index nor the tree has a move to report
mergecase "$work/diff-rename-worktree"
printf 'one\ntwo\nthree\n' > a.txt
gp add a.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp mv a.txt b.txt >/dev/null 2>&1
gp commit -m "the move" >/dev/null 2>&1
printf 'one\ntwo\nthree\nfour\n' > b.txt
wtdiff=$(gp diff | tr -d '\r')
case "$wtdiff" in
*"rename from"*)
	bad "a work-tree edit to a moved file is a change to it" \
		"found a rename in [$wtdiff]"
	;;
*)	ok "a work-tree edit to a moved file is a change to it" ;;
esac
expect "and it is a change to the name the index has" \
	"diff --git a/b.txt b/b.txt" gp diff

# ------------------------------------------------------------------
say "the columns of a diffstat"

# Three things share the width: the paths, the changed-line count and the bar.
# The bar gives way first but only down to a floor, since one with no height in
# it says nothing; what is left over goes to the paths, and a path is cut only
# when even that does not fit.  A path keeps its tail, because the end of a path
# is the part that says which file it is.
#
# Every number in the block below is the one git prints for this repository, and
# the block is compared whole, so the columns are pinned to git's rather than to
# whatever gitprompt happens to produce.
mergecase "$work/diff-stat"
mkdir -p deep/directory/that/goes/on/and/on/and/on/on
longpath=deep/directory/that/goes/on/and/on/and/on/on/deep.txt
printf 'one\ntwo\nthree\n' > a.txt
seq 1 5 > big.txt
printf '\000\001\002' > blob.dat
printf 'x\ny\n' > "$longpath"
printf 'no newline here' > tail.txt
# staged by name rather than with -A, so that the repository does not stage the
# git directory living inside it and put its own blobs in the output
gp add a.txt big.txt blob.dat "$longpath" tail.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1

seq 1 205 > big.txt
printf 'one\nTWO\nthree\nfour\n' > a.txt
printf '\000\001\002\003\004\005\006\007\010\011' > blob.dat
seq 1 6 > "$longpath"
printf 'still none' > tail.txt
gp add a.txt big.txt blob.dat "$longpath" tail.txt >/dev/null 2>&1

cat > "$work/want-stat" <<'EOF'
 a.txt                                              |   3 +-
 big.txt                                            | 200 +++++++++++++++++++++
 blob.dat                                           | Bin 3 -> 10 bytes
 .../that/goes/on/and/on/and/on/on/deep.txt         |   8 +-
 tail.txt                                           |   2 +-
 5 files changed, 209 insertions(+), 4 deletions(-)
EOF
gp diff --cached --stat | tr -d '\r' > "$work/got-stat"
expect_same "a diffstat is laid out in the columns git lays it out in" \
	"$work/got-stat" "$work/want-stat"

# the three rules that decide those columns, named one at a time so that a
# failure says which of them moved rather than only that the block changed
expect "a path too long for its column keeps its tail" \
	".../that/goes/on/and/on/and/on/on/deep.txt" gp diff --cached --stat
expect "a file whose contents are not lines is measured in bytes" \
	"Bin 3 -> 10 bytes" gp diff --cached --stat
expect "a change too large for a bar leaves the counts to say it" \
	"| 200 ++++" gp diff --cached --stat

# A last line with no newline is a different line from the same text with one --
# which is why the hunk above counts `no newline here` as changed -- and a diff
# has to say which of the two it is showing, since the text alone cannot.
cat > "$work/want-tail" <<'EOF'
diff --git a/tail.txt b/tail.txt
index 1045c4a..3814f79 100644
--- a/tail.txt
+++ b/tail.txt
@@ -1 +1 @@
-no newline here
\ No newline at end of file
+still none
\ No newline at end of file
EOF
gp diff --cached | tr -d '\r' | sed -n '/^diff --git a\/tail.txt/,$p' \
	> "$work/got-tail"
expect_same "a last line with no newline says so" "$work/got-tail" \
	"$work/want-tail"

# ------------------------------------------------------------------
say "writing the commit message in an editor"

# Neither -m nor -F, so git falls back on an editor: the checks below write
# one and let it decide the message.  The editors are run as `sh <script>`
# rather than as executables, because that is the same command whether
# system() reaches a shell (Unix) or cmd.exe (Windows), and so whether the
# script has to carry the executable bit stops mattering.
editcase() {   # editcase <directory>: a repository with one staged change
	rm -rf "$1"
	mkdir -p "$1" || exit 2
	cd "$1" || exit 2
	gp init . >/dev/null 2>&1
	gp config user.name "Editor Test" >/dev/null 2>&1
	gp config user.email editor@example.com >/dev/null 2>&1
	printf 'first\n' > e.txt
	gp add e.txt >/dev/null 2>&1
	gp commit -m "the base" >/dev/null 2>&1
	printf 'second\n' >> e.txt
	gp add e.txt >/dev/null 2>&1
}
editor_writes() {   # editor_writes <script> <text>: replaces the message
	cat > "$1" <<EOF
#!/bin/sh
printf '%s' '$2' > "\$1"
EOF
}
editor_appends() {   # editor_appends <script> <text>: adds a line below it
	cat > "$1" <<EOF
#!/bin/sh
printf '%s\n' '$2' >> "\$1"
EOF
}
expect_message() {   # expect_message <description> <expected message>
	got=$(gp cat-file -p HEAD | sed '1,/^$/d')
	if [ "$got" = "$2" ]; then
		ok "$1"
	else
		bad "$1" "wanted [$2] got [$got]"
	fi
}

editcase "$work/commit-editor"
editor_writes "$work/ed-msg.sh" 'written by the editor
'
expect_status "an editor supplies the message" 0 \
	env GIT_EDITOR="sh $work/ed-msg.sh" "$GP" commit
expect_message "the commit carries what the editor wrote" "written by the editor"
expect_file "the editor was given a file to write into" .gitprompt/COMMIT_EDITMSG

# the file the editor is handed explains itself, and the explanation is not
# part of the message -- `#` lines are comments, and blank runs collapse
editcase "$work/commit-editor-comments"
editor_writes "$work/ed-comments.sh" 'subject line

# a comment the editor left behind
body under a comment


'
expect_status "comments in the edited message are accepted" 0 \
	env GIT_EDITOR="sh $work/ed-comments.sh" "$GP" commit
expect_message "the comment and the blank runs are stripped" \
	"subject line

body under a comment"

editcase "$work/commit-editor-empty"
editor_writes "$work/ed-empty.sh" '# nothing but a comment
'
expect_status "an emptied message aborts the commit" 1 \
	env GIT_EDITOR="sh $work/ed-empty.sh" "$GP" commit
expect "the refusal says why" "Aborting commit due to empty commit message." \
	env GIT_EDITOR="sh $work/ed-empty.sh" "$GP" commit
expect_message "nothing was committed" "the base"

editcase "$work/commit-editor-fails"
expect_status "an editor that fails stops the commit" 1 \
	env GIT_EDITOR='sh -c "exit 1"' "$GP" commit
expect "the refusal blames the editor" "the editor exited with an error" \
	env GIT_EDITOR='sh -c "exit 1"' "$GP" commit
expect_message "a failed editor committed nothing" "the base"

editcase "$work/commit-editor-none"
# clearing it locally as well, so a core.editor in this machine's global
# config cannot decide what the check below sees
gp config core.editor "" >/dev/null 2>&1
expect_status "with no editor, commit refuses" 1 "$GP" commit
expect "the refusal names the ways to set one" 'set GIT_EDITOR or EDITOR' \
	"$GP" commit
expect_status "a message on the command line needs no editor" 0 \
	"$GP" commit -m "typed on the command line"
expect_message "and that is the message" "typed on the command line"
gp commit -m "another" >/dev/null 2>&1
expect_status "--no-edit has nothing to reach for" 1 "$GP" commit --no-edit
expect "the refusal names the ways to give a message" 'pass -m "..." or -F <file>' \
	"$GP" commit --no-edit

editcase "$work/commit-editor-amend"
editor_writes "$work/ed-amend.sh" 'a new message
'
expect_status "an amend with no message opens the editor" 0 \
	env GIT_EDITOR="sh $work/ed-amend.sh" "$GP" commit --amend
expect_message "the amendment replaced the message" "a new message"
expect_status "--amend --no-edit keeps the old message" 0 \
	"$GP" commit --amend --no-edit
expect_message "the old message is what came back" "a new message"
expect_status "-e opens the editor even with -m" 0 \
	env GIT_EDITOR="sh $work/ed-amend.sh" "$GP" commit --amend -e -m "ignored"
expect_message "the editor's message won" "a new message"

editcase "$work/commit-editor-priority"
editor_writes "$work/ed-git.sh" 'from GIT_EDITOR
'
editor_writes "$work/ed-editor.sh" 'from EDITOR
'
editor_writes "$work/ed-core.sh" 'from core.editor
'
expect_status "GIT_EDITOR is used when it is set" 0 \
	env GIT_EDITOR="sh $work/ed-git.sh" EDITOR="sh $work/ed-editor.sh" "$GP" commit
expect_message "GIT_EDITOR beats EDITOR" "from GIT_EDITOR"
gp config core.editor "sh $work/ed-core.sh" >/dev/null 2>&1
expect_status "core.editor is used" 0 \
	env EDITOR="sh $work/ed-editor.sh" "$GP" commit --amend
expect_message "core.editor beats EDITOR" "from core.editor"
expect_status "core.editor is consulted before VISUAL too" 0 \
	env VISUAL="sh $work/ed-editor.sh" "$GP" commit --amend
expect_message "core.editor is still what ran" "from core.editor"

# a merge is the case where there is already a message to start from, and the
# editor the suite writes only appends -- so what comes out shows the file it
# was handed began with the merge's message
editcase "$work/commit-editor-merge"
gp commit -m "our second" >/dev/null 2>&1
gp branch side >/dev/null 2>&1
gp checkout side >/dev/null 2>&1
printf 'theirs\n' > z.txt
gp add z.txt >/dev/null 2>&1
gp commit -m "their commit" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
printf 'ours\n' > q.txt
gp add q.txt >/dev/null 2>&1
gp commit -m "our commit" >/dev/null 2>&1
expect_status "a merge is prepared without committing" 0 gp merge --no-commit side
expect "the merge state holds a message" "Merge side" cat .gitprompt/MERGE_MSG
editor_appends "$work/ed-merge.sh" 'and a note about it'
expect_status "commit concludes the prepared merge" 0 \
	env GIT_EDITOR="sh $work/ed-merge.sh" "$GP" commit
expect_message "the editor started from the merge's message" \
	"Merge side
and a note about it"
nparents=$(gp cat-file -p HEAD | grep -c '^parent ')
if [ "$nparents" = 2 ]; then
	ok "and it is still a merge commit"
else
	bad "and it is still a merge commit" "found $nparents parents"
fi

cd "$repo" || exit 2

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
say "the number a prompt gets"

# A prompt's number is what places it in the sequence when the file is read on
# its own, so one number must not be handed out twice.  It is read off the
# repository -- the work tree, the index, and the tip of every ref -- because
# the counter that used to decide it was untracked, and so a clone, a
# colleague's checkout and a second machine all started numbering in the same
# place and wrote a prompt file the others already had.
seqsrc=$work/seq-src
mergecase "$seqsrc"
printf 'one\n' > a.txt
gp add a.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp prompt -m "the first" >/dev/null 2>&1
gp prompt -m "the second" >/dev/null 2>&1
expect_file "a prompt is numbered in order" prompts/0001-the-first.md
expect_file "and the next one follows it" prompts/0002-the-second.md
gp add -A >/dev/null 2>&1
gp commit -m "record the prompts" >/dev/null 2>&1
gp branch side >/dev/null 2>&1
gp switch side >/dev/null 2>&1
gp prompt -m "on the side" >/dev/null 2>&1
gp add -A >/dev/null 2>&1
gp commit -m "a prompt on the side" >/dev/null 2>&1
gp switch main >/dev/null 2>&1

cd "$work" || exit 2
gp clone "$seqsrc" seq-clone >/dev/null 2>&1
cd "$work/seq-clone" || exit 2
# the clone's work tree holds main's prompts and not the side branch's, so only
# the refs can say that 0003 is taken
expect_file "the clone has the prompts it checked out" prompts/0002-the-second.md
expect_absent "the clone did not check out the side branch" \
	prompts/0003-on-the-side.md
gp prompt -m "written in the clone" >/dev/null 2>&1
expect_file "a prompt written in a clone continues the sequence" \
	prompts/0004-written-in-the-clone.md
gp prompt -m "and one more uncommitted" >/dev/null 2>&1
expect_file "an uncommitted prompt's number is not reused either" \
	prompts/0005-and-one-more-uncommitted.md

# and a number the repository has forgotten -- the prompt deleted, the deletion
# committed -- is not handed out again, because the local counter is a floor
# under the repository's own answer
seqdrop=$work/seq-drop
mergecase "$seqdrop"
printf 'x\n' > a.txt
gp add a.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp prompt -m "the only prompt" >/dev/null 2>&1
gp add -A >/dev/null 2>&1
gp commit -m "record it" >/dev/null 2>&1
gp rm prompts/0001-the-only-prompt.md >/dev/null 2>&1
gp commit -m "drop it" >/dev/null 2>&1
gp prompt -m "after the deletion" >/dev/null 2>&1
expect_file "a deleted prompt's number is not handed out again" \
	prompts/0002-after-the-deletion.md

# Back to the repository the section above left behind: the packed-store checks
# read it through the shell's own directory, so this section has to hand that
# directory back the way it found it rather than leave it in a scratch tree.
cd "$repo" || exit 2

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

# ------------------------------------------------------------------
say "repack and prune"

# gc is repack and prune fused into one command, so each half has to stand on
# its own: repack packs and must not delete anything, prune deletes and must
# not pack.
rp=$work/repack
mergecase "$rp"
printf 'one\n' > a.txt
gp add -A >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp prompt -m "a prompt so there is more than one object" >/dev/null 2>&1
gp add -A >/dev/null 2>&1
gp commit -m "record the prompt" >/dev/null 2>&1

loose_before=$(gp count-objects | sed -n 's/ objects$//p')
if [ "${loose_before:-0}" -gt 0 ]; then
	ok "the store starts loose"
else
	bad "the store starts loose" "loose=$loose_before"
fi

rp_out=$(gp repack -a)
case "$rp_out" in
*"into a pack"*) ok "repack -a packs the objects" ;;
*) bad "repack -a packs the objects" "$rp_out" ;;
esac
expect_out "repack -a leaves no loose objects" "0 objects" gp count-objects
expect_status "the store is readable after repack" 0 gp fsck
expect "the log survives repack" "the base" gp log --oneline

# the pack repack wrote is a pack git can check object by object, the same as
# the one gc writes -- it is the same writer
if command -v git >/dev/null 2>&1; then
	rp_bad=
	for idx in "$rp"/.gitprompt/objects/pack/*.idx; do
		[ -e "$idx" ] || continue
		git verify-pack -v "$idx" >/dev/null 2>&1 || rp_bad="$rp_bad $idx"
	done
	if [ -n "$rp_bad" ]; then
		bad "git verifies the pack repack wrote" "unverifiable:$rp_bad"
	else
		ok "git verifies the pack repack wrote"
	fi
else
	skip "git verifies the pack repack wrote (no git)"
fi

# an incremental repack only adds the loose objects, and with nothing loose it
# says so rather than rewriting the pack
expect "an incremental repack finds nothing to add" "Nothing new to pack" \
	gp repack
rp_quiet=$(gp repack -a -q)
if [ -z "$rp_quiet" ]; then
	ok "repack -q says nothing"
else
	bad "repack -q says nothing" "$rp_quiet"
fi
expect "repack --dry-run reports without packing" "Would pack" \
	gp repack -a --dry-run
expect_status "repack -a -d still leaves a readable store" 0 gp repack -a -d
expect_status "the store is readable after repack -d" 0 gp fsck

# prune is about what nothing reaches.  A blob written straight into the store
# is unreachable by construction, and fresh, so it is inside the grace period
# and prune names it as kept rather than deleting it.
dangling=$(printf 'orphan\n' | gp hash-object -w --stdin)
pr_out=$(gp prune)
case "$pr_out" in
*"Kept 1 unreachable object(s) younger"*)
	ok "prune protects an object inside the grace period" ;;
*) bad "prune protects an object inside the grace period" "$pr_out" ;;
esac
expect_out "prune leaves the young unreachable object" "orphan" \
	gp cat-file -p "$dangling"

# and --expire=now closes the grace period, so the same object goes
expect "prune --expire=now removes it" "Pruned 1 unreachable loose object(s)" \
	gp prune --expire=now
expect_status "the pruned object is gone" 128 gp cat-file -t "$dangling"
expect_status "the store is readable after prune" 0 gp fsck
expect "prune spared the reachable objects" "the base" gp log --oneline

# -n must report the same work without doing it
dangling2=$(printf 'orphan two\n' | gp hash-object -w --stdin)
expect "prune -n reports the object it would drop" "Would prune 1" \
	gp prune -n --expire=now
expect_out "prune -n did not drop it" "orphan two" gp cat-file -p "$dangling2"
expect "prune --expire cannot be guessed at" "cannot parse" \
	gp prune --expire=whenever
expect_status "a bad --expire fails" 128 gp prune --expire=whenever

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
say "serving over gp://"

# gitprompt's own transport, and the only one that is not a local path.  It is
# deliberately not git's wire protocol: the store is already an ordinary git
# object store, so the exchange is the one the local transport already performs
# -- read the far side's refs, copy across the objects it is missing -- and HTTP
# is only somewhere to carry those bytes.
#
# The repository served here is the one the section above has just gc'd, and
# another gc runs while the server is up: a server that loaded its packs once
# and kept them would keep serving the loose objects that gc has since removed,
# and hand out a store with holes in it.
cd "$repo" || exit 2
branch=$(gp symbolic-ref HEAD)
branch_short=${branch#refs/heads/}
served_head=$(gp rev-parse HEAD)

# The binary itself, not through `gp`: `gp` is a shell function, and
# backgrounding a function backgrounds a subshell whose pid is what `$!` gives,
# so `kill $serve_pid` below would kill the wrapper and leave the server it
# started behind, listening for the rest of the run and past it.
"$GP" serve --port 0 > "$work/serve.log" 2>&1 &
serve_pid=$!
port=""
i=0
while [ $i -lt 60 ]; do
	port=$(sed -n 's|.*http://127.0.0.1:\([0-9]*\)/.*|\1|p' \
		"$work/serve.log" 2>/dev/null | head -1)
	[ -n "$port" ] && break
	i=$((i + 1))
	sleep 0.5
done

if [ -z "$port" ]; then
	bad "the server reports the port it listens on" \
		"$(cat "$work/serve.log" 2>/dev/null)"
	skip "the rest of the gp:// section (the server did not come up)"
else
	ok "the server reports the port it listens on"
	gpurl="gp://127.0.0.1:$port/"

	cd "$work" || exit 2
	expect "clone over gp://" "Cloning" gp clone "$gpurl" netclone
	expect_file "the gp:// clone has the project files" netclone/a.txt
	expect_file "the gp:// clone has the prompts" \
		netclone/prompts/0001-write-a-tokenizer-first.md
	cd "$work/netclone" || exit 2
	expect "the gp:// clone reads the same log" "record the first prompts" \
		gp log --oneline
	expect_out "the gp:// clone is on the served commit" "$served_head" \
		gp rev-parse HEAD
	expect_status "fsck is clean on the gp:// clone" 0 gp fsck

	# a push over the wire sends the objects before it moves the ref, so the
	# ref having moved is the proof that they arrived -- and reading one back
	# out of the served store is the proof that they arrived intact
	printf 'over the wire\n' > net.txt
	gp add -A >/dev/null
	gp commit -m "a commit pushed over gp://" >/dev/null
	pushed=$(gp rev-parse HEAD)
	expect "push over gp://" "$branch_short -> $branch_short" gp push "$gpurl"
	cd "$repo" || exit 2
	expect_out "the served branch moved to the pushed commit" "$pushed" \
		gp rev-parse "$branch"
	expect "the pushed objects arrived" "net.txt" \
		gp ls-tree -r --name-only "$pushed"

	cd "$work/netclone" || exit 2
	gp reset --hard "$served_head" >/dev/null
	expect_status "a push that would rewind is refused" 1 gp push "$gpurl"
	expect "and the refusal says why" "move backwards" gp push "$gpurl"
	expect "a forced push is taken" "forced update" gp push -f "$gpurl"
	cd "$repo" || exit 2
	expect_out "the forced push moved the served branch back" "$served_head" \
		gp rev-parse "$branch"

	cd "$work" || exit 2
	expect "a second clone over gp://" "Cloning" gp clone "$gpurl" netthird
	cd "$work/netthird" || exit 2
	gp remote add other "$gpurl" >/dev/null
	cd "$repo" || exit 2
	printf 'fetched\n' > net2.txt
	gp add -A >/dev/null
	gp commit -m "a commit to fetch over gp://" >/dev/null
	fetched=$(gp rev-parse HEAD)
	cd "$work/netthird" || exit 2
	expect "fetch over gp://" "other/" gp fetch other
	expect_out "the tracking ref names what was fetched" "$fetched" \
		gp rev-parse "refs/remotes/other/$branch_short"

	# the pack that appeared while the server was running
	cd "$repo" || exit 2
	gp gc >/dev/null 2>&1
	cd "$work" || exit 2
	expect "clone over gp:// after a gc the server did not run" "Cloning" \
		gp clone "$gpurl" netpacked
	cd "$work/netpacked" || exit 2
	expect_status "that clone is complete" 0 gp fsck
	expect "and it reads the packed history" "a commit to fetch over gp://" \
		gp log --oneline

	# the same store answers git's dumb HTTP protocol, which is what says the
	# thing being served is a git object store and not a private format
	if command -v git >/dev/null 2>&1; then
		cd "$work" || exit 2
		if git clone -q "http://127.0.0.1:$port/" netgit \
				2>"$work/netgit.err"; then
			ok "git clones the served store"
			cd "$work/netgit" || exit 2
			if [ "$(git rev-parse HEAD)" = "$fetched" ]; then
				ok "git's clone is on the served commit"
			else
				bad "git's clone is on the served commit" \
					"$(git rev-parse HEAD) is not $fetched"
			fi
		else
			bad "git clones the served store" \
				"$(cat "$work/netgit.err")"
		fi
	else
		skip "git cloning the served store (git is not on PATH)"
	fi

	# only the store is reachable: the configuration and the index sit beside
	# it in .gitprompt and are none of a client's business
	if command -v curl >/dev/null 2>&1; then
		for p in config index ../config; do
			code=$(curl -s -o /dev/null -w '%{http_code}' \
				"http://127.0.0.1:$port/$p")
			if [ "$code" = 404 ]; then
				ok "the server does not serve /$p"
			else
				bad "the server does not serve /$p" "answered $code"
			fi
		done
	else
		skip "the path whitelist (curl is not on PATH)"
	fi

	# Stop it and settle, but never block on it: the wait is bounded and then
	# forced, because a server that would not die must not be able to stop the
	# run at its last section, and `wait` on a live process blocks for as long
	# as it lives.
	kill $serve_pid 2>/dev/null
	i=0
	while kill -0 $serve_pid 2>/dev/null && [ $i -lt 20 ]; do
		i=$((i + 1))
		sleep 0.5
	done
	kill -9 $serve_pid 2>/dev/null
	cd "$repo" || exit 2
fi

# ------------------------------------------------------------------
say "git interoperability"

if command -v git >/dev/null 2>&1; then
	gpdir=$repo/.gitprompt

	# git records nothing without a committer identity, and a runner has none
	# configured -- its merge would stop before it wrote the conflict this
	# section exists to read.  git's own suite sets these rather than reading
	# the machine's identity, and so does this, so that what is verified below
	# does not depend on whose machine the suite ran on.  gitprompt takes its
	# identity from the repository, so these reach git alone.
	GIT_AUTHOR_NAME="gitprompt test"
	GIT_AUTHOR_EMAIL="gitprompt@example.com"
	GIT_COMMITTER_NAME=$GIT_AUTHOR_NAME
	GIT_COMMITTER_EMAIL=$GIT_AUTHOR_EMAIL
	export GIT_AUTHOR_NAME GIT_AUTHOR_EMAIL GIT_COMMITTER_NAME GIT_COMMITTER_EMAIL

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

	# the other direction of the same claim: a conflict git wrote is one
	# gitprompt reads, stages and all.  Its own repository, so that an
	# unfinished merge cannot colour anything before or after this.
	gmerge=$work/gitmerge
	rm -rf "$gmerge"
	mkdir -p "$gmerge"
	(
		cd "$gmerge" || exit 1
		gp init . >/dev/null 2>&1
		printf 'base\n' > k.txt
		gp add k.txt >/dev/null 2>&1
		gp commit -m base >/dev/null 2>&1
		gp switch -c side >/dev/null 2>&1
		printf 'theirs\n' > k.txt
		gp add k.txt >/dev/null 2>&1
		gp commit -m theirs >/dev/null 2>&1
		gp checkout main >/dev/null 2>&1
		printf 'ours\n' > k.txt
		gp add k.txt >/dev/null 2>&1
		gp commit -m ours >/dev/null 2>&1
		git --git-dir=.gitprompt --work-tree=. merge side \
			> "$work/git-merge.out" 2>&1
		git --git-dir=.gitprompt ls-files -u 2>/dev/null | awk '{print $3}' |
			tr -d '\r' | sort > "$work/git-made-stages"
		gp ls-files -s k.txt | awk '{print $3}' | tr -d '\r' | sort \
			> "$work/gp-read-stages"
	)
	merge_head=$gmerge/.gitprompt/MERGE_HEAD
	if [ ! -e "$merge_head" ]; then
		# git's own words: if the merge stopped, why it stopped is the whole
		# of what a reader of this failure needs, and it is not visible from
		# the absence of one file.
		bad "gitprompt reads a conflict git made" \
			"git's merge left no MERGE_HEAD: $(tr '\n' ' ' < "$work/git-merge.out")"
	else
		expect "gitprompt marks git's conflict UU" "UU k.txt" \
			sh -c "cd '$gmerge' && '$GP' status --short"
		if cmp -s "$work/git-made-stages" "$work/gp-read-stages"; then
			ok "gitprompt reads the stages of a conflict git made"
		else
			bad "gitprompt reads the stages of a conflict git made" \
				"git wrote [$(tr '\n' ',' < "$work/git-made-stages")] gitprompt read [$(tr '\n' ',' < "$work/gp-read-stages")]"
		fi
		expect "gitprompt reads git's merge state" "both modified" \
			sh -c "cd '$gmerge' && '$GP' status"
		expect_status "gitprompt refuses to commit git's conflict" 1 \
			sh -c "cd '$gmerge' && '$GP' commit -m too-soon"
	fi
else
	skip "the whole git interoperability section (git is not on PATH)"
fi

# ------------------------------------------------------------------
say "a command and a revision that are not there"

cd "$repo" || exit 2
expect_status "an unknown command fails" 1 gp nosuchcommand
expect_status "a replay of an unknown ref fails" 128 gp replay nosuchref

# ------------------------------------------------------------------
say "an option belongs to the command it was written for"

# Its own repository: every check here is about the argument parser, and a
# commit added while probing `commit` should not land in anyone else's history.
optrepo=$work/optrepo
mkdir -p "$optrepo" || exit 2
cd "$optrepo" || exit 2
gp init . >/dev/null 2>&1
gp config user.name "Option Tester" >/dev/null 2>&1
gp config user.email opt@example.com >/dev/null 2>&1
echo hello > file.txt
gp add -A >/dev/null 2>&1
gp commit -m "a first commit" >/dev/null 2>&1

# The list of accepted options is per command, so an option that belongs to a
# different command is refused rather than accepted and ignored.  That is the
# half of the check that matters: `log --amend` used to look as though it had
# taken effect, when all it did was nothing.
expect_status "an option from another command is refused" 1 gp log --amend
expect "and named" "unknown option '--amend'" gp log --amend
expect "and the refusal says what this command takes" \
	"this command takes --oneline" gp log --amend
expect_status "a merge option on commit is refused" 1 gp commit --ff-only
expect "commit's refusal names commit's options" \
	"this command takes -m <value>" gp commit --ff-only
expect_status "a diff option on status is refused" 1 gp status --cached
expect_status "an option no command has is refused too" 1 gp log --bogus

expect_status "an option gitprompt never implemented is refused" 1 \
	gp checkout --source HEAD
expect_status "so is one the old table advertised but ignored" 1 gp cat-file --batch
expect_status "and one whose value the reader ignored" 1 \
	gp for-each-ref --format='%(refname)'
expect_status "and one that would have changed nothing" 1 \
	gp commit --author "A <a@b>"
expect_status "gc's unimplemented --prune is refused" 1 gp gc --prune
expect_status "fsck's unimplemented --strict is refused" 1 gp fsck --strict
expect_status "fetch's unimplemented --depth is refused" 1 gp fetch --depth 1
expect_status "push's unimplemented --prune is refused" 1 gp push --prune
expect_status "describe takes no -m even though it can name a tag" 1 \
	gp describe -m "a tag"

expect "a command with no options says so" "this command takes no options" \
	gp version --build-options
expect_status "a command with no options refuses one" 1 gp version --build-options
expect_status "mv takes no options at all" 1 gp mv --bogus file.txt other.txt

# The options that are real have to keep working, or the refusal would have
# bought correctness by breaking the commands.  A value-taking option is the
# case worth pinning: `tag -m` swallows the message, leaving the name that
# follows it positional, and the message turns up inside the tag object.
expect_status "log -n still takes its value" 0 gp log -n 1
expect_status "gc -n is accepted" 0 gp gc -n
expect_status "fsck -v is accepted" 0 gp fsck -v
expect_status "config --get is a flag, not a value" 0 gp config --get user.name
expect_status "init --bare is still accepted" 0 gp init --bare "$work/optbare"
expect_status "a value-taking option takes its value" 0 \
	gp tag -m "an option check" opttag
expect "and the value is what it read" "an option check" gp cat-file -p opttag
expect_status "commit -m and -q and --allow-empty still work together" 0 \
	gp commit -q --allow-empty -m "an option check"

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
say "the commits a revision reaches"

# rev-list answers from the graph, so the checks pin what is graph and not what
# is syntax: a range is the difference of two ancestor sets, a merge commit is
# one commit with two parents, and the two ends of a symmetric range keep each
# other's history out.  Where git is on PATH the same questions are put to git,
# because the claim these commands make is that they read git's history.
rv=$work/revisions
rm -rf "$rv"
mkdir -p "$rv" || exit 2
cd "$rv" || exit 2
gp init . >/dev/null 2>&1
gp config user.email rv@example.com
gp config user.name "Revision Tester"
gp prompt -m "the prompt the base holds" >/dev/null 2>&1
echo one > a.txt
gp add a.txt >/dev/null 2>&1
gp commit -m "the base" >/dev/null 2>&1
gp tag base

gp checkout -b side >/dev/null 2>&1
gp prompt -m "the prompt the side holds" >/dev/null 2>&1
echo two > b.txt
gp add b.txt >/dev/null 2>&1
gp commit -m "the side" >/dev/null 2>&1

gp checkout main >/dev/null 2>&1
gp prompt -m "the prompt the mainline holds" >/dev/null 2>&1
echo three > c.txt
gp add c.txt >/dev/null 2>&1
gp commit -m "the mainline" >/dev/null 2>&1

# one branch's commits less the other's, which is what A..B is
expect "A..B is what B has and A does not" "the mainline" gp rev-list --oneline base..main
expect "and the other way round is the other branch" "the side" \
	gp rev-list --oneline base..side
empty=$(gp rev-list --oneline base..base 2>&1)
if [ -z "$empty" ]; then
	ok "a range that excludes everything it names prints nothing"
else
	bad "a range that excludes everything it names prints nothing" "got [$empty]"
fi
expect_status "and asking for it is not an error" 0 gp rev-list base..base

# the symmetric range keeps both sides' shared history out
n_sym=$(gp rev-list --count main...side)
if [ "$n_sym" = 2 ]; then
	ok "A...B is the two branches' own commits and not the base"
else
	bad "A...B is the two branches' own commits and not the base" "counted $n_sym"
fi
expect "and it names both of them" "the side" gp rev-list --oneline main...side
n_caret=$(gp rev-list --count ^base main)
if [ "$n_caret" = 1 ]; then
	ok "excluding by hand and by range agree"
else
	bad "excluding by hand and by range agree" "^base main counted $n_caret"
fi

expect "the newest commit comes first" "the mainline" gp rev-list --oneline -n 1 main
expect "and reversing turns the walk around" "the base" \
	gp rev-list --oneline --reverse -n 1 base
expect "the limit is taken from the newest end before the turn" "the mainline" \
	gp rev-list --oneline --reverse -n 1 main

# the merge base is the commit the two branches last shared
baseoid=$(gp rev-parse base)
expect "the merge base of two branches is where they parted" "$baseoid" \
	gp merge-base main side
expect_status "an ancestor is one" 0 gp merge-base --is-ancestor base side
expect_status "a commit that is not an ancestor is not one" 1 \
	gp merge-base --is-ancestor side main

# a merge commit is one commit with two parents, which is the shape every
# question above is asked about
gp merge --no-ff -m "join the side" side >/dev/null 2>&1
n_par=$(gp cat-file -p HEAD | grep -c '^parent ')
if [ "$n_par" = 2 ]; then
	ok "a merge commit records both parents"
else
	bad "a merge commit records both parents" "found $n_par parent lines"
fi
expect "and the branch it brought in is on the walk" "the side" \
	gp rev-list --oneline --no-merges HEAD
n_merges=$(gp rev-list --count --merges HEAD)
n_plain=$(gp rev-list --count --no-merges HEAD)
if [ "$n_merges" = 1 ] && [ "$n_plain" = 3 ]; then
	ok "a merge can be asked for or left out of a walk"
else
	bad "a merge can be asked for or left out of a walk" \
		"merges=$n_merges plain=$n_plain"
fi
expect "the merge base of a branch that is wholly merged is its tip" \
	"$(gp rev-parse side)" gp merge-base main side

# histories that never met have no commit in common, and that is an empty
# answer rather than an empty line
orphantree=$(gp write-tree)
orphan=$(gp commit-tree "$orphantree" -m "an unrelated root")
gp update-ref refs/heads/orphan "$orphan" >/dev/null 2>&1
expect_status "two histories that never met have no merge base" 1 \
	gp merge-base main orphan
orphanout=$(gp merge-base main orphan 2>&1)
if [ -z "$orphanout" ]; then
	ok "and an empty answer prints nothing at all"
else
	bad "and an empty answer prints nothing at all" "got [$orphanout]"
fi

if command -v git >/dev/null 2>&1; then
	# the same questions of git, on the same objects
	for range in "base..main" "base..side" "main...side" "--all" \
		"--merges HEAD" "--no-merges HEAD"; do
		gp_n=$(gp rev-list --count $range 2>&1)
		git_n=$(git --git-dir="$rv/.gitprompt" rev-list --count $range 2>&1)
		if [ "$gp_n" = "$git_n" ]; then
			ok "rev-list --count $range is git's answer"
		else
			bad "rev-list --count $range is git's answer" \
				"gitprompt $gp_n, git $git_n"
		fi
	done
	gp_bases=$(gp merge-base --all main side | sort)
	git_bases=$(git --git-dir="$rv/.gitprompt" merge-base --all main side 2>&1 | tr -d '\r' | sort)
	if [ "$gp_bases" = "$git_bases" ]; then
		ok "merge-base --all finds the ones git finds"
	else
		bad "merge-base --all finds the ones git finds" \
			"gitprompt [$gp_bases] git [$git_bases]"
	fi
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
	gp reflog side
expect "the new branch has the remote's work" "side work" gp log --oneline

# ------------------------------------------------------------------
say "a date says when something was recorded"

# A conversation recorded today says nothing true about when it happened, and
# the times are most of what tells one such conversation from another.  So a
# date can be given rather than taken from the clock -- and it has to survive
# the trip back out, through the file and through the parse that orders and
# counts it.
dates=$work/dates
rm -rf "$dates"
mkdir -p "$dates" || exit 2
cd "$dates" || exit 2
gp init . >/dev/null 2>&1

gp prompt --date='2026-06-03T10:00:00+08:00' -m "the first one" >/dev/null 2>&1
expect "a date is stored as it was given" \
	"timestamp: 2026-06-03T10:00:00+08:00" cat prompts/0001-*.md

# the form a commit object carries, and the one GIT_AUTHOR_DATE is usually
# given in
gp prompt --date='@1700000000 +0800' -m "an epoch" >/dev/null 2>&1
expect "the epoch form is read as the instant it names" \
	"timestamp: 2023-11-15T06:13:20+08:00" cat prompts/0002-*.md

# The instant, not the text of it.  A stored date is read back, counted and
# printed, and it has to come out as the same moment it went in as -- but what
# a reader prints for that moment is written in the offset the *reader* keeps,
# so comparing the printed text with the text that went in only holds for a
# reader standing in the offset the date was written in.  This check passed on
# the machine it was written on, which keeps +08:00, and failed on every CI
# runner, which keep UTC.  The epoch here is the same instant taken from the
# other side, out of the one form that carries its instant outright, and both
# are printed by the same reader in the same offset.
early=$work/dates-epoch
rm -rf "$early"
mkdir -p "$early" || exit 2
cd "$early" || exit 2
gp init . >/dev/null 2>&1
gp prompt --date='@1700000000 +0800' -m "the epoch, alone" >/dev/null 2>&1
epoch_here=$(gp stats | sed -n 's/^first prompt: *//p')
cd "$dates" || exit 2

got=$(gp stats | sed -n 's/^first prompt: *//p')
if [ -n "$epoch_here" ] && [ "$got" = "$epoch_here" ]; then
	ok "the earliest date is that instant, whatever offset this reader keeps"
else
	bad "the earliest date is that instant, whatever offset this reader keeps" \
		"wanted [$epoch_here] got [$got]"
fi

# no offset means where this machine is, so the fields that were typed are the
# fields that come back -- whatever offset this machine happens to keep
gp prompt --date='2026-06-03T10:00:00' -m "local" >/dev/null 2>&1
expect "a date with no offset is read where this machine is" \
	"timestamp: 2026-06-03T10:00:00" cat prompts/0003-*.md

gp prompt --date='2026-06-03' -m "a bare day" >/dev/null 2>&1
expect "a bare day means midnight" "timestamp: 2026-06-03T00:00:00" \
	cat prompts/0004-*.md

# refused, not quietly recorded as now: a date that was meant and dropped
# without a word is worse than one that was turned down
before=$(ls prompts/*.md | wc -l)
expect_status "a date that is not a date is refused" 1 \
	gp prompt --date="half past whenever" -m "x"
after=$(ls prompts/*.md | wc -l)
if [ "$before" = "$after" ]; then
	ok "and nothing is recorded for it"
else
	bad "and nothing is recorded for it" "$before prompts before, $after after"
fi

# the variable git puts a date in, so that a script or a habit carries over
GIT_AUTHOR_DATE='2026-01-02T03:04:05+08:00' \
	gp prompt -m "from the environment" >/dev/null 2>&1
expect "GIT_AUTHOR_DATE is honoured" \
	"timestamp: 2026-01-02T03:04:05+08:00" cat prompts/0005-*.md

GIT_AUTHOR_DATE='2026-01-02T03:04:05+08:00' \
	gp prompt --date='2026-09-09T09:09:09+08:00' -m "both" >/dev/null 2>&1
expect "--date wins over it" "timestamp: 2026-09-09T09:09:09+08:00" \
	cat prompts/0006-*.md

expect "the date reaches the document that comes back out" \
	"- recorded: 2026-06-03T10:00:00+08:00" gp replay

first=$(gp stats | sed -n 's/^first prompt: *//p')
last=$(gp stats | sed -n 's/^last prompt: *//p')
if [ "$first" = "$epoch_here" ] && [ -n "$last" ] && [ "$last" != "$first" ]; then
	ok "and the dates run from that one to a later one"
else
	bad "and the dates run from that one to a later one" \
		"first [$first] last [$last]"
fi

# a session has a start and an end of its own, and those order the sessions:
# one begun earlier but written later still comes first, because the dates are
# what happened and the writing order is only how it got here
gp session start -t "the session in December" \
	--date='2026-12-01T09:00:00+08:00' >/dev/null 2>&1
gp prompt -m "said in december" >/dev/null 2>&1
gp session end --date='2026-12-01T09:30:00+08:00' >/dev/null 2>&1
gp session start -t "the session in March" \
	--date='2026-03-01T09:00:00+08:00' >/dev/null 2>&1
gp prompt -m "said in march" >/dev/null 2>&1
gp session end --date='2026-03-01T09:30:00+08:00' >/dev/null 2>&1

gp replay -o "$work/dates.md" >/dev/null 2>&1
expect "the session begun in March is replayed first" \
	"## 1. the session in March" cat "$work/dates.md"
expect "and the one begun in December second" \
	"## 2. the session in December" cat "$work/dates.md"
expect "a session records the start it was given" \
	"- started: 2026-12-01T09:00:00+08:00" cat "$work/dates.md"
expect "and the end" "- ended: 2026-12-01T09:30:00+08:00" \
	cat "$work/dates.md"
cd "$repo" || exit 2

# ------------------------------------------------------------------
say "two sessions begun in the same second"

# Ending one session and beginning the next takes far less than a second, so
# the two carry the same start time and only the sequence numbers tell them
# apart.  The order used to fall back on the session id, whose tail is random
# -- so a conversation recorded into a repository, where every session starts
# in the same second, came back with its sessions shuffled.
twos=$work/twosessions
rm -rf "$twos"
mkdir -p "$twos" || exit 2
cd "$twos" || exit 2
gp init . >/dev/null 2>&1
gp session start -t "the first session" >/dev/null 2>&1
gp prompt -m "the first thing said" >/dev/null 2>&1
gp session end >/dev/null 2>&1
gp session start -t "the second session" >/dev/null 2>&1
gp prompt -m "the second thing said" >/dev/null 2>&1
gp session end >/dev/null 2>&1
gp add -A >/dev/null 2>&1
gp commit -m "two sessions" >/dev/null 2>&1
gp replay -o "$work/twos.md" >/dev/null 2>&1
expect "the first session is replayed first" "## 1. the first session" cat "$work/twos.md"
expect "and the session begun after it follows" "## 2. the second session" \
	cat "$work/twos.md"
cd "$repo" || exit 2

# ------------------------------------------------------------------
say "a task worked in two sessions and returned to"

# A conversation is opened, another is opened over it, and the first is returned
# to, which is what a task that outgrew one context window looks like from the
# recorder's side.  The prompts were always ordered by their sequence numbers,
# but the sessions were not: the one that was left kept a single start/end pair,
# so its file claimed to run from the first prompt to the last with the whole
# interruption swallowed -- and both documents that reconstruct the history
# grouped by session, so the order the task was worked in appeared in neither.
# The dates are far enough apart that a reader can tell the stretches from each
# other, which is the thing the same-second case above cannot show.
il=$work/interleaved
rm -rf "$il"
mkdir -p "$il" || exit 2
cd "$il" || exit 2
gp init . >/dev/null 2>&1

gp session start -t "the design session" \
	--date='2026-09-10T09:00:00+08:00' >/dev/null 2>&1
design=$(gp session current)
gp prompt --date='2026-09-10T09:05:00+08:00' -m "the design was settled" >/dev/null 2>&1

# Opening a session over one that is open has to say so.  Without that the
# recorder cannot tell that the prompts about to be written are no longer in
# the conversation they thought they were in.
over=$(gp session start -t "the CI session" \
	--date='2026-09-10T12:00:00+08:00' 2>&1 >/dev/null)
case "$over" in
*"pausing session $design"*) ok "opening a session over another says which one it pauses" ;;
*) bad "opening a session over another says which one it pauses" "got [$over]" ;;
esac
ci=$(gp session current)
gp prompt --date='2026-09-10T12:10:00+08:00' -m "the build was checked" >/dev/null 2>&1

gp session use "$design" --date='2026-09-10T15:00:00+08:00' >/dev/null 2>&1
gp prompt --date='2026-09-10T15:20:00+08:00' -m "the design was revisited" >/dev/null 2>&1
gp add -A >/dev/null 2>&1
gp commit -m "three sessions, interleaved" >/dev/null 2>&1

stretches="segments: 2026-09-10T09:00:00+08:00..2026-09-10T12:00:00+08:00, 2026-09-10T15:00:00+08:00.."
expect "the session that was left records both of its stretches" \
	"$stretches" cat "prompts/sessions/$design.md"
expect "and the stretch it was left for ends where that one resumed" \
	"ended_at: 2026-09-10T15:00:00+08:00" cat "prompts/sessions/$ci.md"
if grep -q '^segments:' "prompts/sessions/$ci.md"; then
	bad "a session that was never left keeps one plain pair" \
		"got [$(grep '^segments:' "prompts/sessions/$ci.md")]"
else
	ok "a session that was never left keeps one plain pair"
fi

# The flat chronology is the point of the whole exercise: the prompts in the
# order they were written, each naming the session it was said in, so an
# interruption shows where it happened instead of being collected away.
flat=$(gp replay --flat --format=txt \
	| sed -n 's/^p_[a-z0-9]*  [^ ]*  \(s_[a-z0-9_]*\)  .*/\1/p')
want="$design
$ci
$design"
if [ "$flat" = "$want" ]; then
	ok "the flat listing is in the order the prompts were written"
else
	bad "the flat listing is in the order the prompts were written" \
		"wanted [$want] got [$flat]"
fi

gp replay --flat -o "$work/flat.md" >/dev/null 2>&1
expect "the flat document names each prompt's session" \
	"- session: \`$ci\` the CI session" cat "$work/flat.md"
bodies=$(sed -n 's/^> //p' "$work/flat.md")
want="the design was settled
the build was checked
the design was revisited"
if [ "$bodies" = "$want" ]; then
	ok "and its bodies run from the first prompt to the last"
else
	bad "and its bodies run from the first prompt to the last" \
		"wanted [$want] got [$bodies]"
fi

# and the grouped document, which is what an agent is handed, is unmoved: a
# conversation is one context, and interleaving two would read as one
gp replay -o "$work/grouped.md" >/dev/null 2>&1
expect "the grouped document still keeps the conversations apart" \
	"## 1. the design session" cat "$work/grouped.md"
expect "and shows how many stretches that session had" \
	"- stretches: 2" cat "$work/grouped.md"
expect "session show prints the stretch that is still open" \
	"2026-09-10T15:00:00+08:00 -- (open)" gp session show "$design"

expect_status "recording into a session that was never recorded fails" 1 \
	gp session use s_1700000000_nope
expect "and says there is no such session" "no session with id" \
	gp session use s_1700000000_nope
expect_status "a flat document refuses the per-session reports" 128 \
	gp replay --flat --stat
expect "and says why" "--flat lists the prompts in one chronology" \
	gp replay --flat --stat

gp session use "$design" >/dev/null 2>&1
expect "recording into the session already current adds no stretch" \
	"$stretches" cat "prompts/sessions/$design.md"

gp session end --date='2026-09-10T16:00:00+08:00' >/dev/null 2>&1
expect "returning to a session that had ended says it had ended" \
	"had ended at 2026-09-10T16:00:00+08:00" \
	gp session use "$design" --date='2026-09-10T17:00:00+08:00'
expect "and that return is recorded as a further stretch" \
	", 2026-09-10T17:00:00+08:00.." cat "prompts/sessions/$design.md"
cd "$repo" || exit 2

# ------------------------------------------------------------------
say "an argument that is not ASCII"

# Windows hands a C program its arguments in the ANSI code page, so a prompt
# typed in Chinese was stored as GBK: a prompt file that was not the UTF-8
# every other part of the store is, and a document `replay` writes that no
# reader would decode.  git reads its command line as UTF-16 for the same
# reason.  The bytes are spelled out rather than written into this file, so
# that the suite stays ASCII whatever locale it runs under.
enc=$work/nonascii
rm -rf "$enc"
mkdir -p "$enc" || exit 2
cd "$enc" || exit 2
gp init . >/dev/null 2>&1
cn=$(printf '\344\270\255\346\226\207')     # 中文
gp session start -t "$cn title" >/dev/null 2>&1
sid=$(gp session current)
expect "a session title that is not ASCII is stored as written" \
	"title: $cn title" cat "prompts/sessions/$sid.md"
expect "a prompt that is not ASCII is recorded" "p_" gp prompt -m "$cn prompt"
expect "and the text is stored as it was written" "$cn prompt" \
	cat prompts/0001-prompt.md
gp add -A >/dev/null 2>&1
gp commit -m "$cn commit" >/dev/null 2>&1
expect "a commit message that is not ASCII is stored as written" \
	"$cn commit" gp log -n 1
cd "$repo" || exit 2

# ------------------------------------------------------------------
say "handing the prompts back to an agent"

# `rerun` gives the history to an agent that may act on it, one prompt at a
# time, and maps each session to one agent conversation -- so a task that moved
# between sessions and back is replayed the same way, the prompt that returns
# resuming the conversation that session began.  Nothing is actually run here:
# no runner has an agent installed, and one that did would not answer the same
# twice.  What these checks cover is the plan, which is what decides what would
# run, and the refusals.
rr=$work/rerun
rm -rf "$rr"
mkdir -p "$rr" || exit 2
cd "$rr" || exit 2
gp init . >/dev/null 2>&1

gp session start -t "the first conversation" >/dev/null 2>&1
first=$(gp session current)
gp prompt -m "work began here" >/dev/null 2>&1

gp session start -t "a second conversation" >/dev/null 2>&1
second=$(gp session current)
gp prompt -m "other work in between" >/dev/null 2>&1

gp session use "$first" >/dev/null 2>&1
gp prompt -m "and back to the first" >/dev/null 2>&1
gp add -A >/dev/null 2>&1
gp commit -m "two conversations, interleaved" >/dev/null 2>&1

# The word at the end of each plan line.  `grep -oE` rather than a sed
# substitution, because the alternation in one of those is a GNU extension and
# the macOS runner's sed is BSD.
plan=$(gp rerun | grep -oE '(start|resume)$')
want="start
start
resume"
if [ "$plan" = "$want" ]; then
	ok "the conversation is begun, another begun, and the first resumed"
else
	bad "the conversation is begun, another begun, and the first resumed" \
		"wanted [$want] got [$plan]"
fi

# The plan says what the run is before it is run: how many agent conversations
# it crosses, and that the order is the recorded one.  A history is replayed
# where it was, not now, and a clock cannot say what came after what once the
# prompts were written on machines that disagreed about the time.
expect "the plan counts conversations, not prompts" "2 conversation(s)" gp rerun
expect "and says the order is the recorded one, not the clock's" \
	"replayed in seq order" gp rerun
expect "and says when the prompts were recorded" "recorded 2" gp rerun

# The questions are for somebody at a terminal.  A run whose output goes into
# a pipe is being read by another program, and stopping to ask would hang it --
# which is every check in this file, so this is the assertion that the rest of
# them are not hanging by luck.
out=$(gp rerun 2>&1)
case "$out" in
*"Which agent"*|*"Start the replay"*)
	bad "no question is asked when there is no terminal to ask" "$out" ;;
*) ok "no question is asked when there is no terminal to ask" ;;
esac

# The whole point is that the third prompt reaches the *same* conversation the
# first one opened, and not a fresh one that happens to be about the same work.
opened=$(gp rerun | sed -n 's/.*--session-id \([0-9a-f-]*\)$/\1/p' | sed -n 1p)
resumed=$(gp rerun | sed -n 's/.*--resume \([0-9a-f-]*\)$/\1/p')
if [ -n "$opened" ] && [ "$opened" = "$resumed" ]; then
	ok "coming back to a session resumes the conversation it began"
else
	bad "coming back to a session resumes the conversation it began" \
		"opened [$opened] resumed [$resumed]"
fi

# Two sessions, two conversations: the second must not be handed the first
# one's, or the two stretches of work would share one agent context.
# `tr` because BSD wc pads its count to a column, and the comparison below is
# of the number, not of how it was laid out.
opens=$(gp rerun | sed -n 's/.*--session-id \([0-9a-f-]*\)$/\1/p' | wc -l | tr -d ' ')
distinct=$(gp rerun | sed -n 's/.*--session-id \([0-9a-f-]*\)$/\1/p' \
	| sort -u | wc -l | tr -d ' ')
if [ "$opens" = "2" ] && [ "$distinct" = "2" ]; then
	ok "each session gets a conversation of its own"
else
	bad "each session gets a conversation of its own" \
		"$opens opened, $distinct distinct"
fi

# Derived, not random: a run that has to be started again from where it
# stopped must reach the conversations the first attempt made.
same=$(gp rerun | sed -n 's/.*--session-id \([0-9a-f-]*\)$/\1/p')
if [ "$same" = "$(gp rerun | sed -n 's/.*--session-id \([0-9a-f-]*\)$/\1/p')" ]; then
	ok "the same history replays into the same conversations"
else
	bad "the same history replays into the same conversations" "the ids moved"
fi
if [ "$same" != "$(gp rerun --salt fresh | sed -n \
	's/.*--session-id \([0-9a-f-]*\)$/\1/p')" ]; then
	ok "--salt asks for a fresh set instead"
else
	bad "--salt asks for a fresh set instead" "the ids did not move"
fi

# Nothing may run unless it was asked for.  A rerun starts processes that edit
# the work tree, so the default has to be to say what would run.
out=$(gp rerun)
case "$out" in
*"dry run"*) ok "a rerun with no --yes runs nothing" ;;
*) bad "a rerun with no --yes runs nothing" "got [$out]" ;;
esac
expect_absent "and leaves no message file behind" .gitprompt/RERUN_MSG

expect "the header counts what will run against the whole history" \
	"rerun: 3 of 3 prompt(s)" gp rerun
expect "the model reaches the agent" "--model opus" gp rerun --model opus
expect "and the permission mode does too" "--permission-mode plan" \
	gp rerun --permission-mode plan

# --from is what a run that stopped uses to start again, so it selects the
# prompt it names and everything after it, in written order.
expect "a run can start again from a prompt" "rerun: 1 of 3 prompt(s)" \
	gp rerun --from "$(gp log-prompt --oneline | sed -n 1p | sed 's/ .*//')"
expect "and a filter that matches nothing is refused" \
	"no prompt with id p_nosuchid" gp rerun --from p_nosuchid
expect_status "--from with an unknown id fails" 128 gp rerun --from p_nosuchid

only=$(gp rerun --only-session "$first" | grep -oE '(start|resume)$')
want="start
resume"
if [ "$only" = "$want" ]; then
	ok "--only-session keeps one conversation and resumes within it"
else
	bad "--only-session keeps one conversation and resumes within it" \
		"wanted [$want] got [$only]"
fi
expect_status "--only-session naming no session fails" 128 \
	gp rerun --only-session s_nosuchid

# An agent that cannot be told which conversation to use cannot play back a
# history whose conversations are interrupted, and saying so beats running
# something that is not the history.
expect "an agent that cannot resume is refused with the reason" \
	"cannot be told which conversation to continue" gp rerun --agent=codex
expect_status "and the refusal is fatal" 128 gp rerun --agent=codex
expect_status "an agent that does not exist is a usage error" 2 \
	gp rerun --agent=emacs
expect_status "so is a permission mode the agent does not take" 2 \
	gp rerun --permission-mode=bogus

# A model name goes on a command line a shell will read, so it is held to what
# a model name can be rather than passed through.
expect "a model name with shell syntax in it is refused" \
	"takes a model name" gp rerun --model 'a;rm -rf /'
expect_status "and that is fatal" 128 gp rerun --model 'a;rm -rf /'

expect_status "an agent that cannot resume is refused whatever it is called" \
	128 gp rerun --agent=dsh

# The command line an agent answers to is a fact about the machine it is
# installed on, not about the history being replayed -- the same agent is
# installed differently, under a wrapper, or older than the flags this tree
# knows.  So every piece of it can be replaced, and `--` is how a value that
# begins with a dash gets past the option parser.
gp config gitprompt.agent.claude.command "npx @anthropic-ai/claude-code -p"
expect "config replaces how the agent is started" \
	"npx @anthropic-ai/claude-code -p --permission-mode" gp rerun
gp config --unset gitprompt.agent.claude.command

gp config gitprompt.agent.claude.newSession -- --conversation
expect "config replaces the flag that names a new conversation" \
	"--conversation " gp rerun
gp config --unset gitprompt.agent.claude.newSession

gp config gitprompt.agent.claude.resume ""
expect "and emptying one takes it away" \
	"cannot be told which conversation to continue" gp rerun
expect_status "which leaves the agent unable to replay a history" 128 gp rerun
gp config --unset gitprompt.agent.claude.resume

gp config gitprompt.agent.claude.modelFlag -- -m
expect "config replaces the flag a model name follows" \
	"-m opus" gp rerun --model opus
gp config --unset gitprompt.agent.claude.modelFlag

# An agent the table has never heard of is a command line, not a rebuild: the
# table holds what the agents shipped here are called, and config holds what
# this machine runs an agent with.
gp config gitprompt.agent.mine.command "my-agent -p"
gp config gitprompt.agent.mine.resume -- --resume
gp config gitprompt.agent.mine.newSession -- --session-id
expect "an agent this repository configures replays like a known one" \
	"agent mine" gp rerun --agent=mine
expect "with the command line it was given" \
	"my-agent -p --session-id " gp rerun --agent=mine

# Whether the machine has the agent at all is the one thing the history cannot
# say, and it is said before the first prompt rather than by the first prompt:
# a run that stopped half way would have done half the work.
gp config gitprompt.agent.mine.command "no-such-agent-program -p"
expect "an agent that is not installed is named before anything runs" \
	"is not on PATH" gp rerun --agent=mine --yes
expect_status "and that is fatal" 128 gp rerun --agent=mine --yes
gp config --unset gitprompt.agent.mine.command
gp config --unset gitprompt.agent.mine.resume
gp config --unset gitprompt.agent.mine.newSession
expect_status "with its name forgotten, it is a usage error again" 2 \
	gp rerun --agent=mine

cd "$rr" || exit 2
rm -rf "$work/rerun-empty"
mkdir -p "$work/rerun-empty" || exit 2
cd "$work/rerun-empty" || exit 2
gp init . >/dev/null 2>&1
expect "a history with nothing in it says so" "no prompts recorded yet" gp rerun
expect_status "and that is not a failure" 0 gp rerun
cd "$repo" || exit 2

# ------------------------------------------------------------------
say "recording what the agent answered"

# A prompt is said and the answer comes back later, so the answer is kept as
# its own file, named after the prompt it answers rather than after itself.
# That is what puts it in the history with the prompt: the two are committed
# together, a clone carries both, and a reader of the history is handed the
# question and what came back.
rn=$work/response
rm -rf "$rn"
mkdir -p "$rn" || exit 2
cd "$rn" || exit 2
gp init . >/dev/null 2>&1

gp session start -t "the conversation" >/dev/null 2>&1
sid=$(gp session current)
pid=$(gp prompt -m "build a login page" | cut -d' ' -f1)
pid2=$(gp prompt -m "and make the fields validate" | cut -d' ' -f1)

expect "an answer is recorded against the prompt it names" \
	"prompts/responses/$pid.md" \
	gp response -m "Created login.html; empty input still gets through." "$pid"
expect_file "and kept as a file of its own" "prompts/responses/$pid.md"
expect "the file says which prompt it answers" "prompt: $pid" \
	cat "prompts/responses/$pid.md"
expect "the answer carries the session it was said in" "session: $sid" \
	cat "prompts/responses/$pid.md"

rid=$(sed -n 's/^id: //p' "prompts/responses/$pid.md")
case "$rid" in
r_????????) ok "an answer gets an id of its own" ;;
*) bad "an answer gets an id of its own" "got [$rid]" ;;
esac

# The point of keeping them: giving the history back has to include what came
# back, in every form the history is rendered in.
expect "the document shows the answer under a label" "**Response.**" \
	gp replay --flat
expect "with the answer's text, not just the label" "Created login.html" \
	gp replay --flat
expect "the grouped document does too" "**Response.**" gp replay
expect "the plain listing shows it" "response: Created login.html" \
	gp replay --flat --format=txt
expect "and so does the prompt log" "response: Created login.html" \
	gp log-prompt
expect "it is an object in the json, not a string" '"response":{' \
	gp replay --flat --format=json

# An answer is somebody's work: replacing it is asked for, not assumed.
expect "a second answer is refused" "already answered" \
	gp response -m "a second attempt" "$pid"
expect_status "and the refusal is a failure" 1 \
	gp response -m "a second attempt" "$pid"
gp response -m "a second attempt" --force "$pid" >/dev/null 2>&1
expect "with --force the answer is replaced in place" "second attempt" \
	cat "prompts/responses/$pid.md"
count=$(ls prompts/responses | wc -l | tr -d ' ')
if [ "$count" = "1" ]; then
	ok "replacing an answer does not leave the old file behind"
else
	bad "replacing an answer does not leave the old file behind" \
		"$count files in prompts/responses"
fi

# With no id the newest prompt is the one being answered, which is the case a
# person recording an answer by hand is normally in.
expect "with no id the newest prompt is answered" \
	"prompts/responses/$pid2.md" gp response -m "validation added"
expect "and its text is what was recorded" "validation added" \
	cat "prompts/responses/$pid2.md"

expect "an id that is not in the history is refused" "no prompt with id" \
	gp response -m x p_nosuchid
expect_status "and that is a failure" 1 gp response -m x p_nosuchid
expect "a date that cannot be read is refused" "cannot read the date" \
	gp response -m x --date "not a date" --force

# Three ways in, because an answer comes from an agent as often as from a
# person: on the command line, out of a file, or down a pipe.
printf 'an answer from a file\n' > ans.txt
gp response -F ans.txt --force >/dev/null 2>&1
expect "an answer can come from a file" "an answer from a file" \
	cat "prompts/responses/$pid2.md"
printf 'an answer down a pipe\n' | gp response --force >/dev/null 2>&1
expect "and it can come down a pipe" "an answer down a pipe" \
	cat "prompts/responses/$pid2.md"
out=$(printf '' | gp response 2>&1)
case "$out" in
*"nothing to record"*) ok "an empty pipe is refused rather than recorded" ;;
*) bad "an empty pipe is refused rather than recorded" "got [$out]" ;;
esac

# An answer is read out of the commit as well as the work tree, so moving the
# file away leaves the history complete.
gp add -A >/dev/null 2>&1
gp commit -m "record the answers" >/dev/null 2>&1
mv prompts/responses "$work/response-aside"
expect "an answer is read out of the commit" "an answer down a pipe" \
	gp replay --flat
mv "$work/response-aside" prompts/responses

gp prompt -m "a third thing nobody answered" >/dev/null 2>&1
case "$(gp replay --flat --format=json)" in
*'"response":null'*) ok "a prompt that was never answered carries none" ;;
*) bad "a prompt that was never answered carries none" "no null in the json" ;;
esac

# `--record` is what keeps an answer nobody typed in.  The agent is a stub
# that echoes the prompt back, so what was kept can be checked exactly; on
# Windows the shell that runs it is cmd.exe, which needs the .bat spelling.
stub=$work/agent-bin
rm -rf "$stub"
mkdir -p "$stub" || exit 2
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
	cat > "$stub/claude.bat" <<'STUB'
@echo off
set /p P=
echo STUB ANSWER: %P%
STUB
	;;
*)
	# `cat` with no operand is stdin everywhere; `< -` is not -- a shell
	# that does not read `-` as a name for it looks for a file called "-".
	cat > "$stub/claude" <<'STUB'
#!/bin/sh
printf 'STUB ANSWER: '
cat
STUB
	chmod +x "$stub/claude"
	;;
esac

expect "--record says the answers will be kept" "each answer will be recorded" \
	gp rerun --record

oldpath=$PATH
PATH="$stub:$PATH"
gp rerun --record --yes > "$work/rerun-record.log" 2>&1
rc=$?
PATH=$oldpath
if [ "$rc" = "0" ]; then
	ok "a recorded rerun finishes"
else
	bad "a recorded rerun finishes" "exit $rc: $(cat "$work/rerun-record.log")"
fi
expect "the agent's answer is printed through as it comes" \
	"STUB ANSWER: build a login page" cat "$work/rerun-record.log"
expect "and it is what gets recorded" "STUB ANSWER: build a login page" \
	cat "prompts/responses/$pid.md"
expect "the answer to the newest prompt is recorded too" \
	"STUB ANSWER: a third thing nobody answered" \
	cat "$work/rerun-record.log"

cd "$repo" || exit 2

# ------------------------------------------------------------------
say "reading a history back"

# Everything above ran in a repository gitprompt made for itself.  What most
# people are handed is the other thing -- a `git clone` of a prompt history,
# which git fills with the prompt files and no store at all -- so this is that
# whole path, from the clone to a working repository.
back=$work/back
mkdir -p "$back" || exit 2
cd "$back" || exit 2
gp init . >/dev/null
gp config user.email back@example.com
gp config user.name "Back Tester"

# a session started without -t is named after what was first said in it, so
# the one line of its file that says what the conversation was about says
# something; a clone reads that file and nothing else
gp session start >/dev/null
gp prompt -m "write a tokenizer first" >/dev/null
bsid=$(gp session current)
expect "an untitled session is named after its first prompt" \
	"title: write a tokenizer first" cat "prompts/sessions/$bsid.md"
gp prompt -m "now add a normalizer" >/dev/null
expect "and a later prompt does not rename it" \
	"title: write a tokenizer first" cat "prompts/sessions/$bsid.md"

# the usage has always advertised `show <rev|prompt-id>`, and a prompt id is
# a file in the history rather than an object, so it needed answering here
bpid=$(gp log-prompt --oneline | sed -n 1p | sed 's/ .*//')
expect "show resolves a prompt id" "write a tokenizer first" gp show "$bpid"
expect "show prints the prompt's file" "prompts/0001-" gp show "$bpid"
expect_status "show still refuses a revision that is not there" 128 \
	gp show nosuchrev
expect "with the wording a revision gets" "unknown revision" gp show nosuchrev

# a prompt is very often a paragraph, and cutting it at the first newline
# loses everything after the opening sentence
printf 'first line of a long one\nsecond line carries the constraint\nthird line\n' \
	| gp capture >/dev/null
expect "log-prompt prints every line of a prompt" "third line" gp log-prompt
expect "timeline folds one onto a single line" \
	"first line of a long one second line carries the constraint" gp timeline
expect "log-prompt --oneline folds it the same way" \
	"first line of a long one second line carries the constraint" \
	gp log-prompt --oneline

# ------------------------------------------------------------------
say "a commit carries its prompts"

# A commit is the join between the code it adds and the prompts that produced
# it.  Everything here is what that claim costs to keep true: the prompt files
# the commit adds are the ones it names, a commit of code alone names none, and
# amending does not drop them.
lk=$work/link
mkdir -p "$lk" || exit 2
cd "$lk" || exit 2
gp init . >/dev/null
gp config user.email link@example.com
gp config user.name "Link Tester"
gp session start -t "the login page" >/dev/null
gp prompt -m "build a login page" >/dev/null
lp1=$(gp log-prompt --oneline | sed -n 1p | sed 's/ .*//')
echo '<form></form>' > page.html
gp add page.html >/dev/null
gp commit -m "add the login page" >/dev/null
expect "a commit names the prompt that produced its code" \
	"Prompts: $lp1" gp show HEAD
pcount() { gp show "$1" | grep '^Prompts:' | grep -oE 'p_[a-z0-9]+' | wc -l | tr -d ' '; }
expect "the session file is not one of them" "1" printf '%s\n' "$(pcount HEAD)"

gp prompt -m "add a password field" >/dev/null
lp2=$(gp log-prompt --oneline | sed -n 2p | sed 's/ .*//')
echo '<input>' >> page.html
gp add page.html >/dev/null
gp commit -m "add the password field" >/dev/null
expect "a later commit names the prompt it added" "Prompts: $lp2" gp show HEAD
expect "and only that one, not the one before it" "1" \
	printf '%s\n' "$(pcount HEAD)"

gp commit --amend -m "add the password field, once more" >/dev/null
expect "amending keeps the prompt the commit carried" \
	"Prompts: $lp2" gp show HEAD

# A prompt edited after it was recorded is the same prompt the commit already
# carried, so amending the edit on does not name it a second time.
printf '\n' >> prompts/0002-*.md
gp add prompts/0002-*.md >/dev/null
gp commit --amend -m "and the outcome written in later" >/dev/null
expect "editing a carried prompt does not name it twice" "1" \
	printf '%s\n' "$(pcount HEAD)"

echo '<style>' >> page.html
gp add page.html >/dev/null
gp commit -m "tidy the markup" >/dev/null
expect "a commit of code alone carries no prompt" "0" \
	printf '%s\n' "$(pcount HEAD)"

# The prompt directory is a configuration value and may be nested, so the
# previous tree is found by following it a component at a time.  A lookup of the
# whole name at once would find nothing, every prompt would look newly added,
# and a prompt this commit never touched would be carried again.
nest=$work/nest
mkdir -p "$nest" || exit 2
cd "$nest" || exit 2
gp init . >/dev/null
gp config user.email nest@example.com
gp config user.name "Nest Tester"
gp config gitprompt.promptDir docs/prompts
gp prompt -m "a prompt in a nested directory" >/dev/null
expect "a nested prompt directory is where the prompt goes" \
	"docs/prompts/0001-" gp status --short
echo 'int main(void) { return 0; }' > main.c
gp add -A >/dev/null
gp commit -m "the nested prompt and its code" >/dev/null
expect "a commit under a nested prompt directory names its prompt" "1" \
	printf '%s\n' "$(pcount HEAD)"
echo '/* x */' >> main.c
gp add main.c >/dev/null
gp commit -m "and code alone still names none" >/dev/null
expect "and unchanged prompts are not carried again" "0" \
	printf '%s\n' "$(pcount HEAD)"

cd "$back" || exit 2

# now the clone.  A `git clone` of the same history is a directory git filled
# with prompts and no store, and it has to say so rather than dying with the
# message a directory that is nothing like a repository gets.
if command -v git >/dev/null 2>&1; then
	bare=$work/back.git
	clone=$work/back-clone
	rm -rf "$bare" "$clone"
	gp commit -m "record the first prompts" >/dev/null 2>&1
	git init --bare -q "$bare"
	git --git-dir="$bare" symbolic-ref HEAD refs/heads/main
	git --git-dir=.gitprompt push -q "$bare" main
	git clone -q "$bare" "$clone"
	cd "$clone" || exit 2

	# the join is carried by the commit itself, so a plain `git clone` has it
	# without gitprompt having to be part of the transfer
	expect "the commit's prompts came with the clone" "gp-prompt p_" \
		git cat-file -p HEAD

	expect "a plain clone is told what it is" \
		"plain git clone of a prompt history" gp log-prompt
	expect "and is told the one command that fixes it" "gitprompt init ." \
		gp log-prompt
	expect "rerun says the same thing rather than dying obscurely" \
		"gitprompt init ." gp rerun

	gp init . > "$work/back-init.log" 2>&1
	expect "init reports that it took the prompts over" \
		"Adopted the existing prompts" cat "$work/back-init.log"
	expect "the adopted clone has its prompts" "write a tokenizer first" \
		gp log-prompt
	expect "they are staged, ready to be committed" "A  prompts/0001-" \
		gp status --short
	expect "and it replays, session by session" "would" gp rerun
	cd "$back" || exit 2
else
	skip "a plain git clone is recognised (git is not on PATH)"
fi

# ------------------------------------------------------------------
say "where HEAD has been"

# A branch's reflog says where that branch went.  A reflog that only ever shows
# the branch HEAD is on now is not a reflog of the work tree at all: switch away
# and the record of where you were is behind you, which is the one question a
# reflog is asked.  So HEAD keeps a log of its own, every move of HEAD is written
# to it, and a bare `reflog` reads that.
hl=$work/headlog
rm -rf "$hl"
mkdir -p "$hl" || exit 2
cd "$hl" || exit 2
gp init . >/dev/null 2>&1
gp config user.email hl@example.com
gp config user.name "Reflog Tester"
gp prompt -m "the first prompt" >/dev/null 2>&1
echo one > a.txt
gp add a.txt >/dev/null 2>&1
gp commit -m "the first" >/dev/null 2>&1

before=$(gp reflog | wc -l | tr -d ' ')
gp checkout -b side >/dev/null 2>&1
gp prompt -m "the side prompt" >/dev/null 2>&1
echo two > b.txt
gp add b.txt >/dev/null 2>&1
gp commit -m "the side" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
after=$(gp reflog | wc -l | tr -d ' ')

if [ "$after" -gt "$before" ]; then
	ok "HEAD's reflog goes on growing across a switch"
else
	bad "HEAD's reflog goes on growing across a switch" \
		"$before before the switch, $after after"
fi
expect "and names the branch it left" "checkout: moving from main to side" \
	gp reflog
expect "and the one it came back to" "checkout: moving from side to main" \
	gp reflog
expect "the branch's own log is still there to ask for" "branch: Created" \
	gp reflog side

# A detach is a move of HEAD and of no branch at all, which is the case a
# reflog kept only per-branch could not record however it was written.  The
# commit is named outright rather than reached by walking back from HEAD,
# because HEAD is the root commit here and has no parent to walk to.
sideoid=$(gp rev-parse side)
gp checkout "$sideoid" >/dev/null 2>&1
expect "a detach is recorded against HEAD" "checkout: moving from main to" \
	gp reflog
expect "and names the commit it detached at" "to $sideoid" gp reflog
gp checkout main >/dev/null 2>&1
cd "$back" || exit 2

# ------------------------------------------------------------------
say "a merge commit joins the two sides"

# A merge brings the other branch's prompts into this one's tree, so by the
# same rule every other commit follows -- the prompt files it adds or changes
# against its first parent -- it carries them, and it names the session it was
# made in.  Without that a history of merges read back would have the code and
# not the prompts that produced it.
mg=$work/mergejoin
rm -rf "$mg"
mkdir -p "$mg" || exit 2
cd "$mg" || exit 2
gp init . >/dev/null 2>&1
gp config user.email mg@example.com
gp config user.name "Merge Tester"
gp session start -t "the merged work" >/dev/null 2>&1
gp prompt -m "the first prompt" >/dev/null 2>&1
echo one > a.txt
gp add a.txt >/dev/null 2>&1
gp commit -m "the first" >/dev/null 2>&1

gp checkout -b side >/dev/null 2>&1
gp prompt -m "the side branch's prompt" >/dev/null 2>&1
echo two > b.txt
gp add b.txt >/dev/null 2>&1
gp commit -m "the side" >/dev/null 2>&1
sidep=$(gp show HEAD | sed -n 's/^Prompts: //p')

gp checkout main >/dev/null 2>&1
echo three >> a.txt
gp add a.txt >/dev/null 2>&1
gp commit -m "the mainline" >/dev/null 2>&1
gp merge side --no-ff -m "join the side" >/dev/null 2>&1
expect "the merge commit carries the prompt it brought in" \
	"Prompts: $sidep" gp show HEAD
expect "and names the session it was made in" "gp-session s_" \
	gp cat-file -p HEAD
expect "the merge is on HEAD's reflog" "merge" gp reflog

# a fast-forward moves the branch without making a commit, and it is a move of
# HEAD all the same
gp checkout -b ahead >/dev/null 2>&1
gp prompt -m "a prompt on the branch that runs ahead" >/dev/null 2>&1
echo four >> a.txt
gp add a.txt >/dev/null 2>&1
gp commit -m "the branch runs ahead" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
gp merge ahead --ff-only >/dev/null 2>&1
expect "a fast-forward is on HEAD's reflog too" "merge: fast-forward" gp reflog
cd "$back" || exit 2

# ------------------------------------------------------------------
say "a commit replayed somewhere else"

# cherry-pick and rebase both apply a commit's diff to what HEAD holds now,
# which is the three-way merge `merge` performs with the commit's own parent as
# the base.  What the replayed commit carries is decided by the rule every other
# commit follows -- the prompts it has that its first parent does not -- read
# against the parent it has now: a prompt the branch already holds is not
# carried a second time, and one it lacks travels with the commit.
cp=$work/replay
rm -rf "$cp"
mkdir -p "$cp" || exit 2
cd "$cp" || exit 2
gp init . >/dev/null 2>&1
gp config user.email cp@example.com
gp config user.name "Replay Tester"
gp session start -t "the replayed work" >/dev/null 2>&1
echo base > base.txt
gp add base.txt >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1
gp branch side >/dev/null 2>&1

gp prompt -m "the prompt behind the picked commit" >/dev/null 2>&1
echo picked > picked.txt
gp add -A >/dev/null 2>&1
gp commit -m "the commit to pick" >/dev/null 2>&1
picked=$(gp rev-parse HEAD)
pickedp=$(gp show HEAD | sed -n 's/^Prompts: //p')

gp checkout side >/dev/null 2>&1
echo own > own.txt
gp add own.txt >/dev/null 2>&1
gp commit -m "the branch's own work" >/dev/null 2>&1

expect_status "a cherry-pick of a diverged commit succeeds" 0 \
	gp cherry-pick "$picked"
expect_file "and the file it carried is here" "$cp/picked.txt"
expect_file "and the branch's own file is still here" "$cp/own.txt"
parents=$(gp cat-file -p HEAD | grep -c '^parent ')
if [ "$parents" = 1 ]; then
	ok "a replayed commit has one parent"
else
	bad "a replayed commit has one parent" "got $parents"
fi
expect "the replay is on the branch's reflog" "cherry-pick:" gp reflog
expect "the replayed commit carries the prompt the original had" \
	"Prompts: $pickedp" gp show HEAD

# the same commit again finds its change already here, which is nothing to
# record: stopping is what git does, and --skip is how it is left out
pickedagain=$(gp cherry-pick "$picked" 2>&1)
case "$pickedagain" in
*"now empty"*) ok "picking it again stops because the result is empty" ;;
*) bad "picking it again stops because the result is empty" "got [$pickedagain]" ;;
esac
expect_status "and --skip leaves it out" 0 gp cherry-pick --skip
expect_absent "and the state is gone" "$cp/.gitprompt/sequencer"
expect "nothing was added by the empty pick" "$pickedp" gp show HEAD
cd "$back" || exit 2

# a pick that lands on a conflicting change stops with the conflict in the index
cpc=$work/replay-conflict
rm -rf "$cpc"
mkdir -p "$cpc" || exit 2
cd "$cpc" || exit 2
gp init . >/dev/null 2>&1
gp config user.email cpc@example.com
gp config user.name "Conflict Tester"
printf 'one\ntwo\n' > f.txt
gp add -A >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1
gp branch side >/dev/null 2>&1
printf 'ONE\ntwo\n' > f.txt
gp add -A >/dev/null 2>&1
gp commit -m "the commit to pick" >/dev/null 2>&1
mainc=$(gp rev-parse HEAD)
gp checkout side >/dev/null 2>&1
printf 'FIRST\ntwo\n' > f.txt
gp add -A >/dev/null 2>&1
gp commit -m "the branch's own edit" >/dev/null 2>&1
sidebefore=$(gp rev-parse side)

pickout=$(gp cherry-pick "$mainc" 2>&1)
pickrc=$?
case "$pickout" in
*"could not apply"*)
	ok "a conflicting pick names the commit it could not apply" ;;
*) bad "a conflicting pick names the commit it could not apply" \
	"got [$pickout]" ;;
esac
if [ "$pickrc" = 1 ]; then
	ok "a conflicting pick stops"
else
	bad "a conflicting pick stops" "exit $pickrc: $pickout"
fi
expect "and --continue says the paths are still unmerged" "unmerged paths" \
	gp cherry-pick --continue
expect "and status shows the conflict" "both modified" gp status
# a commit made here would record the pick by hand and leave the state naming it
expect "a commit while a pick is stopped is refused" "is in progress" \
	gp commit -m "by hand"
printf 'resolved\ntwo\n' > f.txt
gp add f.txt >/dev/null 2>&1
expect_status "with the conflict resolved, --continue records it" 0 \
	gp cherry-pick --continue
expect "and the resolution is what landed" "resolved" cat f.txt
expect_absent "and the state is gone" "$cpc/.gitprompt/sequencer"
cd "$back" || exit 2

# an abort is the way out of a stopped pick, and it puts the branch back
cpa=$work/replay-abort
rm -rf "$cpa"
mkdir -p "$cpa" || exit 2
cd "$cpa" || exit 2
gp init . >/dev/null 2>&1
gp config user.email cpa@example.com
gp config user.name "Abort Tester"
printf 'one\ntwo\n' > f.txt
gp add -A >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1
gp branch side >/dev/null 2>&1
printf 'ONE\ntwo\n' > f.txt
gp add -A >/dev/null 2>&1
gp commit -m "the commit to pick" >/dev/null 2>&1
mainc=$(gp rev-parse HEAD)
gp checkout side >/dev/null 2>&1
printf 'FIRST\ntwo\n' > f.txt
gp add -A >/dev/null 2>&1
gp commit -m "the branch's own edit" >/dev/null 2>&1
sidebefore=$(gp rev-parse side)
gp cherry-pick "$mainc" >/dev/null 2>&1
expect_status "an abort of a stopped pick succeeds" 0 gp cherry-pick --abort
expect "the branch is where it was" "$sidebefore" gp rev-parse side
expect "and the file is back" "FIRST" cat f.txt
expect_absent "and the state is gone" "$cpa/.gitprompt/sequencer"
cd "$back" || exit 2

# a rebase replays the branch's own commits onto another one, which is a
# cherry-pick per commit with the branch moved at the end
rb=$work/rebase
rm -rf "$rb"
mkdir -p "$rb" || exit 2
cd "$rb" || exit 2
gp init . >/dev/null 2>&1
gp config user.email rb@example.com
gp config user.name "Rebase Tester"
gp session start -t "the rebased work" >/dev/null 2>&1
echo base > f.txt
gp add -A >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1
gp branch side >/dev/null 2>&1
gp checkout side >/dev/null 2>&1
gp prompt -m "the first side prompt" >/dev/null 2>&1
echo one > a.txt
gp add -A >/dev/null 2>&1
gp commit -m "the first of the branch's own" >/dev/null 2>&1
p1=$(gp show HEAD | sed -n 's/^Prompts: //p')
gp prompt -m "the second side prompt" >/dev/null 2>&1
echo two > b.txt
gp add -A >/dev/null 2>&1
gp commit -m "the second of the branch's own" >/dev/null 2>&1
p2=$(gp show HEAD | sed -n 's/^Prompts: //p')
gp checkout main >/dev/null 2>&1
gp prompt -m "the mainline's prompt" >/dev/null 2>&1
echo mainline > c.txt
gp add -A >/dev/null 2>&1
gp commit -m "the mainline moves on" >/dev/null 2>&1
pmain=$(gp show HEAD | sed -n 's/^Prompts: //p')
gp checkout side >/dev/null 2>&1

rbout=$(gp rebase main 2>&1)
case "$rbout" in
*"Successfully rebased and updated refs/heads/side"*)
	ok "a rebase says which branch it moved" ;;
*) bad "a rebase says which branch it moved" "got [$rbout]" ;;
esac
expect "and leaves the branch checked out" "On branch side" gp status
expect_file "the mainline's file is here" "$rb/c.txt"
expect_file "and the first commit's" "$rb/a.txt"
expect_file "and the second commit's" "$rb/b.txt"
expect "the first replayed commit carries its own prompt" "Prompts: $p1" \
	gp show side~1
expect "and the second carries the other" "Prompts: $p2" gp show side
expect "and the commit it landed on keeps its own" "Prompts: $pmain" \
	gp show side~2
expect "the reflog names each replayed commit the way git does" \
	"rebase (pick): the second of the branch's own" gp reflog
expect "and says when the replay finished" "rebase (finish)" gp reflog
# each prompt was written once and the replay must not have written it again
carried=$(gp rev-list side | while read -r r; do
	gp cat-file -p "$r" | grep '^gp-prompt '
done | sort)
distinct=$(printf '%s\n' "$carried" | sort -u | wc -l | tr -d ' ')
lines=$(printf '%s\n' "$carried" | grep -c '^gp-prompt ')
if [ "$distinct" = 3 ] && [ "$lines" = 3 ]; then
	ok "no prompt is carried twice by the rebased history"
else
	bad "no prompt is carried twice by the rebased history" \
		"$distinct distinct over $lines lines: $carried"
fi
expect "a rebase that has nothing left to do says so" "up to date" gp rebase main
# a rebase onto a commit that is ahead moves the branch without replaying
gp checkout main >/dev/null 2>&1
expect "a rebase onto a descendant fast-forwards" "Fast-forwarded main to side" \
	gp rebase side
expect "and the two branches are the same commit" "$(gp rev-parse side)" \
	gp rev-parse main
cd "$back" || exit 2

# a branch that merged the upstream in and then moved on rebases back to a line
rbm=$work/rebase-merge
rm -rf "$rbm"
mkdir -p "$rbm" || exit 2
cd "$rbm" || exit 2
gp init . >/dev/null 2>&1
gp config user.email rbm@example.com
gp config user.name "Merge Rebase Tester"
echo base > f.txt
gp add -A >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1
gp branch side >/dev/null 2>&1
gp checkout side >/dev/null 2>&1
echo a > a.txt
gp add -A >/dev/null 2>&1
gp commit -m "the branch's own" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
echo mainline > b.txt
gp add -A >/dev/null 2>&1
gp commit -m "the mainline" >/dev/null 2>&1
gp checkout side >/dev/null 2>&1
gp merge main --no-ff -m "join the mainline" >/dev/null 2>&1
echo later > c.txt
gp add -A >/dev/null 2>&1
gp commit -m "and moves on" >/dev/null 2>&1
gp checkout main >/dev/null 2>&1
echo more > d.txt
gp add -A >/dev/null 2>&1
gp commit -m "the mainline moves on" >/dev/null 2>&1
gp checkout side >/dev/null 2>&1
gp rebase main >/dev/null 2>&1
merges=$(gp rev-list side | while read -r r; do
	gp cat-file -p "$r" | grep -c '^parent .*'
done | grep -c '^2$')
if [ "$merges" = 0 ]; then
	ok "a rebase leaves no merge commit behind"
else
	bad "a rebase leaves no merge commit behind" "got $merges"
fi
expect_file "and the commits it joined are still here" "$rbm/a.txt"
expect_file "and the one that followed them" "$rbm/c.txt"
expect_file "and the mainline's own" "$rbm/d.txt"
cd "$back" || exit 2

# ------------------------------------------------------------------
say "a commit undone"

# A revert is a replay read the other way up: the commit is the base and the
# tree of the parent it is measured against is theirs, so the change lands
# subtracted rather than added.  The prompt rule needs no exception here, and
# that is the part worth pinning down -- the undo is a commit like any other, so
# it carries the prompts its new tree adds, which is to say that undoing a
# commit which introduced a prompt takes the prompt away with the code.  Nothing
# is copied: the prompt moves because it is a file in the tree.
rv=$work/revert
rm -rf "$rv"
mkdir -p "$rv" || exit 2
cd "$rv" || exit 2
gp init . >/dev/null 2>&1
gp config user.email rv@example.com
gp config user.name "Revert Tester"
gp session start -t "the work to undo" >/dev/null 2>&1
echo base > base.txt
gp add base.txt >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1

gp prompt -m "the prompt behind the undone commit" >/dev/null 2>&1
undopfile=$(ls prompts/*.md)
echo undone > undone.txt
gp add -A >/dev/null 2>&1
gp commit -m "the commit to undo" >/dev/null 2>&1
undo=$(gp rev-parse HEAD)
undop=$(gp show HEAD | sed -n 's/^Prompts: //p')

expect_status "reverting a commit succeeds" 0 gp revert "$undo"
expect_absent "the file it added is gone" "$rv/undone.txt"
expect "the undo says which commit it undid" "This reverts commit $undo." \
	gp show HEAD
expect "and its subject names that commit" 'Revert "the commit to undo"' \
	gp show HEAD
expect "the undo is the reverter's own commit, not the author's" \
	"Author: Revert Tester <rv@example.com>" gp show HEAD
expect "the reflog names it the way git does" \
	'revert: Revert "the commit to undo"' gp reflog
expect_absent "and the prompt it introduced goes with the code" \
	"$rv/$undopfile"
expect_out "so the undo carries no prompt of its own" "0" pcount HEAD

# the same commit again has nothing left to take away, which is nothing to
# record: stopping is what git does, and --skip is how it is left out
r1=$(gp rev-parse HEAD)
expect "undoing the same commit twice stops because the result is empty" \
	"now empty" gp revert "$undo"
expect_status "and --skip leaves it out" 0 gp revert --skip
expect_absent "and the state is gone" "$rv/.gitprompt/sequencer"
expect_out "and the skipped undo left HEAD where it was" "$r1" gp rev-parse HEAD

# reverting the undo is the way back, and git has a word for it
expect_status "reverting the undo succeeds" 0 gp revert HEAD
expect "git's word for it is Reapply" 'Reapply "the commit to undo"' \
	gp show HEAD
expect_file "the file comes back" "$rv/undone.txt"
expect_file "and so does the prompt" "$rv/$undopfile"
expect "it is the prompt that was undone, not a new one" "id: $undop" \
	cat "$undopfile"
expect_out "and the commit carries it again" "1" pcount HEAD
expect "the reflog has the word too" 'revert: Reapply "the commit to undo"' \
	gp reflog
# the round trip is exact, which is the claim the whole rule rests on
headtree=$(gp cat-file -p HEAD | sed -n 's/^tree //p')
undotree=$(gp cat-file -p "$undo" | sed -n 's/^tree //p')
if [ "$headtree" = "$undotree" ]; then
	ok "the round trip lands on the tree it started from"
else
	bad "the round trip lands on the tree it started from" \
		"$headtree vs $undotree"
fi
cd "$back" || exit 2

# a merge has no single side to undo, so -m says which parent the undo is
# measured against and is required; a number that is not a parent is refused
rvm=$work/revert-merge
rm -rf "$rvm"
mkdir -p "$rvm" || exit 2
cd "$rvm" || exit 2
gp init . >/dev/null 2>&1
gp config user.email rvm@example.com
gp config user.name "Merge Revert Tester"
echo base > base.txt
gp add -A >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1
gp branch side >/dev/null 2>&1
echo mainline > m.txt
gp add -A >/dev/null 2>&1
gp commit -m "the mainline's work" >/dev/null 2>&1
gp checkout side >/dev/null 2>&1
echo sidework > s.txt
gp add -A >/dev/null 2>&1
gp commit -m "the side's work" >/dev/null 2>&1
gp merge main --no-ff -m "join the mainline" >/dev/null 2>&1
join=$(gp rev-parse HEAD)
nomain=$(gp revert "$join" 2>&1)
case "$nomain" in
*"is a merge but no -m option was given"*)
	ok "undoing a merge without -m is refused" ;;
*) bad "undoing a merge without -m is refused" "got [$nomain]" ;;
esac
expect "and the refusal says what -m is for" "which side" gp revert "$join"
expect "a parent number the commit does not have is refused" \
	"does not have parent 3" gp revert -m 3 "$join"
# -m 2 makes the parent that merged in the mainline, so the side's own work is
# what comes away and the mainline's stays
expect_status "undoing the merge against parent 2 succeeds" 0 \
	gp revert -m 2 "$join"
expect "the undo names the merge by subject" 'Revert "Merge join the mainline"' \
	gp show HEAD
expect_absent "the side the mainline is not is undone" "$rvm/s.txt"
expect_file "and the mainline's own file stays" "$rvm/m.txt"
cd "$back" || exit 2

# a merge undo that conflicts keeps -m in the sequencer, so --continue can
# finish an undo it did not start: this is the on-disk round trip of mainline
rvmc=$work/revert-merge-conflict
rm -rf "$rvmc"
mkdir -p "$rvmc" || exit 2
cd "$rvmc" || exit 2
gp init . >/dev/null 2>&1
gp config user.email rvmc@example.com
gp config user.name "Merge Conflict Tester"
printf 'one\ntwo\n' > f.txt
gp add -A >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1
gp branch side >/dev/null 2>&1
echo m > m.txt
gp add -A >/dev/null 2>&1
gp commit -m "the mainline" >/dev/null 2>&1
gp checkout side >/dev/null 2>&1
echo s > s.txt
gp add -A >/dev/null 2>&1
gp commit -m "the side's work" >/dev/null 2>&1
gp merge main --no-ff -m "join the mainline" >/dev/null 2>&1
join=$(gp rev-parse HEAD)
echo changed > s.txt
gp add -A >/dev/null 2>&1
gp commit -m "a later edit of the side's file" >/dev/null 2>&1
before=$(gp rev-parse HEAD)

mcout=$(gp revert -m 2 "$join" 2>&1)
mcdone=1
case "$mcout" in
*"could not apply"*) mcdone=0 ;;
esac
if [ "$mcdone" = 0 ]; then
	ok "an undo of a merge that conflicts names the commit it could not undo"
else
	bad "an undo of a merge that conflicts names the commit it could not undo" \
		"got [$mcout]"
fi
expect "the conflict is the deletion the undo wants" "deleted by them" gp status
expect_out "and -m is what the sequencer kept for the finish" "2" \
	cat "$rvmc/.gitprompt/sequencer/mainline"
gp rm s.txt >/dev/null 2>&1
gp add -A >/dev/null 2>&1
expect_status "with the deletion taken, --continue finishes the undo" 0 \
	gp revert --continue
expect_absent "and the file the undo wanted gone is gone" "$rvmc/s.txt"
expect_file "and the mainline's own file is here" "$rvmc/m.txt"
expect_file "and the file the two branches shared" "$rvmc/f.txt"
expect_absent "and the state is gone" "$rvmc/.gitprompt/sequencer"
expect "the finished undo is on the reflog" \
	'revert: Revert "Merge join the mainline"' gp reflog
if [ "$(gp rev-parse HEAD~1)" = "$before" ]; then
	ok "and it was recorded on top of where the branch was"
else
	bad "and it was recorded on top of where the branch was" \
		"$before vs $(gp rev-parse HEAD~1)"
fi
cd "$back" || exit 2

# a root commit has nothing to measure against but the empty tree, so undoing
# it takes away everything it introduced -- the prompt included
rvr=$work/revert-root
rm -rf "$rvr"
mkdir -p "$rvr" || exit 2
cd "$rvr" || exit 2
gp init . >/dev/null 2>&1
gp config user.email rvr@example.com
gp config user.name "Root Revert Tester"
gp session start -t "the only work" >/dev/null 2>&1
gp prompt -m "the only prompt there is" >/dev/null 2>&1
rootpfile=$(ls prompts/*.md)
echo base > a.txt
gp add -A >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1
expect_status "undoing a root commit succeeds" 0 gp revert HEAD
expect "and says what it undid" 'Revert "the root"' gp show HEAD
expect_absent "everything it introduced is gone" "$rvr/a.txt"
expect_absent "and the prompt it introduced with it" "$rvr/$rootpfile"
cd "$back" || exit 2

# a stopped undo is resolved and continued like a replay, and aborted like one
rvc=$work/revert-conflict
rm -rf "$rvc"
mkdir -p "$rvc" || exit 2
cd "$rvc" || exit 2
gp init . >/dev/null 2>&1
gp config user.email rvc@example.com
gp config user.name "Revert Conflict Tester"
printf 'one\ntwo\n' > f.txt
gp add -A >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1
printf 'ONE\ntwo\n' > f.txt
gp add -A >/dev/null 2>&1
gp commit -m "the commit to undo" >/dev/null 2>&1
tor=$(gp rev-parse HEAD)
printf 'one\nTWO\n' > f.txt
gp add -A >/dev/null 2>&1
gp commit -m "a later edit" >/dev/null 2>&1

cfl=$(gp revert "$tor" 2>&1)
cfrc=$?
case "$cfl" in
*"could not apply"*)
	ok "an undo that conflicts names the commit it could not undo" ;;
*) bad "an undo that conflicts names the commit it could not undo" \
	"got [$cfl]" ;;
esac
if [ "$cfrc" = 1 ]; then
	ok "and it stops"
else
	bad "and it stops" "exit $cfrc: $cfl"
fi
expect "and --continue says the paths are still unmerged" "unmerged paths" \
	gp revert --continue
expect "and status shows the conflict" "both modified" gp status
expect "a commit while an undo is stopped is refused" "a revert is in progress" \
	gp commit -m "by hand"
printf 'resolved\ntwo\n' > f.txt
gp add f.txt >/dev/null 2>&1
expect_status "with the conflict resolved, --continue records it" 0 \
	gp revert --continue
expect "and the resolution is what landed" "resolved" cat f.txt
expect_absent "and the state is gone" "$rvc/.gitprompt/sequencer"
cd "$back" || exit 2

rva=$work/revert-abort
rm -rf "$rva"
mkdir -p "$rva" || exit 2
cd "$rva" || exit 2
gp init . >/dev/null 2>&1
gp config user.email rva@example.com
gp config user.name "Revert Abort Tester"
printf 'one\ntwo\n' > f.txt
gp add -A >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1
printf 'ONE\ntwo\n' > f.txt
gp add -A >/dev/null 2>&1
gp commit -m "the commit to undo" >/dev/null 2>&1
tor=$(gp rev-parse HEAD)
printf 'one\nTWO\n' > f.txt
gp add -A >/dev/null 2>&1
gp commit -m "a later edit" >/dev/null 2>&1
before=$(gp rev-parse HEAD)
gp revert "$tor" >/dev/null 2>&1
expect "an abort of a stopped undo says what it aborted" "Revert aborted." \
	gp revert --abort
expect "the branch is where it was" "$before" gp rev-parse HEAD
expect "and the file is back" "TWO" cat f.txt
expect_absent "and the state is gone" "$rva/.gitprompt/sequencer"
expect "and there is no undo left to continue" \
	"no cherry-pick, rebase or revert to continue" gp revert --continue
cd "$back" || exit 2

# ------------------------------------------------------------------
say "work set aside and put back"

# A stash is not a special kind of storage.  It is three commits under
# refs/stash whose first parent is HEAD, so an entry is an ordinary revision
# that anything can point at -- and it is built that way for the prompt rule's
# sake: the prompts of the work tree sit in the stash commit's tree, so a prompt
# and the code it was written for are set aside together and come back
# together, with no rule for prompts and none for stashes.
st=$work/stash
rm -rf "$st"
mkdir -p "$st" || exit 2
cd "$st" || exit 2
gp init . >/dev/null 2>&1
gp config user.email st@example.com
gp config user.name "Stash Tester"
gp session start -t "the work in progress" >/dev/null 2>&1
echo one > a.txt
gp add a.txt >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1
abbr=$(gp rev-parse --short HEAD)

# the prompt for work that is not finished
gp prompt -m "a change still in progress" >/dev/null 2>&1
stpfile=$(ls prompts/*.md)
stpid=$(sed -n 's/^id: //p' "$stpfile")

echo two > a.txt
echo new > b.txt
gp add a.txt >/dev/null 2>&1
expect_out "stashing says what it set aside and names where it was" \
	"Saved working directory and index state WIP on main: $abbr the root" \
	gp stash push
expect "the tracked file is back where HEAD has it" "one" cat a.txt
expect_file "an untracked file is left alone" "$st/b.txt"
expect_absent "the prompt goes with the work it was written for" "$st/$stpfile"
expect "the entry is listed the way git lists one" \
	"stash@{0}: WIP on main: $abbr the root" gp stash list
expect "showing it names the file the work touched" "a.txt" gp stash show
expect "the entry carries the prompt that belongs to that work" \
	"Prompts: $stpid" gp show refs/stash

expect_status "putting it back succeeds" 0 gp stash pop
expect "the change is back in the work tree" "two" cat a.txt
expect_file "and so is the prompt that goes with it" "$st/$stpfile"
expect_out "and there is nothing left to list" "" gp stash list
expect_absent "the ref goes when its last entry does" "$st/.gitprompt/refs/stash"
cd "$back" || exit 2

# the message, the untracked files and the index are three separate choices,
# and git has a spelling for each
sto=$work/stash-options
rm -rf "$sto"
mkdir -p "$sto" || exit 2
cd "$sto" || exit 2
gp init . >/dev/null 2>&1
gp config user.email sto@example.com
gp config user.name "Stash Options"
echo one > a.txt
gp add -A >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1
abbr=$(gp rev-parse --short HEAD)

echo two > a.txt
gp add a.txt >/dev/null 2>&1
expect_out "a message is the entry's name, written where git writes it" \
	"Saved working directory and index state On main: a note" \
	gp stash push -m "a note"
expect "and the entry is listed under it" "stash@{0}: On main: a note" gp stash list
expect_out "with nothing in the work tree it says so instead" \
	"No local changes to save" gp stash push
expect_status "popping the entry succeeds" 0 gp stash pop
expect "and it is gone" "" gp stash list

# -u takes the untracked files in as well, as a third commit nothing else
# reaches, and taking them in means taking them out of the work tree
echo three > a.txt
gp add a.txt >/dev/null 2>&1
echo added > u.txt
expect "the untracked files are named by the entry's message" \
	"WIP on main: $abbr the root" gp stash push -u
expect_absent "and taking them in removes them from the work tree" "$sto/u.txt"
note=$(gp cat-file -p refs/stash | grep -c '^parent ')
if [ "$note" = 3 ]; then
	ok "the untracked files are a third commit under the entry"
else
	bad "the untracked files are a third commit under the entry" \
		"$note parents"
fi
expect_status "putting it back succeeds" 0 gp stash pop
expect_file "and it brings the untracked file back with it" "$sto/u.txt"
expect "with its contents" "added" cat u.txt

# -k leaves the index alone, so what was staged stays staged and the work tree
# is reset to the index rather than to HEAD
echo four > a.txt
gp add a.txt >/dev/null 2>&1
echo five > a.txt
gp stash push -k >/dev/null 2>&1
expect "with keep-index the work tree is reset to the index" "four" cat a.txt
expect "and the change is still staged" "M  a.txt" gp status --short
# the index is ahead of HEAD, so putting the entry back meets it as a clash and
# not as an overwrite: the side the merge goes into is the index, which is the
# side git merges into too
expect_status "putting an entry back onto the kept index clashes" 1 gp stash apply
expect "and it names the sides the way git names them" \
	">>>>>>> Stashed changes" cat a.txt
expect "and the entry is kept, still being the only copy" "stash@{0}:" gp stash list
cd "$back" || exit 2

# apply is the same work as pop, without forgetting the entry
sta=$work/stash-apply
rm -rf "$sta"
mkdir -p "$sta" || exit 2
cd "$sta" || exit 2
gp init . >/dev/null 2>&1
gp config user.email sta@example.com
gp config user.name "Stash Apply"
echo one > a.txt
gp add -A >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1
abbr=$(gp rev-parse --short HEAD)

echo two > a.txt
gp stash push >/dev/null 2>&1
expect_status "applying an entry succeeds" 0 gp stash apply
expect "the stashed change is back in the work tree" "two" cat a.txt
expect "and the entry is kept" "stash@{0}:" gp stash list

echo three > a.txt
gp stash push >/dev/null 2>&1
want="stash@{0}: WIP on main: $abbr the root
stash@{1}: WIP on main: $abbr the root"
expect_out "entries are numbered from the newest" "$want" gp stash list
expect "dropping by number takes the older one away" \
	"Dropped refs/stash@{1} (" gp stash drop "stash@{1}"
expect "and the newer one keeps its number" \
	"stash@{0}: WIP on main: $abbr the root" gp stash list
expect_status "which the bare drop takes next" 0 gp stash drop
expect_out "leaving nothing to list" "" gp stash list
expect_absent "and no ref to list it from" "$sta/.gitprompt/refs/stash"
expect "and nothing to show" "No stash entries found." gp stash show

gp stash push >/dev/null 2>&1
echo four > a.txt
gp stash push >/dev/null 2>&1
expect_status "clear forgets them all" 0 gp stash clear
expect_out "and the list is empty" "" gp stash list
expect_absent "with no ref and no log left behind" "$sta/.gitprompt/refs/stash"
cd "$back" || exit 2

# putting an entry back is a merge, so it stops where a merge stops
stc=$work/stash-conflict
rm -rf "$stc"
mkdir -p "$stc" || exit 2
cd "$stc" || exit 2
gp init . >/dev/null 2>&1
gp config user.email stc@example.com
gp config user.name "Stash Conflict"
echo base > a.txt
gp add -A >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1
echo stashed > a.txt
gp stash push >/dev/null 2>&1
echo head > a.txt
gp add -A >/dev/null 2>&1
gp commit -m "another change to the same line" >/dev/null 2>&1

expect_status "an entry that clashes stops rather than guessing" 1 gp stash apply
expect "and it stops in the shape a merge stops in" \
	"<<<<<<< Updated upstream" cat a.txt
expect "labelling the two sides the way git labels them" \
	">>>>>>> Stashed changes" cat a.txt
expect "the entry stays, because it is still the only copy" \
	"stash@{0}:" gp stash list

# a clash is not the only reason to stop: a file the entry would write over is
# refused before anything is written
sts=$work/stash-clobber
rm -rf "$sts"
mkdir -p "$sts" || exit 2
cd "$sts" || exit 2
gp init . >/dev/null 2>&1
gp config user.email sts@example.com
gp config user.name "Stash Clobber"
echo base > a.txt
gp add -A >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1
echo stashed > a.txt
gp stash push >/dev/null 2>&1
echo uncommitted > a.txt
expect "an entry onto a file with uncommitted work is refused" \
	"would be overwritten by merge" gp stash apply
expect "and it says what it is aborting" "Aborting" gp stash apply
expect "and the uncommitted work is untouched" "uncommitted" cat a.txt
expect "and the entry is kept" "stash@{0}:" gp stash list
cd "$back" || exit 2

# an entry is a revision, so it can be the base of a branch
stb=$work/stash-branch
rm -rf "$stb"
mkdir -p "$stb" || exit 2
cd "$stb" || exit 2
gp init . >/dev/null 2>&1
gp config user.email stb@example.com
gp config user.name "Stash Branch"
echo base > a.txt
gp add -A >/dev/null 2>&1
gp commit -m "the root" >/dev/null 2>&1
echo work > a.txt
gp stash push >/dev/null 2>&1
echo dirty > a.txt
expect "a branch cannot be cut while the work tree is dirty" \
	"would be overwritten by" gp stash branch side
echo base > a.txt
expect "an entry can be the base of a branch" \
	"Switched to a new branch 'side'" gp stash branch side
expect "which the reflog reads as a switch, since a branch is what it made" \
	"checkout: moving from main to side" gp reflog
expect "with the work on it" "work" cat a.txt
expect_absent "and the entry goes, having been replayed" \
	"$stb/.gitprompt/refs/stash"
cd "$back" || exit 2

# ------------------------------------------------------------------
say "the same history reads the same from any clock"

# A history is recorded with an offset and replayed elsewhere, so anything that
# ordered or rendered by the reading machine's clock would hand a different
# replay to a different machine.  The plan has to come out identical, and a
# recorded date has to read as the instant and offset it was written in.
tz=$work/timezones
rm -rf "$tz"
mkdir -p "$tz" || exit 2
cd "$tz" || exit 2
gp init . >/dev/null 2>&1
gp config user.email tz@example.com
gp config user.name "Clock Tester"
gp session start -t "written here" >/dev/null 2>&1
gp prompt --date='2026-03-01T09:01:00+08:00' -m "the earlier prompt" >/dev/null 2>&1
gp session end >/dev/null 2>&1
gp session start -t "written later" >/dev/null 2>&1
gp prompt --date='2026-01-05T00:00:00+08:00' -m "the later prompt" >/dev/null 2>&1
gp add -A >/dev/null 2>&1
gp commit -m "both of them" >/dev/null 2>&1

# the clock disagrees with the order they were written in, and the written order
# is the one a replay has to follow
order=$(gp log-prompt --oneline | sed 's/^p_[a-z0-9]* //')
want="the earlier prompt
the later prompt"
if [ "$order" = "$want" ]; then
	ok "the prompts are replayed in the order they were written, not the clock's"
else
	bad "the prompts are replayed in the order they were written, not the clock's" \
		"wanted [$want] got [$order]"
fi

plan=$work/tz-plan
TZ=UTC "$GP" rerun > "$plan" 2>&1
for z in America/New_York Pacific/Chatham Asia/Shanghai; do
	tzplan=$work/tz-plan.$$
	TZ=$z "$GP" rerun > "$tzplan" 2>&1
	if cmp -s "$plan" "$tzplan"; then
		ok "the replay planned under $z is the one planned under UTC"
	else
		bad "the replay planned under $z is the one planned under UTC" \
			"$(diff "$plan" "$tzplan" 2>&1 | head -3)"
	fi
	rm -f "$tzplan"
done

date_line() { TZ=$1 "$GP" rerun 2>&1 | grep -oE '[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}[+-][0-9]{2}:[0-9]{2}' | sed -n 1p; }
d_utc=$(date_line UTC)
d_cht=$(date_line Pacific/Chatham)
d_nyc=$(date_line America/New_York)
case "$d_utc" in
"2026-01-05T00:00:00+08:00") ok "a recorded date reads as the instant and offset it was written in" ;;
*) bad "a recorded date reads as the instant and offset it was written in" "got [$d_utc]" ;;
esac
if [ "$d_utc" = "$d_cht" ] && [ "$d_utc" = "$d_nyc" ]; then
	ok "and it reads the same from every clock, not re-rendered into the reader's"
else
	bad "and it reads the same from every clock, not re-rendered into the reader's" \
		"UTC [$d_utc] Chatham [$d_cht] New York [$d_nyc]"
fi
cd "$back" || exit 2

# ------------------------------------------------------------------
say "tracing a change back to the prompt that asked for it"

# A diff says what changed; it cannot say which prompt asked for the change,
# because two prompts that touch one file are one diff.  What makes the answer
# recoverable is a snapshot: recording a prompt names the work tree as it stood
# at that moment, so a run of prompts is a chain of states and each one's
# change is the step between its snapshot and the next.  These checks are that
# chain on one file, with two prompts interleaved -- which is the case no
# amount of looking at the history could untangle.
tr=$work/trace
rm -rf "$tr"
mkdir -p "$tr" || exit 2
cd "$tr" || exit 2
gp init . >/dev/null 2>&1
gp config user.email trace@example.com
gp config user.name "Trace Tester"

printf 'line one\nline two\nline three\nline four\nline five\n' > app.c
gp add -A >/dev/null 2>&1
gp commit -m "base" >/dev/null 2>&1

# the second prompt is recorded while the first one's change is already on
# disk, so the two snapshots name two different states of the same file
gp prompt -m "change line two" >/dev/null 2>&1
t1=$(sed -n 's/^id: //p' prompts/0001-change-line-two.md)
sed 's/^line two$/LINE TWO/' app.c > app.new && mv app.new app.c
gp add -A >/dev/null 2>&1

gp prompt -m "change line four" >/dev/null 2>&1
t2=$(sed -n 's/^id: //p' prompts/0002-change-line-four.md)
sed 's/^line four$/LINE FOUR/' app.c > app.new && mv app.new app.c
gp add -A >/dev/null 2>&1
gp commit -m "two prompts, one file" >/dev/null 2>&1

expect "the prompt file names the state it was recorded over" \
	"snapshot: " cat prompts/0001-change-line-two.md

# Every line of the blamed file, told apart: the two the prompts changed belong
# to a prompt each, and the three that carried through belong to no prompt at
# all, having been in the tree before the first prompt was recorded.
got=$(gp blame app.c | awk '{print $1}')
want="-
$t1
-
$t2
-"
if [ "$got" = "$want" ]; then
	ok "each line names the prompt that asked for it, and the rest name none"
else
	bad "each line names the prompt that asked for it, and the rest name none" \
		"wanted [$want] got [$got]"
fi

# The commit column has to agree: the two changed lines came from the commit
# the prompts went into, and the untouched ones from the commit before it.
b_now=$(gp blame app.c | awk 'NR==2 {print $2}')
b_old=$(gp blame app.c | awk 'NR==1 {print $2}')
b_also=$(gp blame app.c | awk 'NR==4 {print $2}')
if [ -n "$b_now" ] && [ "$b_now" = "$b_also" ] && [ "$b_now" != "$b_old" ]; then
	ok "and each line names the commit that line came from"
else
	bad "and each line names the commit that line came from" \
		"new [$b_now] also [$b_also] old [$b_old]"
fi

if [ "$(gp blame app.c)" = "$(gp blame HEAD -- app.c)" ]; then
	ok "the revision may be named, and HEAD is the one assumed"
else
	bad "the revision may be named, and HEAD is the one assumed" \
		"$(gp blame HEAD -- app.c 2>&1)"
fi

# The base commit carries no prompt, so nothing in it can be attributed.
if [ "$(gp blame HEAD~1 app.c | awk '{print $1}' | tr -d '\n')" = "-----" ]; then
	ok "a commit that carries no prompt blames every line on no prompt"
else
	bad "a commit that carries no prompt blames every line on no prompt" \
		"$(gp blame HEAD~1 app.c 2>&1)"
fi

# The annotated view is the same ordering, done on the text: the deletion of
# `line two` has to fall between the first prompt's marker and the second's.
ann=$(gp show --prompt-hunks HEAD)
m1=$(printf '%s\n' "$ann" | grep -n "^prompt $t1\$" | cut -d: -f1)
m2=$(printf '%s\n' "$ann" | grep -n "^prompt $t2\$" | cut -d: -f1)
d1=$(printf '%s\n' "$ann" | grep -n '^-line two$' | cut -d: -f1)
d2=$(printf '%s\n' "$ann" | grep -n '^-line four$' | cut -d: -f1)
if [ -n "$m1" ] && [ -n "$m2" ] && [ "$m1" -lt "$d1" ] && \
   [ "$d1" -lt "$m2" ] && [ "$m2" -lt "$d2" ]; then
	ok "--prompt-hunks puts each hunk under the prompt that asked for it"
else
	bad "--prompt-hunks puts each hunk under the prompt that asked for it" \
		"markers [$m1][$m2] deletions [$d1][$d2]"
fi

# A prompt file is in the commit like any other file and the step that wrote it
# really did add it, but naming it underneath its own prompt says nothing about
# the code, so it is left out of the breakdown.
if [ "$(printf '%s\n' "$ann" | grep -c 'diff --git a/prompts/')" = "0" ]; then
	ok "and the prompt files are not filed under their own prompts"
else
	bad "and the prompt files are not filed under their own prompts" \
		"$ann"
fi

expect "diff --prompt-hunks breaks a commit down the same way" \
	"prompt $t1" gp diff --prompt-hunks HEAD
expect "and names the second prompt too" "prompt $t2" gp diff --prompt-hunks HEAD
expect_status "a range has no single chain of prompts to break down" 128 \
	gp diff --prompt-hunks HEAD~1 HEAD
expect_status "and --stat is a different question" 128 \
	gp diff --prompt-hunks --stat HEAD

# The other direction: given a prompt, which commits carry it.  A prompt
# replayed onto another branch is carried by the copy there as well as by the
# original, which is why the answer is a list.
expect "show names the commits that carry a prompt" "carried by:" gp show "$t1"
expect "and lists each by its subject" "two prompts, one file" gp show "$t1"
expect_status "an id that names no prompt is refused" 128 gp show p_nosuchid

# A snapshot is named inside a blob, which no walk reads, so nothing would
# reach it -- and gc would be free to reclaim the state the attribution is
# built from.  fsck calling nothing dangling is what says it is a root.
if [ "$(gp fsck 2>&1 | grep -c dangling)" = "0" ]; then
	ok "a snapshot is reachable, so fsck calls nothing dangling"
else
	bad "a snapshot is reachable, so fsck calls nothing dangling" "$(gp fsck 2>&1)"
fi
before=$(gp blame app.c)
gp gc >/dev/null 2>&1
if [ "$(gp fsck 2>&1 | grep -c dangling)" = "0" ]; then
	ok "and gc leaves the snapshot in place"
else
	bad "and gc leaves the snapshot in place" "$(gp fsck 2>&1)"
fi
if [ "$before" = "$(gp blame app.c)" ]; then
	ok "so the blame reads the same after a gc"
else
	bad "so the blame reads the same after a gc" "$(gp blame app.c)"
fi

# `outcome` rewrites the prompt file from what it parsed, so a key that is
# written but not read back would be dropped here and the chain lost with it.
# It comes after the checks above because the rewritten file is staged, and a
# staged blob is not reachable from a ref until it is committed.
gp outcome "$t1" "the retry count doubled" >/dev/null 2>&1
expect "an outcome does not lose the snapshot" "snapshot: " \
	cat prompts/0001-change-line-two.md

# A prompt can have no snapshot: one recorded before snapshots were kept, or a
# prompt file put into the tree by hand.  The chain is then gone, and the most
# the history can say is which prompts the commit carries as a whole.
sed 's/^line three$/LINE THREE/' app.c > app.new && mv app.new app.c
gp add app.c >/dev/null 2>&1
printf '%s\n' '---' 'id: p_handmade0' 'seq: 3' \
	'timestamp: 2026-01-01T00:00:00+08:00' 'author: T <t@e>' '---' \
	'hand made, with no snapshot' > prompts/0003-hand-made.md
gp add prompts/0003-hand-made.md >/dev/null 2>&1
gp commit -m "a prompt with no snapshot" >/dev/null 2>&1

got=$(gp blame app.c | awk '{print $1}' | sed -n 3p)
case "$got" in
*'?')
	ok "a commit with no chain names the prompt it carries, marked as the commit's" ;;
*)
	bad "a commit with no chain names the prompt it carries, marked as the commit's" \
		"got [$got]" ;;
esac
expect "and the prompt named is one the commit carries" \
	"$(printf '%s' "$got" | sed 's/?$//')" gp show HEAD
expect "and --prompt-hunks says why there is no breakdown" \
	"(no breakdown:" gp show --prompt-hunks HEAD
cd "$back" || exit 2

# ------------------------------------------------------------------
say "finding a line in the files"

# grep is one loop over lines, so almost all of what can go wrong is not the
# searching but the answers around it: which of the three stores was read, and
# what the exit status says afterwards.  That status is git's -- 0 for a hit, 1
# for none, 128 for a pattern that will not compile -- and a script can branch
# on it, so it is asserted here rather than assumed.
gr=$work/grep
rm -rf "$gr"
mkdir -p "$gr" || exit 2
cd "$gr" || exit 2
gp init . >/dev/null 2>&1
gp config user.name "Grep Tester" >/dev/null 2>&1
gp config user.email grep@example.com >/dev/null 2>&1
printf 'alpha beta\nGamma delta\nfoo+bar\nfzzbar\nfoobar\n' > a.txt
printf 'alpha again\nnothing here\nalphabet soup\n' > b.txt
mkdir -p sub
printf 'one\ntwo\nthree\n' > sub/c.txt
gp add -A >/dev/null 2>&1
gp commit -m "the lines to search" >/dev/null 2>&1

expect_out "a match is printed as the path, a colon, and the line" \
	"$(printf 'a.txt:alpha beta\nb.txt:alpha again\nb.txt:alphabet soup')" \
	gp grep alpha
expect_out "-n puts the line number in front" "a.txt:1:alpha beta" gp grep -n beta
expect_out "^ anchors to the start of a line" "a.txt:2:Gamma delta" \
	gp grep -n '^Gamma'
expect_out '$ anchors to the end of one' \
	"$(printf 'a.txt:3:foo+bar\na.txt:4:fzzbar\na.txt:5:foobar')" \
	gp grep -n 'bar$'
expect_out ". stands for any one byte" \
	"$(printf 'a.txt:4:fzzbar\na.txt:5:foobar')" gp grep -n 'f..bar'
expect_out "a class is a set of one byte" \
	"$(printf 'a.txt:1:alpha beta\na.txt:3:foo+bar\na.txt:4:fzzbar\na.txt:5:foobar\nb.txt:3:alphabet soup')" \
	gp grep -n 'b[ae]'
expect_out "and a class may be negated" "a.txt:2:Gamma delta" gp grep -n '^[^a-z]'

# The default is git's: a basic pattern, where + ? | ( ) are ordinary bytes and
# their backslashed forms are the operators.  -E swaps the two, -F drops the
# question.  All three are checked on the same bytes so that a pattern that
# reads one way cannot pass by matching the other way.
expect_out "+ is an ordinary byte in a basic pattern" "a.txt:foo+bar" \
	gp grep 'foo+bar'
expect_out "and an escaped one repeats the byte before it" "a.txt:foobar" \
	gp grep 'foo\+bar'
expect_out "-E makes a bare + repeat" "a.txt:foobar" gp grep -E 'foo+bar'
expect_out "and there an escaped + is the ordinary byte" "a.txt:foo+bar" \
	gp grep -E 'foo\+bar'
expect_out "-F takes every byte of the pattern literally" "a.txt:foo+bar" \
	gp grep -F 'foo+bar'
expect_status "so a pattern with an escape in it finds nothing as -F" 1 \
	gp grep -F 'foo\+bar'
expect_out "\\| is the alternation in a basic pattern" \
	"$(printf 'a.txt:Gamma delta\na.txt:fzzbar')" gp grep 'Gamma\|fzz'
expect_status "where a bare | is just a byte" 1 gp grep 'Gamma|fzz'
expect_out "-E makes a bare | the alternation" \
	"$(printf 'a.txt:Gamma delta\na.txt:fzzbar')" gp grep -E 'Gamma|fzz'
expect_out "-i matches without regard to case" "a.txt:Gamma delta" gp grep -i GAMMA
expect_out "-w keeps the word whole" \
	"$(printf 'a.txt:alpha beta\nb.txt:alpha again')" gp grep -w alpha
expect_status "so the front of a longer word is not a match" 1 gp grep -w alph

expect_out "-l names the file once, however many lines matched" "a.txt" \
	gp grep -l beta
expect_out "-L names the files that had no match at all" \
	"$(printf 'b.txt\nsub/c.txt')" gp grep -L beta
expect_out "-c counts the matching lines instead" \
	"$(printf 'a.txt:1\nb.txt:2')" gp grep -c alpha
expect_out "-v counts the lines that did not match" \
	"$(printf 'a.txt:4\nb.txt:1\nsub/c.txt:3')" gp grep -vc alpha
expect_out "-L wins over -l when both are asked for" \
	"$(printf 'b.txt\nsub/c.txt')" gp grep -l -L beta
expect_out "and -l wins over -c" "a.txt" gp grep -c -l beta
expect_out "-e may be given more than once, and the patterns are alternatives" \
	"$(printf 'a.txt:alpha beta\nsub/c.txt:three')" gp grep -e beta -e three

expect_out "a pathspec narrows the search to it" "sub/c.txt:2:two" \
	gp grep -n two sub
expect_out "a revision is searched from its tree, named as it was spelled" \
	"HEAD:a.txt:1:alpha beta" gp grep -n alpha HEAD -- a.txt
expect_out "and -l names the file under that revision" "HEAD:a.txt" \
	gp grep -l beta HEAD

# The work tree is what is on disk; the index is what was staged.  A file that
# is changed after being added is the one case where the two disagree, which is
# the whole reason --cached exists.
printf 'alpha staged\n' > d.txt
gp add d.txt >/dev/null 2>&1
printf 'alpha unstaged\n' > d.txt
expect_out "the work tree is read from disk" "d.txt:1:alpha unstaged" \
	gp grep -n alpha d.txt
expect_out "and --cached reads what was staged instead" "d.txt:1:alpha staged" \
	gp grep -n --cached alpha d.txt

expect_status "a hit is exit 0" 0 gp grep alpha
expect_status "nothing found is exit 1" 1 gp grep zzz
expect "a pattern that cannot be compiled says so" \
	"grep: invalid regular expression" gp grep 'a\{2\}'
expect_status "and that is exit 128, not a wrong answer" 128 gp grep 'a\{2\}'
expect_status "as is a back reference, which is not supported" 128 gp grep '\1'
expect_status "and a POSIX class the engine does not carry" 128 \
	gp grep '[[:alpha:]]'
expect_status "an argument that is neither a revision nor a path is 128" 128 \
	gp grep alpha nosuch
expect "and it is the same complaint git makes" \
	"ambiguous argument 'nosuch'" gp grep alpha nosuch

# A NUL makes a file not text.  git would still report it -- "Binary file
# bin.dat matches" -- where we pass over it and say nothing; that difference is
# written down in docs/notes.md.  `tr` is the portable way to put a NUL in a file
# here: printf's \0 is not spelled the same on every shell.
printf 'alpha~beta\n' | tr '~' '\000' > bin.dat
gp add bin.dat >/dev/null 2>&1
expect_status "a file with a NUL in it is not text, and is left unsearched" 1 \
	gp grep alpha -- bin.dat
cd "$back" || exit 2

# ------------------------------------------------------------------
say "taking out what the index does not know about"

# clean is the one command here whose job is deletion, so it is checked from
# both ends: what it says it would do, which is where every decision lives and
# where the three modes differ, and what is really left afterwards.  The
# fixture is built so that each decision has something to decide -- a directory
# holding nothing but ignored files, one holding a mixture, one that is empty,
# one the index knows, and one that only -X can reach into.
cl=$work/clean
rm -rf "$cl"
mkdir -p "$cl" || exit 2
cd "$cl" || exit 2
gp init . >/dev/null 2>&1
gp config user.name "Clean Tester" >/dev/null 2>&1
gp config user.email clean@example.com >/dev/null 2>&1

mkdir -p ignored_dir allign mixdir a/b/c emptyd tracked_dir two
printf 'thing.log\na.log\nb.log\nignored_dir/\n' > .gitpromptignore
printf 'tracked\n' > tracked.txt
printf 'kept\n' > tracked_dir/kept.txt
gp add .gitpromptignore tracked.txt tracked_dir/kept.txt >/dev/null 2>&1
gp commit -m "base" >/dev/null 2>&1

printf 'plain\n' > plain.txt
printf 'log\n' > thing.log
printf 'x\n' > ignored_dir/x.txt
printf 'a\n' > allign/a.log
printf 'm\n' > mixdir/plain.txt
printf 'b\n' > mixdir/b.log
printf 'c\n' > a/b/c/c.txt
printf 'i\n' > tracked_dir/inner.txt
printf '1\n' > two/a.log
printf '2\n' > two/plain.txt

# expect_absent_from <description> <text> <command...> -- the inverse of expect
expect_absent_from() {
	desc=$1; unwanted=$2; shift 2
	got=$("$@" 2>&1)
	case "$got" in
	*"$unwanted"*) bad "$desc" "found [$unwanted] in [$got]" ;;
	*) ok "$desc" ;;
	esac
}

expect "nothing is removed without -f" \
	"clean.requireForce is true and -f not given: refusing to clean" gp clean
expect_status "and the refusal is fatal rather than a list" 128 gp clean
expect "the two ignored modes cannot be asked for at once" \
	"options '-x' and '-X' cannot be used together" gp clean -n -x -X
expect_status "and that is fatal too" 128 gp clean -n -x -X

# The plain mode is the narrow one: files the index does not know about, and
# not the ignored ones.  tracked_dir is walked into although it needs no -d,
# because the index knows a path inside it.
expect_out "the plain mode names the untracked files" \
"Would remove plain.txt
Would remove tracked_dir/inner.txt" gp clean -n
expect_absent_from "and says nothing about the ignored file" "thing.log" gp clean -n
expect_absent_from "nor about anything under an ignored directory" \
	"mixdir" gp clean -n
expect_absent_from "nor about an untracked directory without -d" "a/" gp clean -n

expect_out "-d brings the untracked directories in" \
"Would remove a/
Would remove emptyd/
Would remove mixdir/plain.txt
Would remove plain.txt
Would remove tracked_dir/inner.txt
Would remove two/plain.txt" gp clean -nd
expect_absent_from "-d still leaves the ignored files alone" "thing.log" gp clean -nd
expect_absent_from "-nd hides a directory whose files are all ignored" \
	"allign/" gp clean -nd

expect_out "-x takes the ignored files as well" \
"Would remove plain.txt
Would remove thing.log
Would remove tracked_dir/inner.txt" gp clean -nx
expect_absent_from "-x without -d still leaves directories" "mixdir/" gp clean -nx

expect_out "-X takes only the ignored files" \
"Would remove mixdir/b.log
Would remove thing.log
Would remove two/a.log" gp clean -nX
expect_absent_from "and an empty directory is not an ignored one" "emptyd" gp clean -nX

expect_out "-d and -X together collapse whole ignored directories" \
"Would remove allign/
Would remove ignored_dir/
Would remove mixdir/b.log
Would remove thing.log
Would remove two/a.log" gp clean -ndX
expect_absent_from "an all-ignored directory is left out of -ndx too" "a/b/c" \
	gp clean -ndX
expect_out "-d and -x collapse everything that is going whole" \
"Would remove a/
Would remove allign/
Would remove emptyd/
Would remove ignored_dir/
Would remove mixdir/
Would remove plain.txt
Would remove thing.log
Would remove tracked_dir/inner.txt
Would remove two/" gp clean -ndx

expect_out "-q says nothing at all" "" gp clean -n -q

# -e is an exclude rule, and the three modes are three attitudes to exclude
# rules, so it adds to what -X is after rather than taking from it.
expect_out "an excluded file is not offered up in the plain mode" \
"Would remove plain.txt
Would remove tracked_dir/inner.txt" gp clean -nx -e thing.log
expect_out "but -X is exactly what -e adds to" \
"Would remove plain.txt
Would remove thing.log" gp clean -nX -e plain.txt
expect_out "an excluded directory is skipped whole" \
"Would remove a/
Would remove emptyd/
Would remove plain.txt
Would remove tracked_dir/inner.txt
Would remove two/plain.txt" gp clean -nd -e mixdir
expect_out "a trailing slash makes an -e pattern about a directory" \
"Would remove mixdir/b.log
Would remove thing.log" gp clean -nX -e two/
expect_out "so without -d it is one entry or nothing" \
"Would remove allign/
Would remove ignored_dir/
Would remove mixdir/b.log
Would remove thing.log
Would remove two/" gp clean -ndX -e two/

# A pathspec is what says which part of the tree was meant.  It names the
# directory it stops at -- that is where a collapse happens -- and it can take
# the run into a directory that -d alone would not have opened.
expect_out "a pathspec naming a directory decides where the collapse lands" \
"Would remove a/b/" gp clean -nd -- a/b
expect_out "and one that reaches below it is followed down with no -d" \
"Would remove a/b/c/c.txt" gp clean -n -- a/b/c/c.txt
expect_out "a pathspec naming a file takes the file" \
"Would remove mixdir/plain.txt" gp clean -nx -- mixdir/plain.txt
expect_out "a pathspec naming a mixture descends into it" \
"Would remove mixdir/plain.txt" gp clean -n -- mixdir
expect_status "a pathspec that matches nothing is not an error" 0 \
	gp clean -n -- nosuch
expect_out "and prints nothing" "" gp clean -n -- nosuch

# What is actually left.  Without -d only files go.
expect_out "a forced run says what it took, in the present tense" \
"Removing plain.txt
Removing tracked_dir/inner.txt" gp clean -f
expect_absent "the untracked file is gone" plain.txt
expect_absent "and so is the untracked one inside a tracked directory" \
	tracked_dir/inner.txt
expect_file "the ignored file is still there" thing.log
expect_file "the ignored directory is still there" ignored_dir/x.txt
expect_out "the tracked file kept its contents" "tracked" cat tracked.txt
expect_out "and the one in the tracked directory kept its own" "kept" \
	cat tracked_dir/kept.txt
expect_file "the empty directory is still there, -d was not given" emptyd

expect_status "with -d the directories go too" 0 gp clean -fd
expect_absent "the untracked directory is gone" a
expect_absent "the empty directory is gone" emptyd
expect_absent "the wanted file in the mixture is gone" mixdir/plain.txt
expect_file "the ignored one beside it is still there" mixdir/b.log
expect_file "the all-ignored directory is still there" allign/a.log

expect_status "-x finally takes the ignored ones" 0 gp clean -fdx
expect_absent "the ignored file at the top is gone" thing.log
expect_absent "the all-ignored directory is gone" allign
expect_absent "and so is the ignored directory itself" ignored_dir
expect_file "what the index knows is what is left" tracked.txt
expect_file "including what is in a directory it knows" tracked_dir/kept.txt
expect_out "and its contents are untouched" "kept" cat tracked_dir/kept.txt

# A directory that is a repository in its own right is left alone however many
# -f are given.  The three names a store always has are what makes it one, and
# a directory that merely happens to be called .git is not one.
mkdir -p nest/.git/objects nest/.git/refs sub/.git
printf 'ref: refs/heads/main\n' > nest/.git/HEAD
printf 'not-a-store\n' > sub/.git/config
expect_out "a nested store is not named, nor descended into" \
"Would remove sub/" gp clean -ndx
expect_status "a forced run leaves it alone" 0 gp clean -fdx -- nest
expect_file "and it is still there" nest/.git/HEAD
expect_file "objects and all" nest/.git/objects
expect_status "a directory called .git with no store in it is not one" 0 \
	gp clean -fdx -- sub
expect_absent "so it goes like any other directory" sub

cd "$back" || exit 2

# ------------------------------------------------------------------
say "what the ignore file means"

# The rules are git's, and the ones that are easy to get wrong are the ones
# worth pinning down: a pattern with a slash is anchored to the directory its
# file is in, one without a slash matches a name at any depth, a trailing slash
# means a directory and takes everything under it with it, a double star spans
# whole directories, and a ! puts a path back only when nothing above it is
# already out.  Every rule below is asked through status, which is where a
# reader meets it.
ig=$work/ignore
rm -rf "$ig"
mkdir -p "$ig" || exit 2
cd "$ig" || exit 2
gp init . >/dev/null 2>&1
gp config user.name "Ignore Tester" >/dev/null 2>&1
gp config user.email ignore@example.com >/dev/null 2>&1

mkdir -p sub build buildmore nest mid/a/b lead/p/q out dir anch x/anch \
	sub2 sub3/deep

# The spaces after vis.txt are the point: a run of them at the end of a line is
# not part of the pattern.  And both files are written, because both are read:
# git's, and this project's, the second with the last word inside a directory.
printf '*.log\n!keep.log\n?.txt\n[qr]s.txt\nbuild/\n**/deep.txt\nmid/**/end.txt\nlead/**/\nout/\n!out/\ndir/\n!dir/keep.txt\n/root.txt\nanch/inner.txt\n\\#lit.txt\n\\!bang.txt\nvis.txt   \nprompt-wins.txt\npromptonly.txt\n' > .gitpromptignore
printf 'gitonly.txt\n!prompt-wins.txt\n' > .gitignore
printf '!*.log\n' > sub2/.gitpromptignore
printf '/only.txt\n' > sub3/.gitpromptignore

for f in a.log keep.log q.txt qs.txt rs.txt zs.txt xy.txt plain.txt deep.txt \
	 leadly.txt root.txt '#lit.txt' '!bang.txt' vis.txt gitonly.txt \
	 prompt-wins.txt promptonly.txt tracked.log; do
	printf 'x\n' > "$f"
done
printf 'x\n' > sub/a.log
printf 'x\n' > sub/root.txt
printf 'x\n' > build/x.txt
printf 'x\n' > build/kept.txt
printf 'x\n' > buildmore/near.txt
printf 'x\n' > nest/deep.txt
printf 'x\n' > mid/end.txt
printf 'x\n' > mid/a/end.txt
printf 'x\n' > mid/a/b/end.txt
printf 'x\n' > mid/other.txt
printf 'x\n' > lead/p/q/y.txt
printf 'x\n' > out/skip.txt
printf 'x\n' > dir/keep.txt
printf 'x\n' > anch/inner.txt
printf 'x\n' > x/anch/inner.txt
printf 'x\n' > sub2/x.log
printf 'x\n' > sub3/only.txt
printf 'x\n' > sub3/deep/only.txt

# ignore_check <description> <seen|hidden> <path> -- a path is seen when status
# names it as untracked, and hidden when the walk never offered it.  The whole
# line is compared, so a rule about root.txt is not satisfied by sub/root.txt.
ignore_check() {
	desc=$1
	want=$2
	path=$3
	got=$(gp status --short 2>&1 | tr -d '\r')
	if printf '%s\n' "$got" | awk -v p="?? $path" '$0 == p { f = 1 } END { exit !f }'
	then
		[ "$want" = seen ] && ok "$desc" || bad "$desc" "$path was not ignored"
	else
		[ "$want" = hidden ] && ok "$desc" || bad "$desc" "no [$path] in [$got]"
	fi
}

ignore_check "a pattern with no slash matches a name at any depth" hidden a.log
ignore_check "even several directories down" hidden sub/a.log
ignore_check "a later line puts a path back" seen keep.log
ignore_check "? stands for one character" hidden q.txt
ignore_check "and the pattern has to be the whole name" seen xy.txt
ignore_check "a set is the characters in it" hidden qs.txt
ignore_check "whichever of them matched" hidden rs.txt
ignore_check "and a name outside it is left alone" seen zs.txt
ignore_check "a trailing slash takes the directory and all of it" hidden build/x.txt
ignore_check "but only that name, not a longer one" seen buildmore/near.txt
ignore_check "a double star is no directory at all" hidden deep.txt
ignore_check "or more than one" hidden nest/deep.txt
ignore_check "a double star in the middle spans what lies between" hidden mid/end.txt
ignore_check "however many directories that is" hidden mid/a/b/end.txt
ignore_check "and the names beside it are not touched" seen mid/other.txt
ignore_check "a double star at the end takes everything below" hidden lead/p/q/y.txt
ignore_check "and is not a prefix of an ordinary name" seen leadly.txt
ignore_check "a directory can be put back whole" seen out/skip.txt
ignore_check "a file cannot be put back under a directory that is out" hidden dir/keep.txt
ignore_check "a leading slash anchors to the root" hidden root.txt
ignore_check "so the same name below is another file" seen sub/root.txt
ignore_check "a slash in the middle anchors it too" hidden anch/inner.txt
ignore_check "and that name inside another directory is not it" seen x/anch/inner.txt
ignore_check "an escaped hash is a literal hash" hidden '#lit.txt'
ignore_check "an escaped bang is a literal bang" hidden '!bang.txt'
ignore_check "spaces nobody escaped are not part of it" hidden vis.txt
ignore_check "git's own file is read" hidden gitonly.txt
ignore_check "and this project's file has the last word" hidden prompt-wins.txt
ignore_check "a rule of this project's own is read as well" hidden promptonly.txt
ignore_check "a deeper file can put back what a shallower one took" seen sub2/x.log
ignore_check "a deeper file can anchor within its directory" hidden sub3/only.txt
ignore_check "and then not below it" seen sub3/deep/only.txt
ignore_check "a file no rule mentions is the work tree's own" seen plain.txt

# A path the index knows is never ignored, whatever a rule says about its name:
# ignoring is about what has not been recorded, and a file that was committed
# before anyone wrote the rule must not disappear from status because of it.
gp add -f .gitignore .gitpromptignore tracked.log build/kept.txt >/dev/null 2>&1
gp commit -m "a tracked file inside an ignored directory" >/dev/null 2>&1
printf 'changed\n' > tracked.log
expect "a tracked file a rule matches is still reported" " M tracked.log" \
	gp status --short
expect_status "and add takes it with no -f at all" 0 gp add tracked.log
expect "so the change is staged" "M  tracked.log" gp status --short
printf 'changed\n' > build/kept.txt
expect "and the same holds inside an ignored directory" " M build/kept.txt" \
	gp status --short

# add refuses a path the rules take out, names it, and says what to do.  The
# wording is git's, down to the line break, with one difference: there is no
# advice setting here to turn the message off, so git's last line would be a
# lie and is not printed.
expect_status "add on a named ignored file is refused" 1 gp add a.log
expect_out "with git's wording, naming the path" \
"The following paths are ignored by one of your .gitpromptignore files:
a.log
hint: Use -f if you really want to add them." gp add a.log
expect_out "a directory is named the way a pathspec spells it" \
"The following paths are ignored by one of your .gitpromptignore files:
build
hint: Use -f if you really want to add them." gp add build/
expect_out "and several named at once are one list, in order" \
"The following paths are ignored by one of your .gitpromptignore files:
a.log
q.txt
hint: Use -f if you really want to add them." gp add q.txt a.log
expect_status "a directory that is not itself ignored is not refused" 0 \
	gp add sub
expect_status "nor is anything add . walks past" 0 gp add .
expect_status "clean does not fold a directory the index knows" 0 gp clean -nX
# clean_line compares a whole line, not a substring: "Would remove build/" is a
# prefix of "Would remove build/x.txt", and the difference between the two is
# exactly what this is asking about.
clean_line() {
	desc=$1
	want=$2
	line=$3
	got=$(gp clean -nX 2>&1 | tr -d '\r')
	if printf '%s\n' "$got" | awk -v p="$line" '$0 == p { f = 1 } END { exit !f }'
	then
		[ "$want" = seen ] && ok "$desc" || bad "$desc" "the line [$line] is there"
	else
		[ "$want" = absent ] && ok "$desc" || bad "$desc" "no line [$line] in [$got]"
	fi
}
clean_line "an ignored directory holding a tracked file is descended into" \
	seen "Would remove build/x.txt"
clean_line "and the directory is not folded into one line" absent \
	"Would remove build/"
expect_status "add -f stages what was refused" 0 gp add -f build
expect "so the ignored file is in the index now" "A  build/x.txt" gp status --short
expect_status "and -f takes a named file too" 0 gp add -f '!bang.txt'
expect "with the file in the index" "A  !bang.txt" gp status --short

cd "$back" || exit 2

# ------------------------------------------------------------------
say "halving a range to find the first bad commit"

# bisect is a loop over a shrinking range, and nearly everything it can get
# wrong is which commits it looks at.  So the walk below is asserted step by
# step -- the middle, then the later one, then the answer -- and not only for
# where it stops: a range that narrows in a different order can still name the
# right commit after having asked about needless ones on the way, and on a
# linear history the order is settled enough for pinning it down to be fair.
bs=$work/bisect
rm -rf "$bs"
mkdir -p "$bs" || exit 2
cd "$bs" || exit 2
gp init . >/dev/null 2>&1
gp config user.name "Bisect Tester" >/dev/null 2>&1
gp config user.email bisect@example.com >/dev/null 2>&1
i=1
while [ $i -le 8 ]; do
	if [ $i -lt 5 ]; then printf 'state %d\n' "$i" > f.txt
	else printf 'BROKEN %d\n' "$i" > f.txt; fi
	gp add f.txt >/dev/null 2>&1
	gp commit -m "commit $i" >/dev/null 2>&1
	i=$((i + 1))
done
fifth=$(gp rev-parse HEAD~3)

expect "nothing to bisect until a range is given" \
	'You need to start by "git bisect start"' gp bisect bad
expect_status "and that is not a success" 1 gp bisect bad

startout=$(gp bisect start HEAD HEAD~7 2>&1)
expect "start says how much of the range is left to test" \
	"Bisecting: 3 revisions left to test after this (roughly 2 steps)" \
	printf '%s\n' "$startout"
expect "and names the commit it moved to" "] commit 4" printf '%s\n' "$startout"
expect_status "which leaves HEAD detached, as a probe must" 1 gp symbolic-ref HEAD
expect_file "and a session is marked by the file the reset reads" \
	.gitprompt/BISECT_START

# The whole walk goes into one file, so what it looked at can be read back out
# of it afterwards rather than counted by hand.
walk=$work/bisect/walk.txt
printf '%s\n' "$startout" > "$walk"
n=0
while [ $n -lt 12 ]; do
	n=$((n + 1))
	if grep -q BROKEN f.txt; then step=bad; else step=good; fi
	stepout=$(gp bisect "$step" 2>&1)
	printf '%s\n' "$stepout" >> "$walk"
	case "$stepout" in
	*"is the first bad commit"*) break ;;
	esac
done
expect "the walk comes to rest on one commit" \
	"$fifth is the first bad commit" cat "$walk"
probes=$(grep -oE '^\[[0-9a-f]+\] commit [0-9]+' "$walk" | cut -d' ' -f 2,3)
expect_out "and it halves in the order git halves it" \
	"$(printf 'commit 4\ncommit 6\ncommit 5')" printf '%s' "$probes"

log=$work/bisect/log.txt
gp bisect log > "$log" 2>&1
expect "the log remembers the command that opened the range" \
	"git bisect start" cat "$log"
expect "and every commit judged along the way" "# bad: " cat "$log"
expect "on the good side too" "# good: " cat "$log"
expect "and names all three of the commits it stopped on" "commit 6" cat "$log"

# ------------------------------------------------------------------
say "skipping a probe"

expect "reset returns to the branch it started from" \
	"Switched to branch 'main'" gp bisect reset
expect_out "so HEAD names that branch again" "refs/heads/main" gp symbolic-ref HEAD
expect_out "and the work tree is the branch tip once more" "BROKEN 8" head -1 f.txt
expect_absent "the ref holding the bad end is gone" .gitprompt/refs/bisect/bad
expect_absent "as are the good ones" .gitprompt/refs/bisect/good-$fifth
expect_absent "the start file goes with them" .gitprompt/BISECT_START
expect_absent "and so does the log" .gitprompt/BISECT_LOG
expect_absent "and the expected-rev note" .gitprompt/BISECT_EXPECTED_REV
expect "with nothing left, reset says so" "We are not bisecting." gp bisect reset

out=$(gp bisect start HEAD HEAD~7 2>&1)
n=0
while [ $n -lt 12 ]; do
	n=$((n + 1))
	case $(head -1 f.txt) in
	"BROKEN 6") out=$(gp bisect skip 2>&1) ;;
	*BROKEN*)   out=$(gp bisect bad 2>&1) ;;
	*)          out=$(gp bisect good 2>&1) ;;
	esac
	case "$out" in
	*"is the first bad commit"*) break ;;
	esac
done
expect "a probe that cannot be judged does not stop the narrowing" \
	"$fifth is the first bad commit" printf '%s\n' "$out"
expect "and the log keeps the skip beside the rest" \
	"git bisect skip" gp bisect log
gp bisect reset >/dev/null 2>&1

# ------------------------------------------------------------------
say "letting a script do the judging"

# The command goes through a shell, so a script named by its path is what git's
# own users hand to `run`; 0 is good, 125 is skip, anything else is bad until it
# reaches 128, which stops the bisection instead of judging anything.
judge=$bs/judge.sh
cat > "$judge" <<'EOF'
#!/bin/sh
grep -q BROKEN f.txt && exit 1
exit 0
EOF
chmod +x "$judge"
gp bisect start HEAD HEAD~7 >/dev/null 2>&1
expect "a run hands each probe to the script and stops where it says" \
	"$fifth is the first bad commit" gp bisect run ./judge.sh
gp bisect reset >/dev/null 2>&1

cat > "$judge" <<'EOF'
#!/bin/sh
exit 125
EOF
chmod +x "$judge"
gp bisect start HEAD HEAD~7 >/dev/null 2>&1
expect "125 means skip, and a range with nothing left to test ends" \
	"We cannot bisect more!" gp bisect run ./judge.sh
gp bisect reset >/dev/null 2>&1

cat > "$judge" <<'EOF'
#!/bin/sh
exit 130
EOF
chmod +x "$judge"
gp bisect start HEAD HEAD~7 >/dev/null 2>&1
expect "a status of 128 or more stops everything" \
	"is < 0 or >= 128" gp bisect run ./judge.sh
expect_status "and that is reported as a failure" 1 gp bisect run ./judge.sh
gp bisect reset >/dev/null 2>&1

# 126 and 127 are the shell's own statuses for a command it could not run, so
# one of them arriving as the verdict has to be checked before it is believed:
# read as "this probe is bad" a misspelt path would narrow the range and name
# the wrong commit with no complaint at all.
gp bisect start HEAD HEAD~7 >/dev/null 2>&1
expect "a command that cannot run at all is not read as a bad probe" \
	"bogus exit code 127 for good revision" gp bisect run ./no-such-script.sh
expect_status "and that stops the run rather than answering" 1 \
	gp bisect run ./no-such-script.sh
gp bisect reset >/dev/null 2>&1

# ------------------------------------------------------------------
say "refusing to start over local changes"

printf 'dirty\n' > f.txt
expect "a start over a changed file is refused" \
	"Your local changes to the following files would be overwritten by checkout:" \
	gp bisect start HEAD HEAD~7
expect_status "and nothing is started" 1 gp bisect start HEAD HEAD~7
expect_status "so there is no half-written range to log to" 1 gp bisect log
expect_out "with the file left alone" "dirty" head -1 f.txt
gp checkout -- f.txt >/dev/null 2>&1
expect_out "and once it is put back the tip is what is on disk" "BROKEN 8" head -1 f.txt

cd "$back" || exit 2

# ------------------------------------------------------------------
printf '\n%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skipped"
[ "$fail" -eq 0 ] || exit 1
exit 0
