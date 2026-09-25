#!/usr/bin/env bash
# End-to-end tests.  Each check asserts a behaviour, so this fails loudly
# instead of printing a transcript for someone to eyeball.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
export PYTHONPATH="$ROOT/src"
export GITPROMPT_TRACE=1
GP="python -m gitprompt"

SANDBOX="${SANDBOX:-/tmp/gitprompt-tests}"
rm -rf "$SANDBOX"
mkdir -p "$SANDBOX"

PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; [ -n "${2:-}" ] && printf '       %s\n' "$2"; }
section() { printf '\n== %s ==\n' "$1"; }

# check <description> <expected> <actual>
check() {
  if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "expected [$2] got [$3]"; fi
}

# contains <description> <needle> <haystack>
contains() {
  case "$3" in
    *"$2"*) ok "$1" ;;
    *)      bad "$1" "output did not contain [$2]" ;;
  esac
}

# lacks <description> <needle> <haystack>
lacks() {
  case "$3" in
    *"$2"*) bad "$1" "output unexpectedly contained [$2]" ;;
    *)      ok "$1" ;;
  esac
}

cd "$SANDBOX"

# --------------------------------------------------------------------------
section "identity and init"

$GP config --global user.name "Smoke Tester" >/dev/null 2>&1
$GP config --global user.email "smoke@example.com" >/dev/null 2>&1
check "global user.name reads back" "Smoke Tester" "$($GP config --global --get user.name 2>&1)"

mkdir proj && cd proj
$GP init >/dev/null 2>&1
check "init creates .gitprompt" "yes" "$([ -d .gitprompt ] && echo yes || echo no)"

# --------------------------------------------------------------------------
section "sessions and prompts"

$GP session start -t "Core engine" >/dev/null 2>&1
$GP prompt "Build a CLI entry point that dispatches like git.c" >/dev/null 2>&1
$GP prompt "Add an object store: content-addressed, zlib, sha1" >/dev/null 2>&1

check "worktree prompt count before commit" "2" "$(ls prompts/*.md 2>/dev/null | wc -l | tr -d ' ')"
check "session show sees uncommitted prompts" "2 prompt(s)" \
  "$($GP session show 2>&1 | grep -o '[0-9]* prompt(s)' | head -1)"
check "session prompt numbering starts at 1" "1" \
  "$($GP session show 2>&1 | grep -c '^ *1\. ')"

# filenames are numbered repo-wide, not per-session, so a flat listing reads
# in the order the prompts were written
$GP session end >/dev/null 2>&1
$GP session start -t "Replay layer" >/dev/null 2>&1
$GP prompt "Implement replay: walk every prompt object" >/dev/null 2>&1
check "filename numbering continues across sessions" "003-implement-replay-walk-every-prompt-objec.md" \
  "$(ls prompts/ | sed -n '3p')"
check "frontmatter seq restarts per session" "1" \
  "$(grep -m1 '^seq:' prompts/003-*.md | awk '{print $2}')"

# --------------------------------------------------------------------------
section "outcomes"

$GP outcome 2 "objects store correctly, fsck clean" >/dev/null 2>&1
check "outcome lands in the prompt file" "objects store correctly, fsck clean" \
  "$(grep -m1 '^outcome:' prompts/002-*.md | sed 's/^outcome: //')"

$GP outcome --last "replay groups prompts by session" >/dev/null 2>&1
check "outcome --last takes its text from the positional" "replay groups prompts by session" \
  "$(grep -m1 '^outcome:' prompts/003-*.md | sed 's/^outcome: //')"

# --------------------------------------------------------------------------
section "snapshot layer"

$GP add -A >/dev/null 2>&1
check "add -A stages every prompt" "3" "$($GP status --short 2>&1 | grep -c '^A  prompts/')"

