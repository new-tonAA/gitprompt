# The commands

`gitprompt help` lists them all; `gitprompt help <command>` describes one. The
names are git's, and the behaviour is meant to match. A gitprompt built from
this tree lists 64; what git has and this does not is in
[What is not implemented](limitations.md).

- **start** — `init`, `clone`, `config`
- **record prompts** — `session`, `prompt`, `capture`, `response`, `outcome`,
  `add`, `rm`, `mv`, `clean`, `commit`
- **reconstruct** — `replay`, `timeline`, `log-prompt`, `attach`, `rerun`
- **examine** — `status`, `log`, `show`, `diff`, `reflog`, `blame`, `grep`,
  `bisect`
- **branch and history** — `branch`, `checkout`, `switch`, `merge`,
  `cherry-pick`, `rebase`, `revert`, `stash`, `tag`, `reset`, `describe`
- **collaborate** — `remote`, `push`, `fetch`, `pull`, `serve`
- **plumbing** — `hash-object`, `cat-file`, `ls-tree`, `write-tree`,
  `commit-tree`, `rev-parse`, `rev-list`, `merge-base`, `update-ref`,
  `symbolic-ref`, `for-each-ref`, `ls-files`, `count-objects`,
  `verify-objects`, `check-ref-format`
- **maintenance** — `gc`, `repack`, `prune`, `fsck`, `stats`, `help`, `version`

The prompt commands are the ones git has no counterpart for: `session` and
`prompt` are how a prompt is recorded, `replay` and `timeline` are how a history
is read back, and the rest of the list behaves as git's command of the same name
does. [The guide](guide.md) is the walkthrough.
