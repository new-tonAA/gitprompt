#!/bin/sh
#
# restore.sh -- cross-session, cross-machine restore, end to end.
#
#   sh test/restore.sh
#   GP=/path/to/gitprompt sh test/restore.sh
#
# The history is built with three things true of it that a restore has to
# survive: it crosses two sessions and returns to the first one, the clock the
# prompts were recorded by disagrees with the order they were written in, and
# what the other machine gets is a plain `git clone` with no gitprompt store in
# it.  What that machine then does -- adopt the clone and hand the prompts back
# to an agent, in the order and the conversations they were written in -- is the
# whole design claim, run rather than described.
#
# The clone needs git, so without git on PATH the battery stops after the recipe
# and says so.  Everything else is POSIX, because the macOS runner is where the
# other half of it runs.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
GP=${GP:-$root/gitprompt.exe}
[ -f "$GP" ] || GP=$root/gitprompt
[ -f "$GP" ] || { echo "no gitprompt binary at $GP; run make first"; exit 2; }

case "${WORK:-}" in
"") work=$root/build/restore ;;
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
say()  { printf '\n-- %s\n' "$1"; }

nlines() { wc -l < "$1" | tr -d ' '; }
# the agent invocations a plan or a run names: one leg per conversation reached
legs() { grep -oE '(--session-id|--resume) [0-9a-f-]{36}' | sed 's/--//'; }
# `show` prints the prompt ids a commit carries and then the diff, whose prompt
# file has the id in its frontmatter too; the header line is the one that counts
pcount() { "$GP" show "$1" | sed -n 's/^Prompts: //p' | grep -oE 'p_[a-z0-9]+' | wc -l | tr -d ' '; }
code() { printf '%s\n' "$2" > "$1"; }

have_git=0
command -v git >/dev/null 2>&1 && have_git=1

# ------------------------------------------------------------------ recipe
say "the recipe"
recipe=$work/recipe
mkdir -p "$recipe" || exit 2
cd "$recipe" || exit 2
"$GP" init . >/dev/null
"$GP" config user.email recipe@example.com
"$GP" config user.name "Recipe Author"

# session one: two prompts, some code, then it is left
"$GP" session start -t "design the schema" >/dev/null
s1=$("$GP" session current)
"$GP" prompt -m "design a users table" --date "2026-03-01T09:01:00+08:00" >/dev/null
"$GP" prompt -m "add an index on email" --date "2026-03-01T09:02:00+08:00" >/dev/null
"$GP" session end >/dev/null
code schema.sql "create table users (id integer primary key, email text);"
"$GP" add schema.sql >/dev/null
"$GP" commit -m "add the users table" >/dev/null
chk "the first commit carries both prompts it was made from" "2" "$(pcount HEAD)"

# session two: a second conversation, a week later
"$GP" session start -t "write the API" >/dev/null
s2=$("$GP" session current)
"$GP" prompt -m "write the login endpoint" --date "2026-03-08T10:01:00+08:00" >/dev/null
code api.go "func login() {}"
"$GP" add api.go >/dev/null
"$GP" commit -m "add the login endpoint" >/dev/null
chk "a commit in the second session carries only its own prompt" "1" "$(pcount HEAD)"

# back into the first conversation -- and this prompt's clock is the oldest of
# the four, so anything that orders by time puts it first and is wrong
"$GP" session use "$s1" >/dev/null 2>&1
"$GP" prompt -m "the table needs a deleted_at column" \
	--date "2026-01-05T00:00:00+08:00" >/dev/null
"$GP" session end >/dev/null
"$GP" add -A >/dev/null 2>&1
"$GP" commit -m "add deleted_at" >/dev/null

chk "the recipe has two sessions" "2" "$("$GP" session list | grep -cE '^[* ]+s_')"
chk "and four prompts" "4" "$("$GP" log-prompt --oneline | wc -l | tr -d ' ')"

# the order they were written in, which is what a restore has to reproduce
want_order="1 design a users table
2 add an index on email
3 write the login endpoint
4 the table needs a deleted_at column"
got_order=$("$GP" log-prompt --oneline |
	awk '{ $1 = ""; sub(/^ /, ""); print NR " " $0 }')