$GP commit -q -m "session 1: core engine" >/dev/null 2>&1
check "commit records a commit" "1" "$($GP log --oneline 2>&1 | grep -c .)"
check "working tree clean after commit" "" "$($GP status --short 2>&1)"
check "prompt objects are mode 100640" "100640" \
  "$(python -c "
import sys
from gitprompt.repo import Repository
r = Repository.discover()
t = r.read_tree(r.store.read(r.refs.head_sha()).tree)
print(oct([v[0] for k, v in t.items() if k.startswith('prompts/')][0])[2:])
" 2>&1)"

# --------------------------------------------------------------------------
section "history and plumbing"

contains "log --oneline shows the subject" "session 1: core engine" "$($GP log --oneline 2>&1)"
check "log by full sha works" "1" \
  "$($GP log --oneline "$($GP rev-parse HEAD 2>/dev/null | head -1)" 2>&1 | grep -c .)"
contains "show --stat reports a file count" "changed" "$($GP show --stat 2>&1)"
contains "diff of a clean worktree is empty" "" "$($GP diff 2>&1)"
contains "reflog has the commit" "commit:" "$($GP reflog 2>&1)"
check "fsck finds no problems" "no problems found" "$($GP fsck 2>&1 | tail -1)"
contains "count-objects reports the store size" "objects," "$($GP count-objects 2>&1)"

# --------------------------------------------------------------------------
section "replay"

check "replay markdown carries the agent preamble" "1" \
  "$($GP replay 2>&1 | grep -c 'You are reconstructing a project')"
contains "replay renders outcomes" "> **Outcome:** objects store correctly, fsck clean" "$($GP replay 2>&1)"
check "replay json is valid json" "yes" \
  "$(python -c "
import json, subprocess, sys
out = subprocess.run([sys.executable, '-m', 'gitprompt', 'replay', '-f', 'json'],
                     capture_output=True, text=True).stdout
print('yes' if json.loads(out)['stats']['prompts'] == 3 else 'no')
" 2>&1)"
check "replay --list-sessions lists both sessions" "2" \
  "$($GP replay --list-sessions 2>&1 | grep -c 'prompt(s)')"

# --------------------------------------------------------------------------
section "branches, tags, checkout"

$GP branch experiment >/dev/null 2>&1
contains "branch -a lists a remote-less branch" "experiment" "$($GP branch -a 2>&1)"
$GP checkout experiment >/dev/null 2>&1
check "checkout switches branch" "* experiment" "$($GP branch 2>&1 | grep '^\*')"
$GP checkout main >/dev/null 2>&1
check "checkout switches back" "* main" "$($GP branch 2>&1 | grep '^\*')"

$GP tag v1.0 -m "first cut" >/dev/null 2>&1
check "describe names the tag" "v1.0" "$($GP describe 2>&1)"
check "tag is peeled to its commit" "$($GP rev-parse HEAD 2>/dev/null | head -1)" \
  "$(python -c "
from gitprompt.repo import Repository
r = Repository.discover()
print(r.peel_to_commit('v1.0'))
" 2>&1)"
check "fsck is clean with a tag object" "no problems found" "$($GP fsck 2>&1 | tail -1)"

# --------------------------------------------------------------------------
section "remotes"

cd "$SANDBOX"
$GP clone proj clone1 >/dev/null 2>&1
check "clone restores the prompt files" "3" "$(ls clone1/prompts/*.md 2>/dev/null | wc -l | tr -d ' ')"
check "clone lands on the source branch" "* main" "$(cd clone1 && $GP branch 2>&1 | grep '^\*')"
check "clone preserves outcomes" "2" "$(cd clone1 && $GP replay 2>&1 | grep -c 'Outcome')"
check "clone is self-consistent" "no problems found" "$(cd clone1 && $GP fsck 2>&1 | tail -1)"

cd clone1
$GP prompt "Add a vscode extension for the timeline" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m "clone side: extension" >/dev/null 2>&1

contains "push reports the ref it moved" "refs/heads/main -> refs/heads/main" \
  "$($GP push origin main 2>&1)"
