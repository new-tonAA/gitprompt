"""A small git-style option parser.

git's parser accepts options and operands in any order, lets `--` end option
processing, allows `--opt=value` and `--opt value`, and lets short flags
cluster (`-am "msg"`).  argparse cannot do all of that without fighting it, so
this module handles just those shapes.
"""

from __future__ import annotations

from .utils import GitPromptError


class Opt:
    """One option definition.  `takes_value` may be True, False, or '?' for
    an optional value (`--force` vs `--force=always`)."""

    def __init__(self, *names, takes_value=False, default=None, repeatable=False):
        self.names = list(names)
        self.takes_value = takes_value
        self.default = default
        self.repeatable = repeatable


class Args:
    def __init__(self, opts=None):
        self._opts = {}
        self._by_name = {}
        # Every spelling an option was declared with, canonicalised.  Without
        # this, `Opt("-n", "--dry-run")` would be reachable as "n" only: the
        # long name would never resolve because the key is taken from the
        # first spelling.
        self._canon_names = {}
        for opt in (opts or []):
            key = _canon(opt.names[0])
            self._opts[key] = opt
            for name in opt.names:
                self._by_name[name] = key
                self._canon_names[_canon(name)] = key
        self.values = {}
        self.positional = []

    @staticmethod
    def parse(opts, argv, stop_at_first_positional=False):
        self = Args(opts)
        i = 0
        no_more_opts = False
        while i < len(argv):
            token = argv[i]
            if no_more_opts:
                self.positional.append(token)
                i += 1
                continue
            if token == "--":
                no_more_opts = True
                i += 1
                continue
            if token.startswith("--") and len(token) > 2:
                i = self._long(token, argv, i)
            elif token.startswith("-") and token != "-" and not _looks_negative(token):
                i = self._short(token, argv, i)
            else:
                self.positional.append(token)
                i += 1
                if stop_at_first_positional:
                    self.positional.extend(argv[i:])
                    break
        return self

    # -- internals ---------------------------------------------------------

    def _long(self, token, argv, i):
        name, eq, inline = token.partition("=")
        key = self._by_name.get(name)
        if key is None:
            raise GitPromptError(f"unknown option '{name}'")
        opt = self._opts[key]
        if not opt.takes_value:
            if eq:
                raise GitPromptError(f"option '{name}' takes no value")
            self._record(key, True)
            return i + 1
        if eq:
            self._record(key, inline)
            return i + 1
        if opt.takes_value == "?":
            # consume the next token only when it is not itself an option
            if i + 1 < len(argv) and not argv[i + 1].startswith("-"):
                self._record(key, argv[i + 1])
                return i + 2
            self._record(key, True)
            return i + 1
        if i + 1 >= len(argv):
            raise GitPromptError(f"option '{name}' requires a value")
        self._record(key, argv[i + 1])
        return i + 2

    def _short(self, token, argv, i):
        cluster = token[1:]
        pos = 0
        while pos < len(cluster):
            name = "-" + cluster[pos]
            key = self._by_name.get(name)
            if key is None:
                raise GitPromptError(f"unknown option '{name}'")
            opt = self._opts[key]
            if not opt.takes_value:
                self._record(key, True)
                pos += 1
                continue
            rest = cluster[pos + 1:]
            if rest:
                self._record(key, rest[1:] if rest.startswith("=") else rest)
                return i + 1
            if opt.takes_value == "?":
                if i + 1 < len(argv) and not argv[i + 1].startswith("-"):
                    self._record(key, argv[i + 1])
                    return i + 2
                self._record(key, True)
                return i + 1
            if i + 1 >= len(argv):
                raise GitPromptError(f"option '{name}' requires a value")
            self._record(key, argv[i + 1])
            return i + 2
        return i + 1

    def _record(self, key, value):
        opt = self._opts[key]
        if key not in self.values:
            # repeatable options accumulate into a list, and the first
            # occurrence is already one value rather than an empty list.
            self.values[key] = [value] if opt.repeatable else value
        elif opt.repeatable:
            self.values[key].append(value)
        else:
            self.values[key] = value       # last one wins, like getopt

    # -- accessors ---------------------------------------------------------

    def _key(self, name: str) -> str:
        """Resolve any spelling of an option to the key its value is stored
        under, so `--dry-run`, `-n` and `dry_run` all find the same flag."""
        if name in self._by_name:
            return self._by_name[name]
        canon = _canon(name)
        return self._canon_names.get(canon, canon)

    def __contains__(self, name):
        return self._key(name) in self.values

    def get(self, name, default=None):
        """The supplied value, else the option's declared default, else the
        caller's fallback.  An option declared with `default=` is never
        present in `values`, so without this step the declaration is silently
        ignored and the caller gets `default` instead."""
        key = self._key(name)
        value = self.values.get(key)
        if value is not None:
            return value
        opt = self._opts.get(key)
        if opt is not None and opt.takes_value and opt.default is not None:
            return opt.default
        return default

    def flag(self, *names) -> bool:
        """True when any of the given spellings was supplied.  Callers pass
        both forms of an option (`flag("f", "force")`) so that either one
        works; a caller passing one name is unaffected."""
        return any(self.values.get(self._key(n)) for n in names)

    def list(self, *names):
        for name in names:
            value = self.values.get(self._key(name))
            if value is None:
                continue
            return value if isinstance(value, list) else [value]
        return []

    def arg(self, index, default=None):
        return self.positional[index] if index < len(self.positional) else default

    @property
    def nargs(self):
        return len(self.positional)


def _canon(name: str) -> str:
    return name.lstrip("-").replace("-", "_")


def _looks_negative(token: str) -> bool:
    return len(token) > 1 and token[1].isdigit()
