# The guide

Recording a task, following it across sessions, and handing the history
back to an agent.

## Reconstructing a project from a prompt repository

This is the point of the whole thing. A prompt history is not a chat log: it
spans sessions, prompts revise earlier prompts rather than only appending to
them, and the outcome of each prompt is part of the record -- as is what the
agent answered, when somebody kept it. `gitprompt replay`
renders that history as one document, with an agent-facing preamble, in
chronological order, marking where each session begins and ends:

```console
$ gitprompt replay                    # markdown, to stdout
$ gitprompt replay --format=json      # machine-readable
$ gitprompt replay --format=txt       # plain text
$ gitprompt replay --layout=tree/     # one file per prompt, plus tree/REPLAY.md
$ gitprompt replay --list-sessions
```

The order is the order the prompts were written in, and the session boundaries
are explicit, so an agent reading the document knows where context was reset.
That is the property the storage format exists to preserve.

### A task that spanned several sessions

Opening a session over one that is already open is a switch, and the session
being left is closed at the instant the new one opens -- so the two meet exactly,
and the one left behind records the stretch it really had rather than a span with
the interruption silently inside it. The recorder is told which session it left,
because otherwise there is no way to know that the next prompts are not going
into the conversation they were meant for:

```console
$ gitprompt session start -t "Plan the tokenizer"
session s_1790514149_qr22tt started
$ gitprompt session start -t "Check the CI logs"
warning: pausing session s_1790514149_qr22tt; `gitprompt session use s_1790514149_qr22tt` records into it again
session s_1790514149_4e29cj started
$ gitprompt session use s_1790514149_qr22tt
session s_1790514149_qr22tt had ended at 2026-09-27T21:02:29+08:00
(recording into it again opens a new stretch)
now recording into s_1790514149_qr22tt
```

`session use` refuses an id that names no session file, rather than pointing the
recorder at a boundary that exists nowhere but in the pointer.

`replay` groups by session, because a conversation is one context and
interleaving two of them would read as one that never happened. Its `--flat`
form answers the other question -- what was worked on, in what order:

```console
$ gitprompt replay --flat                  # markdown, each prompt naming its session
$ gitprompt replay --flat --format=txt
$ gitprompt replay --flat --format=json
```

The order there is the sequence number rather than the timestamp, which is what
makes a conversation imported after a later one come out in the order it was
filed rather than the order its dates happen to run.

### Handing the history back to an agent

`attach` writes the history where an agent reads it, in the mode where reading
is all it does. `rerun` is the other half: the same history given to an agent
that is allowed to act on it, one prompt at a time.

What makes it more than a loop over the prompt files is the conversations. Each
session is mapped to one agent conversation, and the prompts are handed over in
written order — so a task that moved between sessions and back is replayed the
same way, the prompt that returns resuming the conversation that session began:

```console
$ gitprompt rerun
Which agent should replay this history?
  1. claude   claude -p
  2. codex    -- cannot resume a session, so a replay into it would not be one
Pick one [1]:
rerun: 3 of 3 prompt(s), agent claude, permission mode acceptEdits
       2 conversation(s): one per session, and a session
         returned to is resumed rather than begun again,
         so the replay crosses them as the history did
       recorded 2026-09-26T00:31:02+08:00 .. 2026-09-26T01:14:47+08:00
       replayed in seq order, which is the order they were
         written -- the one thing a clock cannot say for them
       the agent works in /home/you/project and may change it

Start the replay now? [y/N] y

run    1  p_crmwszhq  s_1790510692_39lroz      start
run    2  p_gy32kr9c  s_1790510692_2fb6jq      start
run    3  p_xbr72wvy  s_1790510692_39lroz      resume

rerun: 3 prompt(s) across 2 conversation(s)
```

The conversation ids are derived from the session ids rather than handed out at
random, so the same history replays into the same conversations — the same ones
on the machine that pushed and on the machine that cloned — and a run that
stopped half way can be started again from where it stopped with
`--from <prompt id>`. `--salt` asks for a fresh set instead.

A prompt is arbitrary text, so it is never put on a command line: it is written
to a file in the store and handed to the agent on standard input, which is what
removes the question of what a quote or a percent sign in someone's prompt would
have done to the shell.

Nothing runs unless it is asked for. A rerun starts processes that edit the work
tree, so at a terminal it asks which agent to replay into and then asks to
confirm before it begins — the agent is a fact about the machine that pulled the
history, and a clone carries no answer to it. With no terminal to ask, the plan
above is printed and nothing more happens until `--yes`:

```console
$ gitprompt rerun --agent=claude             # skips the menu, still confirms
$ gitprompt rerun --yes                      # runs it, claude's default mode
$ gitprompt rerun --yes --permission-mode=bypassPermissions
$ gitprompt rerun --yes --model opus --from p_xbr72wvy
$ gitprompt rerun --only-session s_1790510692_39lroz
```

The prompts are handed over **one at a time** — each is a separate agent
invocation, and the next starts only when the one before it has exited — in the
order the sequence numbers were handed out when they were recorded, which is the
order a clone months later can still reconstruct.

**It reconstructs the prompts, not the project.** The prompts are stored exactly
and are handed over verbatim; an agent doing the work a second time may do it
differently, and nothing here can promise otherwise. Answers are kept only when
somebody kept them — see the next section — so a replay carries the ones the
history already has and can add the new ones to it. The project itself is
restored exactly by checking out the commit, which is what `checkout` is for —
the prompts are the *how it was made*, the commits are the *what was made*, and
only the second is byte-exact.

`codex` is refused rather than half-supported. Its conversations cannot be given
an id to resume by, so an interrupted history could not be played back as the
conversations it was — and running each session as a string of unrelated ones
would not be a replay of anything.

### Keeping what the agent answered

A prompt on its own is half of what happened. `response` records the other half,
against the prompt it belongs to:

```console
$ gitprompt prompt -m "Build a login page with email and password fields."
p_ibd076r6 prompts/0001-build-a-login-page-with-email-and-passwo.md
$ gitprompt response -m "Created login.html. Validation misses empty input."
r_vql9qir6 prompts/responses/p_ibd076r6.md
$ gitprompt response -m "Added the check; empty input is refused now." p_ibd076r6
error: response: p_ibd076r6 is already answered (use --force to replace it)
```

The answer goes in a file of its own, `prompts/responses/<prompt id>.md`, rather
than inside the prompt file. A prompt file is text somebody may have written by
hand, so there is no delimiter in it that could be trusted to mean "the answer
starts here" — and the two arrive at different times, since the answer exists
only once an agent has replied, and may never. Being an ordinary file in the
tree is what makes it travel: a clone, a push and a `checkout` carry the answers
with the prompts, and `git --git-dir=.gitprompt ls-tree` shows both.

With no prompt id the newest prompt is the one being answered, and the text can
also come from a file or down a pipe, which is how it normally arrives:

```console
$ claude -p < notes.md | gitprompt response p_ibd076r6
```

Nothing else has to change for the answer to be part of the history:

```console
$ gitprompt replay --flat          # the document gains **Response.** blocks
$ gitprompt log-prompt             # the listing gains a response line
$ gitprompt attach                 # the agent's context file carries them
$ gitprompt replay --flat --format=json   # "response" is an object, or null
```

`rerun --record` is the same thing without a second step: it runs the agent with
its output coming back rather than going only to the terminal, prints it through
unchanged, and records it against the prompt it was answering. A run that
finishes records; a run that fails or is interrupted records nothing for the
prompt it was on, so an answer is never half a one. Recording replaces an answer
that is already there, because a rerun is what is being kept now.

The run path was verified by hand against the real CLI, and the suite runs a
stub agent for it: the plan `rerun` would execute, the conversations it maps
sessions to, and what `--record` keeps. No runner has a real agent installed,
and one that did would not answer the same twice.

### Recording a conversation that happened earlier

A prompt recorded today is dated today, which is right for one being typed now
and wrong for one being written down afterwards — and when a run is imported
after the fact, the dates are most of what tells one run from another. So they
can be given:

```console
$ gitprompt session start -t "Plan the tokenizer" --date='2026-03-01T09:00:00+08:00'
$ gitprompt prompt --date='2026-03-01T09:12:40+08:00' -m "Write a tokenizer first."
$ gitprompt session end --date='2026-03-01T09:30:00+08:00'
```

`--date` takes what git takes in `GIT_AUTHOR_DATE` — ISO 8601 with or without
an offset, a bare day, or an `<epoch> <offset>` pair — and that variable is
honoured when no `--date` is given, so a script that sets it for git sets it
here too. An explicit `--date` wins over it. A date given with an offset is kept
in that offset; one given without is read as the machine's local time and
written back out that way, so the document `replay` produces shows the wall
clock the prompt was written at. A date that cannot be read is refused rather
than silently replaced with now.

Sessions are ordered by when they began, so a run recorded out of order still
comes back in the order it happened.