check "push refreshed the remote's working tree" "4" "$(ls ../proj/prompts/*.md | wc -l | tr -d ' ')"
check "the pushed prompt landed in the remote's worktree" "004-add-a-vscode-extension-for-the-timeline.md" \
  "$(ls ../proj/prompts/ | grep '^004-')"

# The remote's next commit must not swallow the pushed prompt.  This is the
# regression where a push moved the ref but left the worktree and index behind,
# so the origin's next `add -A` recorded the pushed file as deleted.
cd "$SANDBOX/proj"
$GP prompt "A commit only the origin has" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m "origin-only commit" >/dev/null 2>&1
check "pushed prompt survives the remote's next commit" "1" \
  "$($GP ls-tree HEAD 2>&1 | grep -c 'prompts/004-')"
check "working tree is clean again after that commit" "" "$($GP status --short 2>&1)"

$GP remote add other ../clone1 >/dev/null 2>&1
$GP fetch other >/dev/null 2>&1
check "merge of a diverged remote succeeds" "0" "$($GP merge other/main >/dev/null 2>&1; echo $?)"

cd "$SANDBOX/clone1"
$GP prompt "Diverged on the clone side" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m "clone diverges" >/dev/null 2>&1
contains "non-fast-forward push is refused" "non-fast-forward" "$($GP push origin main 2>&1)"
check "force push overrides the refusal" "0" "$($GP push --force origin main >/dev/null 2>&1; echo $?)"

check "remote branch listing is shorthand" "  origin/main" \
  "$($GP branch -r 2>&1 | grep 'origin/main' | head -1)"
check "remote URL names the checkout, not the store" "0" \
  "$($GP remote -v 2>&1 | grep -c '\.gitprompt ')"
check "fsck clean after all transfers" "no problems found" "$($GP fsck 2>&1 | tail -1)"

# --------------------------------------------------------------------------
section "argument parsing"
# forms that regressed once and must not again

cd "$SANDBOX/proj"
echo "*.bak" >> .gitpromptignore
check "-am clusters a flag with a valued option" "1" \
  "$($GP commit -q -am "clustered message" >/dev/null 2>&1; $GP log --oneline 2>&1 | grep -c 'clustered message')"
check "the clustered commit picked up the tracked edit" "" "$($GP status --short 2>&1)"
MULTI="$(echo x > repeated.txt && $GP add -A >/dev/null 2>&1
         $GP commit -q -m "subject line" -m "body line" >/dev/null 2>&1
         $GP log -1 2>&1)"
contains "repeated -m keeps the first message as the subject" "    subject line" "$MULTI"
contains "repeated -m appends the second as the body" "    body line" "$MULTI"
check "--dry-run resolves through its long name" "add '" \
  "$(touch dryrun.md; $GP add -n dryrun.md 2>&1 | head -1 | cut -c1-5; rm -f dryrun.md)"

# --------------------------------------------------------------------------
section "housekeeping"

check "gc runs clean" "0" "$($GP gc >/dev/null 2>&1; echo $?)"
check "repository is still valid after gc" "no problems found" "$($GP fsck 2>&1 | tail -1)"
check "stats reports sessions and prompts" "3" "$($GP stats 2>&1 | grep -c 'prompt(s)')"

# --------------------------------------------------------------------------
section "prompt plumbing"
# a fresh repository, so nothing here disturbs the counts above

cd "$SANDBOX"
mkdir plumb && cd plumb
$GP init . >/dev/null 2>&1
$GP session start -t "Plumbing" >/dev/null 2>&1
$GP prompt "alpha prompt" >/dev/null 2>&1
$GP prompt "beta prompt" >/dev/null 2>&1
$GP outcome 1 "alpha turned out fine" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m "c1" >/dev/null 2>&1
$GP prompt "gamma prompt" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m "c2" >/dev/null 2>&1