chk "the history is ordered by seq, not by the clock" "$want_order" "$got_order"

if [ "$have_git" = 0 ]; then
	skip "the clone, the replay and the clocks (git is not on PATH)"
	printf '\n%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skipped"
	[ "$fail" = 0 ]
	exit $?
fi

git init --bare -q "$work/remote.git" || exit 2
# the branch gitprompt pushes is main; a bare repo this git makes points HEAD at
# master, and a clone of it then checks nothing out
git --git-dir="$work/remote.git" symbolic-ref HEAD refs/heads/main

"$GP" remote add origin "$work/remote.git"
if "$GP" push -u origin main > "$work/push.log" 2>&1; then
	ok "the recipe pushes to a bare git remote"
else
	bad "the recipe pushes to a bare git remote" "$(cat "$work/push.log")"
fi

# what the recipe would replay: the shape the clone has to match
"$GP" rerun > "$work/recipe.plan" 2>&1
legs < "$work/recipe.plan" > "$work/recipe.cmds"
chk "the plan crosses four conversation legs" "4" "$(nlines "$work/recipe.cmds")"

# ------------------------------------------------------------------ clone
say "the clone on the other machine"
cd "$work" || exit 2
git clone -q "$work/remote.git" cloned || exit 2
cd "$work/cloned" || exit 2
chk "the clone is on the branch the recipe pushed" "main" \
	"$(git rev-parse --abbrev-ref HEAD)"
chk "a plain git clone brings the prompt files" "4" \
	"$(ls prompts/*.md | wc -l | tr -d ' ')"
chk "and the session files with them" "2" \
	"$(ls prompts/sessions/*.md | wc -l | tr -d ' ')"
chk "and the commits, with the prompts they carry" "1" \
	"$(git cat-file -p HEAD | grep -c '^gp-prompt ' || true)"

"$GP" init . > "$work/adopt.log" 2>&1
rc=$?
chk "adopting the clone succeeds" "0" "$rc"
has "and says it took the tree over" "Adopted" "$(cat "$work/adopt.log")"
"$GP" config user.email clone@example.com
"$GP" config user.name "Clone Tester"

chk "the clone reads the same four prompts" "4" \
	"$("$GP" log-prompt --oneline | wc -l | tr -d ' ')"
chk "and the same two sessions" "2" "$("$GP" session list | grep -cE '^[* ]+s_')"
chk "with the titles they were given" "2" \
	"$("$GP" session list | grep -cE 'design the schema|write the API')"

"$GP" rerun > "$work/clone.plan" 2>&1
legs < "$work/clone.plan" > "$work/clone.cmds"
if diff "$work/recipe.cmds" "$work/clone.cmds" > "$work/cmds.diff" 2>&1; then
	ok "the clone replays what the recipe would, ids and all"
else
	bad "the clone replays what the recipe would, ids and all" \
		"$(cat "$work/cmds.diff")"
fi

# ------------------------------------------------------------------ stub
# The agent is a stub, so what was handed over can be checked exactly.  On
# Windows the shell that runs it is cmd.exe, which needs the .bat spelling; it
# has no sleep, so the pause between the two markers is a ping.
say "handing it back to an agent"
stub=$work/agent-bin
mkdir -p "$stub" || exit 2
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
	cat > "$stub/claude.bat" <<'STUB'
@echo off
echo --- BEGIN %*
set /p P=
echo     said: %P%
ping -n 2 127.0.0.1 >nul
echo --- END
STUB
	;;
*)
	# `cat` with no operand is stdin everywhere; `< -` is not -- a shell that
	# does not read `-` as a name for it looks for a file called "-".
	cat > "$stub/claude" <<'STUB'
#!/bin/sh
echo "--- BEGIN $*"
IFS= read -r p || true
printf '    said: %s\n' "${p:-}"
sleep 1
echo "--- END"
STUB
	chmod +x "$stub/claude"
	;;
esac

