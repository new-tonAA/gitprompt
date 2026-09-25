"""gitprompt — a distributed version control system for prompts.

The object model, refs, index and wire semantics intentionally mirror git's,
because the goal is a tool you can drive exactly like git, except that the
versioned artifact is the prompt history of a project rather than its source.
"""

__version__ = "0.1.0"

GP_DIR = ".gitprompt"
GP_VERSION = 1