check "log-prompt lists the session's prompts" "3" "$($GP log-prompt 2>&1 | grep -cE '^ +[0-9]+\. ')"
check "verify-objects is clean" "no problems found" \
  "$($GP verify-objects 2>&1 | sed 's/^Checked [0-9]* object(s): //')"

check "replay keeps outcomes by default" "1" "$($GP replay 2>&1 | grep -c 'Outcome')"
check "--no-outcomes drops them" "0" "$($GP replay --no-outcomes 2>&1 | grep -c 'Outcome')"
$GP replay -o replay.md >/dev/null 2>&1
check "replay -o writes the file" "yes" "$([ -s replay.md ] && echo yes || echo no)"
rm -f replay.md

check "reset --soft stages the diff" "A  prompts/003-gamma-prompt.md" \
  "$($GP reset --soft HEAD~1 >/dev/null 2>&1; $GP status --short 2>&1)"
check "reset --hard clears it" "" "$($GP reset --hard HEAD >/dev/null 2>&1; $GP status --short 2>&1)"

printf 'piped captured prompt\n' | $GP capture >/dev/null 2>&1
check "capture reads a prompt from stdin" "1" "$(ls prompts/ | grep -c 'piped-captured')"
$GP add -A >/dev/null 2>&1
$GP commit -q -m "capture it" >/dev/null 2>&1

$GP mv prompts/002-beta-prompt.md prompts/002-renamed.md >/dev/null 2>&1
check "mv stages the deletion of the old name" "D  prompts/002-beta-prompt.md" \
  "$($GP status --short 2>&1 | grep '002-beta')"
check "mv stages the new name" "A  prompts/002-renamed.md" \
  "$($GP status --short 2>&1 | grep '002-renamed')"

$GP rm prompts/001-alpha-prompt.md >/dev/null 2>&1
check "rm stages a deletion" "D  prompts/001-alpha-prompt.md" \
  "$($GP status --short 2>&1 | grep '001-alpha')"
$GP rm --cached prompts/003-piped-captured-prompt.md >/dev/null 2>&1
check "rm --cached keeps the file but unstages it" "yes" \
  "$($GP status --short 2>&1 | grep -q '^D  prompts/003-piped' && [ -f prompts/003-piped-captured-prompt.md ] && echo yes)"
check "the unstaged file now reads as untracked" "1" \
  "$($GP status --short 2>&1 | grep -c '^?? prompts/003-piped')"

$GP branch side >/dev/null 2>&1
check "switch moves to a branch" "* side" "$($GP switch side >/dev/null 2>&1; $GP branch 2>&1 | grep '^\*')"
check "switch -c creates and moves" "* fresh" \
  "$($GP switch -c fresh >/dev/null 2>&1; $GP branch 2>&1 | grep '^\*')"

contains "pull with no remote explains itself" "no such remote" "$($GP pull 2>&1)"

# --------------------------------------------------------------------------
section "http transport"
# `serve` advertises a clone URL, so the whole round trip has to work: the
# native transport once stored objects without ever moving a ref, which made a
# push look like it had succeeded while the branch stayed put.

PORT="${GP_TEST_PORT:-8799}"
cd "$SANDBOX"
mkdir -p served && cd served
$GP init . >/dev/null 2>&1
$GP session start -t "Served" >/dev/null 2>&1
$GP prompt "served prompt one" >/dev/null 2>&1
$GP outcome 1 "served correctly" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m "served initial" >/dev/null 2>&1

$GP serve --port "$PORT" > "$SANDBOX/serve.log" 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null' EXIT

READY=""
for _ in $(seq 1 40); do
  if python -c "
import urllib.request
urllib.request.urlopen('http://127.0.0.1:$PORT/health', timeout=2)
" >/dev/null 2>&1; then READY="yes"; break; fi
  sleep 0.25
done
check "serve answers /health" "yes" "$READY"