cd "$work/cloned" || exit 2
oldpath=$PATH
PATH="$stub:$PATH"
TZ=Asia/Shanghai "$GP" rerun --yes > "$work/rerun.log" 2>&1
rc=$?
PATH=$oldpath
chk "the replay runs" "0" "$rc"

chk "every prompt was handed over" "4" "$(grep -c '^--- BEGIN' "$work/rerun.log")"
chk "and every one finished" "4" "$(grep -c '^--- END' "$work/rerun.log")"

# one at a time: with a second of work between BEGIN and END, anything that
# started the next prompt early would show a BEGIN before an END
seq_ok=$(
	grep -E '^--- (BEGIN|END)' "$work/rerun.log" |
	awk '{ if ($2 == "BEGIN" && open) bad = 1
	       if ($2 == "END" && !open) bad = 1
	       open = ($2 == "BEGIN") }
	     END { print (bad || open) ? "no" : "yes" }'
)
chk "the prompts are replayed one at a time, not all at once" "yes" "$seq_ok"

grep '^    said: ' "$work/rerun.log" | sed 's/^    said: //' > "$work/said.txt"
chk "in the recorded order" "$want_order" \
	"$(awk '{ print NR " " $0 }' "$work/said.txt")"

# two sessions in, two conversations out
legs < "$work/rerun.log" > "$work/legs.txt"
chk "four legs: into one conversation, back into it, then the other" "4" \
	"$(nlines "$work/legs.txt")"
u1=$(sed -n 1p "$work/legs.txt" | awk '{ print $2 }')
u2=$(sed -n 2p "$work/legs.txt" | awk '{ print $2 }')
u3=$(sed -n 3p "$work/legs.txt" | awk '{ print $2 }')
u4=$(sed -n 4p "$work/legs.txt" | awk '{ print $2 }')
chk "the second prompt continues the conversation the first opened" "$u1" "$u2"
if [ "$u1" = "$u3" ]; then
	bad "the second session is a conversation of its own" "both are $u1"
else
	ok "the second session is a conversation of its own"
fi
chk "and the last prompt goes back into the first conversation" "$u1" "$u4"
chk "so the two sessions came back as two conversations" "2" \
	"$(awk '{ print $2 }' "$work/legs.txt" | sort -u | wc -l | tr -d ' ')"
chk "as start, resume, start, resume" "start resume start resume" \
	"$(sed 's/^session-id /start /; s/^resume /resume /' "$work/legs.txt" |
		awk '{ print $1 }' | tr '\n' ' ' | sed 's/ $//')"

# ------------------------------------------------------------------ cross-time
say "the same replay under any clock"
for tz in UTC America/New_York Pacific/Chatham; do
	TZ=$tz "$GP" rerun 2>&1 | legs > "$work/tz.cmds"
	if diff "$work/clone.cmds" "$work/tz.cmds" > "$work/tz.diff" 2>&1; then
		ok "the same replay is planned from a $tz clock"
	else
		bad "the same replay is planned from a $tz clock" "$(cat "$work/tz.diff")"
	fi
done

# A date is stored as an instant with the offset it was written in, so every
# machine reads the same unambiguous text; what must not happen is the date
# being silently re-rendered into the reading machine's clock, which would make
# the same history read differently in different places.
d_utc=$(TZ=UTC "$GP" rerun 2>&1 | awk '/recorded/ { print $2 }')
d_nyc=$(TZ=America/New_York "$GP" rerun 2>&1 | awk '/recorded/ { print $2 }')
d_cht=$(TZ=Pacific/Chatham "$GP" rerun 2>&1 | awk '/recorded/ { print $2 }')
chk "the recorded date reads the same wherever it is read" "$d_utc" "$d_nyc"
chk "under every clock" "$d_utc" "$d_cht"
has "and it carries the offset it was written in" "+08:00" "$d_utc"
chk "and the span is the earliest time recorded, not the first one written" \
	"2026-01-05T00:00:00+08:00" "$d_utc"
chk "up to the latest" "2026-03-08T10:01:00+08:00" \
	"$(TZ=UTC "$GP" rerun 2>&1 | awk '/recorded/ { print $4 }')"

printf '\n%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skipped"
[ "$fail" = 0 ]