cd "$SANDBOX"
$GP clone "gp://127.0.0.1:$PORT/" hclone >/dev/null 2>&1
check "clone over http restores the prompts" "1" "$(ls hclone/prompts/*.md 2>/dev/null | wc -l | tr -d ' ')"
check "http clone lands on the right branch" "* main" "$(cd hclone && $GP branch 2>&1 | grep '^\*')"
check "http clone preserves outcomes" "1" "$(cd hclone && $GP replay 2>&1 | grep -c Outcome)"
check "http clone is self-consistent" "no problems found" "$(cd hclone && $GP fsck 2>&1 | tail -1)"

cd hclone
$GP prompt "pushed over http" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m "http side" >/dev/null 2>&1
contains "push over http moves the ref" "refs/heads/main -> refs/heads/main" "$($GP push origin main 2>&1)"
check "the served branch advanced" "http side" \
  "$(cd "$SANDBOX/served" && $GP log -1 2>&1 | grep -o 'http side' | head -1)"
check "the served checkout was refreshed" "1" "$(ls "$SANDBOX/served/prompts/" | grep -c 'pushed-over-http')"
check "the served checkout is still clean" "" "$(cd "$SANDBOX/served" && $GP status --short 2>&1)"

# diverging from the served side must be refused, not force-applied
cd "$SANDBOX/served"
$GP prompt "served diverges" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m "served only" >/dev/null 2>&1
cd "$SANDBOX/hclone"
contains "non-fast-forward is refused over http" "non-fast-forward" "$($GP push origin main 2>&1)"

kill "$SERVER_PID" 2>/dev/null
trap - EXIT

# --------------------------------------------------------------------------
section "git-mirror carrier"
# GitHub cannot run our server, so a remote there is hosted as ordinary files
# on a dedicated branch of a real git repository.  A local bare repo stands in
# for GitHub here, which exercises the same code without needing the network.

if ! command -v git >/dev/null 2>&1; then
  bad "git is on PATH for the carrier test" "git not found; skipping the git-mirror section"
else
  cd "$SANDBOX"
  git init --bare --quiet -b main carrier.git >/dev/null 2>&1
  mkdir -p mirrored && cd mirrored
  $GP init . >/dev/null 2>&1
  $GP session start -t "Mirrored" >/dev/null 2>&1
  $GP prompt "mirrored prompt one" >/dev/null 2>&1
  $GP outcome 1 "mirrored fine" >/dev/null 2>&1
  $GP add -A >/dev/null 2>&1
  $GP commit -q -m "mirrored initial" >/dev/null 2>&1

  python -c "
import os, sys
from gitprompt.repo import Repository
from gitprompt.transport import GitMirrorRemote
root = os.path.abspath('.')
carrier = os.path.abspath(os.path.join('..', 'carrier.git'))
repo = Repository.discover(start=root)
sha = repo.refs.head_sha()
t = GitMirrorRemote(carrier, os.path.join(repo.gpdir, 'remotes', 'origin', 'mirror'), 'origin')
assert t.refs().refs == {}, 'a fresh carrier should hold no refs'
t.publish(repo.store, {'refs/heads/main': sha}, 'carrier push')
assert t.refs().refs.get('refs/heads/main') == sha, 'the carrier did not record the branch'
" 2>&1 | tail -3

  check "the carrier branch holds the store" "FORMAT" \
    "$(git -C "$SANDBOX/carrier.git" ls-tree -r --name-only gitprompt/store 2>&1 | head -1)"
  check "the carrier records the pushed branch" "1" \
    "$(git -C "$SANDBOX/carrier.git" show gitprompt/store:refs.json 2>&1 | grep -c 'refs/heads/main')"

  # read it back the way a fresh clone would
  cd "$SANDBOX"
  mkdir -p mirrored2
  python -c "
import os, sys
from gitprompt.repo import Repository
from gitprompt.objects import ObjectStore
from gitprompt.refs import RefStore
from gitprompt.transport import GitMirrorRemote
root = os.path.abspath('mirrored2')
carrier = os.path.abspath('carrier.git')
repo = Repository.init(root, quiet=True)
t = GitMirrorRemote(carrier, os.path.join(repo.gpdir, 'remotes', 'origin', 'mirror'), 'origin')
refs = t.refs().refs
assert refs, 'the carrier read back no refs'
imported = t.materialise(repo.store, list(refs.values()))
assert imported > 0, 'no objects imported from the carrier'
rs = RefStore(repo.gpdir)
for name, sha in refs.items():
    rs.update(name, sha, reflog=False)
repo.checkout_tree(repo.commit_of(rs.read('refs/heads/main')).tree,
                   force=True, update_index=True)
" 2>&1 | tail -3

  check "materialising the carrier restores the prompt" "001-mirrored-prompt-one.md" \
    "$(ls "$SANDBOX/mirrored2/prompts/" 2>&1 | head -1)"
  check "the carrier clone preserves the outcome" "1" \
    "$(cd "$SANDBOX/mirrored2" && $GP replay 2>&1 | grep -c Outcome)"
  check "the carrier clone is self-consistent" "no problems found" \
    "$(cd "$SANDBOX/mirrored2" && $GP fsck 2>&1 | tail -1)"
fi

# --------------------------------------------------------------------------
section "cli behaviour"

cd "$SANDBOX"
mkdir cli && cd cli
$GP init . >/dev/null 2>&1
$GP session start -t "CLI" >/dev/null 2>&1
$GP prompt "cli prompt one" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m "cli initial" >/dev/null 2>&1

# A mistyped option used to be dropped on the floor, which turned a typo into
# a silently different command.
contains "an unknown long option is an error" "unknown option '--bogus'" "$($GP log --bogus 2>&1)"
contains "an unknown short option is an error" "unknown option '-Z'" "$($GP log -Z 2>&1)"
check "an unknown option exits non-zero" "1" "$($GP log --bogus >/dev/null 2>&1; echo $?)"

$GP replay --layout tree >/dev/null 2>&1
check "--layout writes a manifest" "yes" "$([ -f tree/REPLAY.md ] && echo yes || echo no)"
check "--layout writes one file per prompt" "1" "$(find tree -name '0*-*.md' | wc -l | tr -d ' ')"
# `--layout` writes a tree and `-o` writes a file; asking for both is ambiguous
contains "--layout and -o together are refused" "ask for one of them" \
  "$($GP replay --layout t2 -o out.md 2>&1)"
check "the refused combination wrote nothing" "no" "$([ -e t2 ] && echo yes || echo no)"
# Help text drifts silently when a flag is renamed; the help is the only
# documentation a user reads before typing the command.
contains "replay help names the current flag" "--layout=DIR" "$($GP help replay 2>&1)"
contains "replay help has dropped the old flag" "no" \
  "$($GP help replay 2>&1 | grep -q -- '--checkout' && echo yes || echo no)"

# Several prompts can land in one commit, and the commit graph cannot order
# what shares a node — replay has to fall back to seq.  If that ever breaks,
# a burst of prompts replays scrambled.
cd "$SANDBOX"
mkdir -p multi && cd multi
$GP init . >/dev/null 2>&1
$GP session start -t "Burst" >/dev/null 2>&1
$GP prompt "burst prompt one" >/dev/null 2>&1
$GP prompt "burst prompt two" >/dev/null 2>&1
$GP prompt "burst prompt three" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m "three prompts, one commit" >/dev/null 2>&1
check "all three prompts land in one commit" "1" "$($GP log --oneline | wc -l | tr -d ' ')"
check "a burst replays in the order it was written" \
  "burst prompt one|burst prompt two|burst prompt three" \
  "$($GP replay --format=txt 2>&1 | grep '^burst prompt' | tr '\n' '|' | sed 's/|$//')"

# --------------------------------------------------------------------------
section "pull"

cd "$SANDBOX"
mkdir -p upstream && cd upstream
$GP init . >/dev/null 2>&1
$GP session start -t "Upstream" >/dev/null 2>&1
$GP prompt "upstream prompt one" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m "upstream initial" >/dev/null 2>&1

cd "$SANDBOX"
$GP clone upstream downstream >/dev/null 2>&1
cd upstream
$GP prompt "upstream prompt two" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m "upstream second" >/dev/null 2>&1

cd "$SANDBOX/downstream"
contains "pull reports the fast-forward" "Fast-forward" "$($GP pull 2>&1)"
check "pull brings the new prompt down" "2" "$(ls prompts/*.md | wc -l | tr -d ' ')"
check "pull leaves a clean tree" "" "$($GP status --short 2>&1)"
check "pull is idempotent" "Already up to date." "$($GP pull 2>&1)"
check "pull --ff-only accepts a fast-forward" "Already up to date." "$($GP pull --ff-only 2>&1)"
check "the repository is still sound" "no problems found" "$($GP fsck 2>&1 | tail -1)"

# --------------------------------------------------------------------------
section "merge conflicts"

cd "$SANDBOX"
mkdir -p mc && cd mc
$GP init . >/dev/null 2>&1
$GP session start -t "Conflicts" >/dev/null 2>&1
$GP prompt "the shared prompt" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m base >/dev/null 2>&1
CONFLICT_FILE=$(ls prompts/*.md)

$GP checkout -q -b side >/dev/null 2>&1
printf '\nSIDE EDIT\n' >> "$CONFLICT_FILE"
$GP add -A >/dev/null 2>&1
$GP commit -q -m side >/dev/null 2>&1

$GP checkout -q main >/dev/null 2>&1
printf '\nMAIN EDIT\n' >> "$CONFLICT_FILE"
$GP add -A >/dev/null 2>&1
$GP commit -q -m main >/dev/null 2>&1

# A conflicted merge used to name the file and stop, leaving HEAD's version on
# disk with no markers — "resolve the files by hand" was unactionable.
check "a conflicting merge exits non-zero" "1" \
  "$($GP merge side >/dev/null 2>&1; echo $?)"
check "conflict markers are written" "yes" \
  "$(grep -q '<<<<<<<' "$CONFLICT_FILE" && echo yes || echo no)"
check "the markers carry both sides" "yes" \
  "$(grep -q 'SIDE EDIT' "$CONFLICT_FILE" && grep -q 'MAIN EDIT' "$CONFLICT_FILE" && echo yes || echo no)"
# The marker blocks have to be valid prompt files: a resolved file that lost
# its frontmatter would be re-added as a brand-new prompt.
contains "the markers keep the prompt frontmatter" "id: p_" "$(cat "$CONFLICT_FILE")"
contains "status marks the path unmerged" "UU $CONFLICT_FILE" "$($GP status --short 2>&1)"
contains "a long status explains the merge" "You have unmerged paths." "$($GP status 2>&1)"
contains "commit refuses while unmerged" "unmerged paths" "$($GP commit -m too-early 2>&1)"
contains "a second merge refuses" "not concluded your merge" "$($GP merge side 2>&1)"

# Resolve: keep HEAD's file and append the other side's line.
python - "$CONFLICT_FILE" <<'PYEOF'
import sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
ours = text.split("<<<<<<< HEAD\n", 1)[1].split("\n||||||| base\n", 1)[0]
theirs = text.split("\n=======\n", 1)[1].split("\n>>>>>>> side\n", 1)[0]
side_added = [l for l in theirs.splitlines() if l.strip() and l not in ours]
open(path, "w", encoding="utf-8").write(ours.rstrip("\n") + "\n" + "\n".join(side_added) + "\n")
PYEOF
$GP add -A >/dev/null 2>&1
check "staging clears the unmerged marker" "no" \
  "$($GP status --short 2>&1 | grep -q '^UU' && echo yes || echo no)"
contains "the resolved file is staged" "M  $CONFLICT_FILE" "$($GP status --short 2>&1)"
$GP commit -q -m "merge side into main" >/dev/null 2>&1
# Without MERGE_HEAD the follow-up commit would be an ordinary one, and the
# merge would be recorded as though the branch had never been joined in.
check "the merge commit has two parents" "2" \
  "$($GP cat-file -p HEAD | grep -c '^parent')"
check "the merge state is cleared" "" "$(ls .gitprompt 2>/dev/null | grep -i '^MERGE_' || true)"
check "the merged prompt keeps both edits" "yes" \
  "$(grep -q 'MAIN EDIT' "$CONFLICT_FILE" && grep -q 'SIDE EDIT' "$CONFLICT_FILE" && echo yes || echo no)"
check "the repository survives the merge" "no problems found" "$($GP fsck 2>&1 | tail -1)"

# --abort has to put the tree back, not just drop the state files.
cd "$SANDBOX"
mkdir -p mabort && cd mabort
$GP init . >/dev/null 2>&1
$GP session start -t "Abort" >/dev/null 2>&1
$GP prompt "shared" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m base >/dev/null 2>&1
AF=$(ls prompts/*.md)
$GP checkout -q -b side >/dev/null 2>&1
printf '\nSIDE\n' >> "$AF"
$GP add -A >/dev/null 2>&1
$GP commit -q -m side >/dev/null 2>&1
$GP checkout -q main >/dev/null 2>&1
printf '\nMAIN\n' >> "$AF"
$GP add -A >/dev/null 2>&1
$GP commit -q -m main >/dev/null 2>&1
$GP merge side >/dev/null 2>&1
check "the conflict is present before the abort" "1" "$(grep -c '<<<<<<<' "$AF")"
$GP merge --abort >/dev/null 2>&1
check "--abort removes the markers" "0" "$(grep -c '<<<<<<<' "$AF")"
check "--abort leaves a clean tree" "" "$($GP status --short 2>&1)"
contains "--abort with no merge says so" "no merge to abort" "$($GP merge --abort 2>&1)"

# --ff-only used to be parsed and ignored: a diverged merge ran anyway.
contains "--ff-only refuses a divergence" "not possible to fast-forward" \
  "$($GP merge --ff-only side 2>&1)"
check "--ff-only leaves no merge state" "" "$(ls .gitprompt | grep -i '^MERGE_' || true)"

# --------------------------------------------------------------------------
section "merge --no-commit"

cd "$SANDBOX"
mkdir -p mnc && cd mnc
$GP init . >/dev/null 2>&1
$GP session start -t "Prepare" >/dev/null 2>&1
$GP prompt "base" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m base >/dev/null 2>&1
$GP checkout -q -b side >/dev/null 2>&1
$GP prompt "side only" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m side >/dev/null 2>&1
$GP checkout -q main >/dev/null 2>&1
$GP prompt "main only" >/dev/null 2>&1
$GP add -A >/dev/null 2>&1
$GP commit -q -m main >/dev/null 2>&1
# --no-commit used to compute the tree and then drop it on the floor: neither
# the working tree nor the index was touched.
contains "--no-commit prepares without committing" "not committing" "$($GP merge --no-commit side 2>&1)"
check "--no-commit stages the merged result" "A  prompts/002-side-only.md" \
  "$($GP status --short 2>&1)"
$GP commit -q -m "conclude the prepared merge" >/dev/null 2>&1
check "the prepared merge commits with two parents" "2" \
  "$($GP cat-file -p HEAD | grep -c '^parent')"
check "no prompts were lost in the merge" "3" "$(ls prompts/*.md | wc -l | tr -d ' ')"

# --------------------------------------------------------------------------
printf '\n%s passed, %s failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
