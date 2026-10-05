/*
 * gp.h - common declarations for gitprompt.
 *
 * gitprompt versions prompts the way git versions code: same object model,
 * same object ids, same command names and usage.  The pieces git needs that
 * gitprompt also needs -- sha1, zlib-deflated loose objects, trees, commits,
 * refs -- are reimplemented here rather than linked from git.
 */
#ifndef GP_H
#define GP_H

/*
 * MinGW's default stdio is msvcrt's, which predates C99 and knows nothing
 * of %lld or %zu.  This must be defined before any header is pulled in, so
 * it lives above the includes and not in the Makefile where a stray -include
 * could reorder things.
 */
#ifdef _WIN32
#ifndef __USE_MINGW_ANSI_STDIO
#define __USE_MINGW_ANSI_STDIO 1
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

typedef unsigned char u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t i64;

/*
 * A socket, without the header that defines one.  net.c is the only file that
 * includes <winsock2.h>, and it has to be the first thing that file includes:
 * on Windows that header and <windows.h> disagree about each other, so pulling
 * the network headers in here would make every other translation unit a
 * candidate for the argument.  SOCKET is UINT_PTR, which uintptr_t matches on
 * both the 32- and the 64-bit builds; on Unix it is a file descriptor.
 */
#ifdef _WIN32
typedef uintptr_t gp_socket;
#else
typedef int gp_socket;
#endif
#define GP_SOCKET_INVALID ((gp_socket)-1)

#define GP_VERSION "0.1.0"

/* ------------------------------------------------------------------ */
/* errors                                                              */

/*
 * Every command returns 0 on success and a non-zero exit status on
 * failure.  gp_die() reports and exits; gp_error() reports and lets the
 * caller decide.
 */
void gp_die(const char *fmt, ...);
void gp_error(const char *fmt, ...);
void gp_warn(const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* growable byte buffer                                                */

struct buf {
	u8 *b;
	size_t len, cap;
};

#define BUF_INIT { NULL, 0, 0 }

void buf_init(struct buf *b);
void buf_release(struct buf *b);
void buf_reset(struct buf *b);
void buf_grow(struct buf *b, size_t need);
void buf_add(struct buf *b, const void *data, size_t len);
void buf_addstr(struct buf *b, const char *s);
void buf_addch(struct buf *b, int c);
void buf_addf(struct buf *b, const char *fmt, ...);
void buf_insert(struct buf *b, size_t pos, const void *data, size_t len);
void buf_splice(struct buf *b, size_t pos, size_t len);   /* remove */
void buf_swap(struct buf *a, struct buf *b);

/* NUL-terminated view; the buffer keeps its own copy of the terminator */
char *buf_detach(struct buf *b, size_t *lenp);
const char *buf_cstr(struct buf *b);

/* text shape: a body folded onto one line, and a body printed in full behind
 * an indent */
void body_oneline(const char *body, struct buf *out);
void body_print_indented(const char *body, const char *indent);
/* "Name <email> 1700000000 +0800" reduced to "Name <email>" */
void format_author_line(const char *raw, struct buf *out);

/* ------------------------------------------------------------------ */
/* object ids                                                          */

#define GP_SHA1_RAWSZ 20
#define GP_SHA1_HEXSZ 40

struct oid {
	u8 raw[GP_SHA1_RAWSZ];
};

typedef struct oid oid_t;

extern const oid_t null_oid;

int oid_is_null(const oid_t *o);
int oid_equal(const oid_t *a, const oid_t *b);
int oid_compare(const oid_t *a, const oid_t *b);
void oid_hex(const oid_t *o, char out[GP_SHA1_HEXSZ + 1]);
int oid_parse(oid_t *o, const char *hex);
int oid_parse_prefix(oid_t *o, const char *hex);   /* exact length only */

struct oid_array {
	oid_t *oid;
	size_t nr, alloc;
};
#define OID_ARRAY_INIT { NULL, 0, 0 }
void oid_array_append(struct oid_array *a, const oid_t *o);
int oid_array_contains(const struct oid_array *a, const oid_t *o);
void oid_array_clear(struct oid_array *a);
void oid_array_sort(struct oid_array *a);

/*
 * A list of strings, used for path sets and pathspecs.  The paths in one have
 * been through path_normalize, so matching is a prefix comparison rather than
 * a string comparison -- see slist_matches.
 */
struct slist {
	char **v;
	size_t nr, alloc;
};
#define SLIST_INIT { NULL, 0, 0 }
void slist_push(struct slist *l, const char *s);
void slist_release(struct slist *l);
/* does `path` match any of the specs?  No specs means everything. */
int slist_matches(const struct slist *specs, const char *path);

/*
 * A compiled regular expression -- see regex.c.  `ere` picks the extended
 * syntax over the basic one, `icase` folds case.  Compiling returns -1 and
 * puts one line of English in *err (which the caller frees) when the pattern
 * uses something the engine does not implement; `_fixed` is `-F`, where every
 * byte of the pattern stands for itself.
 */
struct rx;
int rx_compile(struct rx **out, const char *pattern, int ere, int icase,
	       char **err);
int rx_compile_fixed(struct rx **out, const char *pattern, int icase, char **err);
void rx_release(struct rx *re);
/*
 * The first match at or after `from` in s[0..len): 1 and [*ms,*me) if there is
 * one, 0 if there is not, -1 if the pattern would have taken longer than the
 * engine's budget allows.  `len` is the line without its newline, so `$` is
 * "pos == len".
 */
int rx_search(const struct rx *re, const char *s, size_t len, size_t from,
	      size_t *ms, size_t *me);
/* the same over several patterns, with `-w` if asked for */
int rx_search_any(struct rx *const *res, size_t n, const char *s, size_t len,
		  int word, size_t *ms, size_t *me);

/* ------------------------------------------------------------------ */
/* sha1                                                                */

typedef struct {
	u32 state[5];
	u64 count;              /* total bytes */
	u8 buffer[64];
} gp_sha1_ctx;

void gp_sha1_init(gp_sha1_ctx *ctx);
void gp_sha1_update(gp_sha1_ctx *ctx, const void *data, size_t len);
void gp_sha1_final(gp_sha1_ctx *ctx, u8 out[GP_SHA1_RAWSZ]);
void gp_sha1(const void *data, size_t len, u8 out[GP_SHA1_RAWSZ]);

/* ------------------------------------------------------------------ */
/* object model                                                        */

/*
 * gitprompt uses git's own four types and invents none.  A prompt is an
 * ordinary blob holding a markdown file with a frontmatter block, recorded
 * at an ordinary mode, which is what keeps a gitprompt repository a plain
 * git repository: git can clone it, check it out and browse it, and the
 * metadata a replay needs rides along inside the files.
 */
enum obj_type {
	OBJ_NONE   = 0,
	OBJ_COMMIT = 1,
	OBJ_TREE   = 2,
	OBJ_BLOB   = 3,
	OBJ_TAG    = 4
};

enum obj_type obj_type_from_name(const char *name);
const char *obj_type_name(enum obj_type t);
int obj_type_valid(enum obj_type t);

/* tree modes, exactly as git writes them */
#define MODE_TREE   040000
#define MODE_BLOB   0100644
#define MODE_EXEC   0100755
#define MODE_LINK   0120000

/*
 * A gitlink: the entry a submodule leaves in the tree that holds it.  It names
 * a commit, but not one of this repository's -- the commit is in the submodule's
 * own store -- so nothing here reads it as an object.  git spells it 160000.
 */
#define MODE_GITLINK 0160000

struct tree_entry {
	u32 mode;
	oid_t oid;
	char *name;             /* no '/' and no NUL; owned */
};

struct tree {
	struct tree_entry *e;
	size_t nr, alloc;
};
#define TREE_INIT { NULL, 0, 0 }
void tree_release(struct tree *t);
void tree_append(struct tree *t, u32 mode, const oid_t *oid, const char *name);
void tree_parse(struct tree *t, const void *data, size_t len);
void tree_format(const struct tree *t, struct buf *out);
int  tree_lookup(const struct tree *t, const char *name, u32 *mode, oid_t *oid,
		 size_t *pos);
void tree_sort_for_write(struct tree *t);

struct commit {
	oid_t tree;
	struct oid_array parents;
	char *author;           /* full ident line, with timestamp */
	char *committer;
	char *message;          /* owned */
	char *session;          /* gitprompt: session id, or NULL */
	char **prompts;         /* gitprompt: prompt ids this commit carries */
	size_t nr_prompts;
};
#define COMMIT_INIT { { {0} }, OID_ARRAY_INIT, NULL, NULL, NULL, NULL, NULL, 0 }
void commit_release(struct commit *c);
void commit_parse(struct commit *c, const void *data, size_t len);
void commit_format(const struct commit *c, struct buf *out);

/* ------------------------------------------------------------------ */
/* prompt and session objects                                          */

/*
 * What the agent answered a prompt, stored as its own file at
 * prompts/responses/<prompt id>.md.
 *
 * It is a separate object because a prompt file is text somebody may have
 * written by hand, and there is no delimiter inside it that could be trusted to
 * mean "the answer starts here".  It is also separate because of when it
 * arrives: the prompt is recorded as it is said, and the answer exists, if at
 * all, only once the agent has replied -- and may never.
 *
 * `prompt` is the id of the prompt being answered and is what ties the two
 * together; the file's name repeats it, so an answer can be found without
 * opening every file.  `session` is copied from that prompt, so an answer is
 * placed in the conversation even when the prompt it belongs to is not at hand.
 */
struct response {
	char *id;               /* r_... */
	char *prompt;           /* the prompt being answered */
	char *session;          /* s_... or NULL, copied from that prompt */
	char *timestamp;        /* ISO 8601 with offset, as written */
	i64 ts;                 /* epoch seconds */
	char *model;            /* the agent that answered, or NULL */
	char *body;             /* the answer text itself */
	char *path;             /* repo-relative path of the file form */
};
#define RESPONSE_INIT { NULL,NULL,NULL,NULL,0,NULL,NULL,NULL }
void response_release(struct response *r);
void response_to_file(const struct response *r, struct buf *out);
int response_from_file(struct response *r, const void *data, size_t len);
void response_to_json(const struct response *r, struct buf *out);
char *new_response_id(void);

struct prompt {
	char *id;               /* p_... */
	char *session;          /* s_... or NULL */
	int seq;                /* 1-based position within the session */
	char *timestamp;        /* ISO 8601 with offset, as written */
	i64 ts;                 /* epoch seconds, for ordering */
	char *author;
	char *model;            /* or NULL */
	char **tags; size_t nr_tags;
	char *outcome;          /* or NULL */
	char *parent_prompt;    /* or NULL */
	char **attachments; size_t nr_attachments;
	char *path;             /* repo-relative path of the file form */
	char *body;             /* the prompt text itself */
	/*
	 * The work tree as it stood when this prompt was recorded, named by a
	 * tree object.  It is taken before the prompt's own change, so a run of
	 * prompts is a chain of states and each one's change is the step between
	 * its snapshot and the next -- the commit's tree closing the last step.
	 * NULL when it could not be taken (an unmerged index, say), which is also
	 * what every prompt recorded before snapshots existed carries.
	 */
	char *snapshot;
	/*
	 * The answer, when one has been recorded.  Not part of the prompt
	 * file: it is folded on at load time, so that everything which reads
	 * the history sees the prompt and what came back together.
	 */
	struct response *response;
};
#define PROMPT_INIT { NULL,NULL,0,NULL,0,NULL,NULL,NULL,0,NULL,NULL,NULL,0,NULL,NULL,NULL,NULL }
void prompt_release(struct prompt *p);

/*
 * One stretch of time a session was being recorded into: when the recorder
 * opened it, and when it was closed.  `end` is NULL while that visit is still
 * the one being recorded into.
 *
 * A session that was never left has exactly one visit, and its file is written
 * exactly as it always was.  One that was switched away from and come back to
 * has one visit per stretch, which is what keeps the time in between from being
 * swallowed by a single start/end pair.
 */
struct session_visit {
	char *start;
	char *end;              /* or NULL while the visit is open */
};

struct session {
	char *id;
	char *title;
	char *started_at;       /* the first visit's start */
	i64 started_ts;
	char *ended_at;         /* the last visit's end, or NULL */
	char *author;
	char *notes;            /* or NULL */
	struct session_visit *visits;
	size_t nr_visits;
};
#define SESSION_INIT { NULL,NULL,NULL,0,NULL,NULL,NULL,NULL,0 }
void session_release(struct session *s);

/* open a visit; the previous one, if it is still open, is left alone */
void session_visit_add(struct session *s, const char *start);
/* close the visit the session is in; -1 if it is already closed */
int  session_visit_close(struct session *s, const char *end);
/* is the session in a visit right now? */
int  session_is_open(const struct session *s);

/*
 * The on-disk form, and the form the object store holds: a frontmatter
 * block fenced by ---, then the text.  One representation serves both the
 * working tree and the blob, so a checked-out file and its blob always
 * hash the same and `status` can compare them directly.
 */
void prompt_to_file(const struct prompt *p, struct buf *out);
int  prompt_from_file(struct prompt *p, const void *data, size_t len);
void session_to_file(const struct session *s, struct buf *out);
int  session_from_file(struct session *s, const void *data, size_t len);

/* rendering used by `replay --format=json` */
void prompt_to_json(const struct prompt *p, struct buf *out);
void session_to_json(const struct session *s, struct buf *out);

/* emit a quoted, escaped JSON string */
void json_quote(struct buf *out, const char *s);

/*
 * A minimal JSON reader, used for the refs.json a carrier repository holds.
 * Values are opaque; ask for the type you expect and take a default.
 */
struct jval;
struct jval *json_parse(const char *s, size_t len);
void json_free(struct jval *v);
const char *json_get_str(const struct jval *o, const char *key, const char *dflt);
i64 json_get_int(const struct jval *o, const char *key, i64 dflt);
size_t json_obj_len(const struct jval *o);
const char *json_obj_key(const struct jval *o, size_t i);
const char *json_obj_val_str(const struct jval *o, size_t i);
i64 json_obj_val_int(const struct jval *o, size_t i);

/* ------------------------------------------------------------------ */
/* object database                                                     */

struct pack;

struct odb {
	char *dir;              /* .gitprompt/objects */
	char *replace_dir;      /* .gitprompt/refs/replace, or NULL for none */
	struct buf tmp;
	struct pack *packs;     /* the object packs, loaded on demand */
	size_t nr_packs, alloc_packs;
	int packs_loaded;
};
#define ODB_INIT { NULL, NULL, BUF_INIT, NULL, 0, 0, 0 }
void odb_init(struct odb *o, const char *dir);
void odb_release(struct odb *o);

/*
 * Where the replace refs are, or NULL to read the store as it literally is.
 * A repository sets this at open; the maintenance commands clear it, because a
 * traversal that followed a replacement would mark the wrong sub-objects.
 */
void odb_set_replace_dir(struct odb *o, const char *dir);
/* follow refs/replace from *oid to the object that stands in for it */
void odb_replace(struct odb *o, oid_t *oid);

int odb_exists(struct odb *o, const oid_t *oid);
int odb_read(struct odb *o, const oid_t *oid, enum obj_type *type,
	     struct buf *out);
int odb_read_raw(struct odb *o, const oid_t *oid, enum obj_type *type,
		 struct buf *out);
int odb_write(struct odb *o, enum obj_type type, const void *data, size_t len,
	      oid_t *out);
int odb_hash(struct odb *o, enum obj_type type, const void *data, size_t len,
	     oid_t *out, int write_it);
int odb_type_of(struct odb *o, const oid_t *oid, enum obj_type *type);
size_t odb_count(struct odb *o);
size_t odb_count_packed(struct odb *o);
size_t odb_nr_packs(struct odb *o);
/* what those objects occupy on disk */
size_t odb_loose_size(struct odb *o);
size_t odb_pack_size(struct odb *o);
/* calls fn for every loose object; fn returns non-zero to stop */
void odb_foreach_loose(struct odb *o,
		       int (*fn)(const oid_t *, void *), void *data);
/* the same over loose and packed objects alike, loose first */
void odb_foreach(struct odb *o, int (*fn)(const oid_t *, void *), void *data);
/* resolve an abbreviated hex prefix to a full oid */
int odb_resolve_prefix(struct odb *o, const char *hex, oid_t *out);
/* drop what a loose object leaves behind once its bytes are in a pack */
int odb_forget_loose(struct odb *o, const oid_t *oid);
/* zlib-deflate a byte string, which is what a loose object file holds */
int gp_deflate(const void *in, size_t len, struct buf *out);

/* ------------------------------------------------------------------ */
/* packfiles (pack.c)                                                  */

/*
 * The store's packs are opened on first use, so a repository that has none
 * pays for the directory scan at most once and nothing else.
 */
void pack_load_all(struct odb *o);
void pack_release_all(struct odb *o);
int pack_has(struct odb *o, const oid_t *oid);
int pack_read_object(struct odb *o, const oid_t *oid, enum obj_type *type,
		     struct buf *out);
size_t pack_count_all(struct odb *o);
size_t pack_nr(struct odb *o);
size_t pack_disk_size(struct odb *o);
void pack_foreach(struct odb *o, int (*fn)(const oid_t *, void *), void *data);
int pack_resolve_prefix(struct odb *o, const char *hex, oid_t *out);
/*
 * Write oids as one pack and its index into `dir`, named after the pack's
 * own checksum.  out_sha, if given, receives that checksum.
 */
int pack_write(struct odb *o, const oid_t *oids, size_t nr, const char *dir,
	       u8 out_sha[GP_SHA1_RAWSZ]);
/*
 * Delete packs whose every object is in the sorted `seen`, which a pack just
 * written already holds.  `keep_path` is never dropped even if it qualifies.
 * Returns how many were (or, with dry_run, would be) removed.
 */
size_t pack_drop_redundant(struct odb *o, const oid_t *seen, size_t nr_seen,
			   const char *keep_path, int dry_run);

/* ------------------------------------------------------------------ */
/* index (git's index v2, byte for byte)                               */

/*
 * The two index flags that survive a round trip through the file.  Skip-worktree
 * is the one sparse checkout is built on: it says the path is tracked but is not
 * meant to be in this work tree, so nothing is expected of it there.
 */
#define IDX_FLAG_ASSUME_VALID  0x8000
#define IDX_FLAG_SKIP_WORKTREE 0x4000

/*
 * `stage` is git's merge stage: 0 for an ordinary entry, 1 for the merge
 * base, 2 for ours and 3 for theirs.  A path is "unmerged" when the index
 * holds any entry for it above stage 0, and then it has no stage-0 entry.
 */
struct index_entry {
	u32 ctime_sec, ctime_nsec;
	u32 mtime_sec, mtime_nsec;
	u32 dev, ino, mode, uid, gid, size;
	oid_t oid;
	char *path;
	u16 flags;
	u16 stage;
};

struct index_state {
	struct index_entry *e;
	size_t nr, alloc;
};
#define INDEX_INIT { NULL, 0, 0 }
void index_release(struct index_state *istate);
void index_read(struct index_state *istate, const char *path);
void index_write(const struct index_state *istate, const char *path);
/*
 * The entry for a path: stage 0 if it has one, else the first entry the path
 * has -- ours for a conflicted path, since merge records stage 2 first.  The
 * callers asking "is this path tracked" get the same answer either way.
 */
struct index_entry *index_get(const struct index_state *istate,
			      const char *path);
/* one exact stage, or NULL */
struct index_entry *index_get_stage(const struct index_state *istate,
				    const char *path, unsigned stage);
/*
 * Add an entry.  A stage-0 entry replaces every entry the path had, stages
 * included -- staging a path is how a conflict is declared resolved -- while
 * an entry with a stage takes a slot of its own beside the others.
 */
void index_add(struct index_state *istate, const struct index_entry *e);
/* drop every entry for the path, all stages of it */
void index_remove(struct index_state *istate, const char *path);
void index_clear(struct index_state *istate);
void index_fill_stat(struct index_entry *e, const char *fullpath);

int index_has_unmerged(const struct index_state *istate);
/*
 * The paths with entries above stage 0, each once, sorted.  A NULL-terminated
 * array, freed with index_paths_free.
 */
char **index_unmerged_paths(const struct index_state *istate, size_t *nr);
void index_paths_free(char **v);

/* ------------------------------------------------------------------ */
/* refs                                                                */

struct ref_store {
	char *dir;              /* .gitprompt, where refs/ and packed-refs are */
	char *head_dir;         /* where HEAD is; the worktree's own when linked */
	char *ident;            /* cached identity, for reflog lines */
};
#define REF_STORE_INIT { NULL, NULL, NULL }
void refs_init(struct ref_store *r, const char *gpdir);
void refs_release(struct ref_store *r);
/*
 * HEAD is the one thing under the directory that belongs to the worktree and
 * not to the repository: every linked worktree has its own, so the store is
 * told where to find it.  Left unset it is the directory itself.
 */
void refs_set_head_dir(struct ref_store *r, const char *dir);

int refs_read(struct ref_store *r, const char *name, oid_t *out);
int refs_write(struct ref_store *r, const char *name, const oid_t *oid);
int refs_delete(struct ref_store *r, const char *name);
int refs_exists(struct ref_store *r, const char *name);
void refs_reflog(struct ref_store *r, const char *name, const oid_t *old,
		 const oid_t *new, const char *msg);
/*
 * One line of a ref's reflog, read back: the id it moved to, and what the
 * entry says.  `refs_reflog_read` returns them oldest first, -1 when the ref
 * has no reflog at all, and the caller frees the array with
 * `reflog_entries_free`.  `refs_reflog_drop` removes entry `n`, counting from
 * the newest, and takes the file away when that was the last one.
 * `refs_reflog_delete` takes the whole log, for a ref whose history is being
 * forgotten rather than trimmed.
 */
struct reflog_entry {
	oid_t oid;
	char *msg;              /* owned */
};
int refs_reflog_read(struct ref_store *r, const char *name,
		     struct reflog_entry **out);
void reflog_entries_free(struct reflog_entry *e, int nr);
int refs_reflog_drop(struct ref_store *r, const char *name, int n);
int refs_reflog_delete(struct ref_store *r, const char *name);
/*
 * The entry HEAD's own reflog gets when `ref` moves.  `ref` is the ref that
 * was updated, and the entry is written only when that is the one HEAD names;
 * pass NULL to say HEAD moved without any ref being rewritten, which is what
 * a switch does.
 */
void refs_reflog_head(struct ref_store *r, const char *ref, const oid_t *old,
		      const oid_t *new, const char *msg);
char *refs_head_target(struct ref_store *r);          /* "refs/heads/main" */
int refs_head(struct ref_store *r, oid_t *out);
int refs_set_head(struct ref_store *r, const char *target);
int refs_set_head_detached(struct ref_store *r, const oid_t *oid);
int refs_read_1(struct ref_store *r, const char *name, oid_t *out, int *symref,
		char **target);
void refs_list(struct ref_store *r, const char *prefix,
	       void (*fn)(const char *name, const oid_t *oid, void *), void *data);
void refs_list_packed(struct ref_store *r, const char *prefix,
		      void (*fn)(const char *name, const oid_t *oid, void *),
		      void *data);
int refs_check_name(const char *name);

/* ------------------------------------------------------------------ */
/* repository                                                          */

struct repo {
	char *root;             /* work tree, or NULL for bare */
	char *gpdir;            /* the common .gitprompt: objects, refs, config */
	char *wt_dir;           /* the worktree's own: HEAD, index, merge state */
	struct odb odb;
	struct ref_store refs;
};
#define REPO_INIT { NULL, NULL, NULL, ODB_INIT, REF_STORE_INIT }

int repo_find(struct repo *r, const char *start);      /* search upwards */
int repo_open(struct repo *r, const char *dir);
void repo_release(struct repo *r);

const char *repo_index_path(struct repo *r);
const char *repo_head_path(struct repo *r);
/*
 * A file in the common directory -- objects, refs, config, reflogs -- which
 * is where almost everything lives.  What belongs to one worktree rather than
 * to the repository goes through repo_worktree_path instead: HEAD and the
 * index, and the state a merge, a replay, a bisect or a replace edit leaves
 * behind while it is stopped.
 */
char *repo_git_path(struct repo *r, const char *fmt, ...);
char *repo_worktree_path(struct repo *r, const char *fmt, ...);
/* ".." and "." folded out of a path, separators made "/" (repo.c) */
char *gp_clean_path(const char *in);

int repo_config_get(struct repo *r, const char *key, char **out);
/* one piece of how an agent is driven, config first and the shipped table
 * second; empty means "none", NULL means "nothing says" */
char *repo_agent_setting(struct repo *r, const char *agent, const char *field,
			 const char *builtin);
int repo_config_set(struct repo *r, const char *key, const char *value,
		    int global);
int repo_config_unset(struct repo *r, const char *key);
/* the same reader and writer, opened to a named file -- `.gitmodules` is a
 * configuration file by another name, and these are what the submodule
 * commands read and write it with (repo.c) */
int config_file_get(const char *path, const char *key, char **out);
int config_file_set(const char *path, const char *key, const char *value);
int config_file_unset(const char *path, const char *key);
/* the subsection names under `section`, in the order they appear */
struct slist *config_file_subsections(const char *path, const char *section);
/*
 * Every value a key has, in order, and not just one: `credential.helper` is a
 * list, and an empty value empties the list.  The repository's own file is
 * read after the global one, so its empty value is the last word.
 */
void config_file_get_all(const char *path, const char *key, struct slist *out);
void repo_config_get_all(struct repo *r, const char *key, struct slist *out);
void repo_config_list(struct repo *r, int global,
		      void (*fn)(const char *k, const char *v, void *),
		      void *data);
/* resolved identity */
void repo_ident(struct repo *r, struct buf *out);      /* "Name <email>" */
/* the editor to run, and running it; shared by commit, notes edit and
 * replace -e, so that one machine's choice of editor means one thing */
char *repo_editor_command(struct repo *r);
int repo_run_editor(const char *editor, const char *path);
void repo_ident_with_time(struct repo *r, struct buf *out);  /* with " 123 +0800" */

/* gitprompt configuration */
const char *repo_prompt_dir(struct repo *r);           /* default "prompts" */
/* the work tree's path, with the trailing "/." discovery leaves off */
char *repo_root_display(const struct repo *r);
const char *repo_default_branch(struct repo *r);
/* the next repository-wide file number */
long repo_next_file_seq(struct repo *r);
void repo_bump_file_seq(struct repo *r, long n);
/* current session id, or NULL */
char *repo_current_session(struct repo *r);
int repo_set_current_session(struct repo *r, const char *id);

/*
 * Fill in the prompt ids a commit carries: the prompt files in `ist` that
 * `prev_tree` does not already hold unchanged.  A commit made by a merge passes
 * its first parent's tree and the merged index, so it carries what it brought
 * in, which is the same rule the ordinary commit follows.
 */
void collect_commit_prompts(struct repo *r, const struct index_state *ist,
			    const oid_t *prev_tree, int have_prev,
			    struct commit *c);

/* ------------------------------------------------------------------ */
/* the three-way content merge                                         */

/* which side wins a hunk both sides changed; MERGE_FAVOR_NONE conflicts */
enum merge_favor {
	MERGE_FAVOR_NONE = 0,
	MERGE_FAVOR_OURS,
	MERGE_FAVOR_THEIRS,
};

/*
 * Merge two versions of a base line by line.  Returns 0 when nothing had to be
 * decided and 1 when a hunk conflicted, in which case `out` holds the text with
 * conflict markers in it -- and with a `favor` set no hunk is ever left to the
 * reader, so the result is always clean.
 */
int merge3(const struct buf *base, const struct buf *ours,
	   const struct buf *theirs, enum merge_favor favor,
	   const char *label_ours, const char *label_theirs, struct buf *out);

/*
 * How much of one text the other still has, as a percentage of the lines
 * between them: 100 for identical, 0 for nothing in common.  This is how a
 * moved file that was also edited is told from an unrelated add.
 */
int merge3_similarity(const struct buf *a, const struct buf *b);

/* ------------------------------------------------------------------ */
/* three-way merge of whole trees                                      */

struct merge_result {
	int conflicts;
	/*
	 * Conflicts that were answered from a remembered resolution and staged
	 * as the file was written.  They are not unmerged paths, so they are
	 * counted apart from `conflicts`; but a merge that met one is still a
	 * merge that stopped, and the caller has to leave it to be concluded
	 * rather than commit it out of hand.
	 */
	int rerere_staged;
	size_t files_changed;
};

/*
 * Merge two trees against their base, writing the result to the work tree and
 * filling `merged` with the index that records it -- a conflicted path as its
 * unmerged stages rather than as a resolved entry.  `base` may be NULL, which
 * is how a commit with no parent is replayed: everything is an addition.
 *
 * `label` names the far side in the messages, and is the revision the merge is
 * being made from: a branch for `merge`, the commit for a replay.
 *
 * merge_trees_labeled is the same merge with the near side named too, because
 * a stash conflict is not a merge and git gives its two sides the names it
 * gives them.
 */
void merge_trees(struct repo *r, const oid_t *base, const oid_t *ours,
		 const oid_t *theirs, struct merge_result *res,
		 struct index_state *merged, enum merge_favor favor,
		 const char *label);
void merge_trees_labeled(struct repo *r, const oid_t *base, const oid_t *ours,
			 const oid_t *theirs, struct merge_result *res,
			 struct index_state *merged, enum merge_favor favor,
			 const char *ours_label, const char *label);

/*
 * What came of a conflict the merge machinery wrote out.  A conflict that has
 * been resolved here before is resolved again as it is put in the file, so the
 * caller learns which of the four happened and writes accordingly.
 */
enum rr_result {
	RR_NONE,        /* nothing was filed: not a conflict this can key on */
	RR_RECORDED,    /* written down for the next time it is met */
	RR_RESOLVED,    /* carried over from the cache, left unmerged to check */
	RR_STAGED       /* carried over from the cache, and to be staged */
};

int rerere_enabled(struct repo *r);
enum rr_result rerere_auto(struct repo *r, const char *path,
			   const struct buf *conflicted, struct buf *out);
void rerere_report(const char *path, enum rr_result res);

/* ------------------------------------------------------------------ */
/* the working tree                                                    */

int write_tree_from_index(struct repo *r, const struct index_state *istate,
			  oid_t *out);
/*
 * The index the work tree would make: every path `idx` knows, hashed again from
 * disk, with the ones no longer there dropped.  What the index holds is the set
 * of paths that count as tracked; what the files hold is the state.  Writing the
 * result with write_tree_from_index is how a snapshot of the work tree is taken.
 */
void index_from_worktree(struct repo *r, const struct index_state *idx,
			 struct index_state *out);
/* hash one work-tree file into the object store; -1 when it is not there */
int hash_worktree_blob(struct repo *r, const char *relpath, oid_t *oid, u32 *mode);
/* check out a tree into the work tree; force==0 refuses to clobber */
int checkout_tree(struct repo *r, const oid_t *tree, int force, int update_index);
int read_tree_into_index(struct repo *r, struct index_state *istate,
			 const oid_t *tree, const char *prefix);
/*
 * One blob out to the work tree at its path, directories and mode included --
 * the step a checkout is made of, on its own because putting back what a sparse
 * checkout left out is exactly one of these (tree.c, cmd_sparse.c).
 */
void checkout_path(struct repo *r, const char *relpath, u32 mode, const oid_t *oid);

/*
 * The paths whose work tree file differs from what the index holds -- what a
 * checkout would have to overwrite.  The caller frees the array.
 */
int worktree_dirty_paths(struct repo *r, char ***paths);
void path_list_free(char **paths);

/*
 * Whether a working directory would lose anything if it were taken away whole:
 * a tracked file that differs from the index, or one the index has never heard
 * of.  Removing a linked worktree and clearing a submodule both refuse on this.
 */
int worktree_is_clean(struct repo *r);

/* paths */
void path_normalize(const char *in, struct buf *out);   /* no leading ./, / */
/* a glob against a whole single name -- no slash in either, anchored at both
 * ends (ignore.c, which owns the matcher) */
int glob_match_name(const char *pattern, const char *name);
/*
 * The same matcher over a whole path from the root, which is what a
 * sparse-checkout pattern is -- one language, two readers (ignore.c, and the
 * caller relpath is relative to the root either way).
 */
int path_match_root(const char *pattern, const char *relpath);
int path_is_ignored(struct repo *r, const char *relpath);
int path_is_ignored_dir(struct repo *r, const char *relpath);
void ignore_forget(void);       /* the index was written: look at it again */
int read_file(const char *path, struct buf *out);
int write_file(const char *path, const void *data, size_t len);
int is_directory(const char *path);
int is_file(const char *path);
int mkdir_p(const char *path);
void mkdir_one(const char *path);
int remove_file(const char *path);
int remove_dir_recursive(const char *path);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrfmt(const char *fmt, ...);
/* walk every file in the work tree, skipping .gitprompt and ignored paths */
void walk_worktree(struct repo *r,
		   void (*fn)(const char *relpath, void *), void *data);
int path_in_subdir(const char *relpath);

/* ------------------------------------------------------------------ */
/* revision walking                                                    */

int resolve_rev(struct repo *r, const char *rev, oid_t *out);
int resolve_rev_tree(struct repo *r, const char *rev, oid_t *out);
/* fill out[] with commits reachable from tips, newest first */
void walk_commits(struct repo *r, const struct oid_array *tips,
		  void (*fn)(const oid_t *, const struct commit *, void *),
		  void *data);
int is_ancestor(struct repo *r, const oid_t *ancestor, const oid_t *tip);
/* every commit reachable from tip, tip included, in walk order */
void commit_ancestors(struct repo *r, const oid_t *tip, struct oid_array *out);
/*
 * The best common ancestors of two commits: reachable from both, and not an
 * ancestor of another one.  Histories that never meet have none.  out is
 * cleared first; the answer is returned and left in walk order.
 */
size_t merge_bases(struct repo *r, const oid_t *a, const oid_t *b,
		   struct oid_array *out);

/*
 * A revision argument list.  A plain revision and ^<rev> land in include and
 * exclude; A..B splits between them; A...B needs a second pair of its own,
 * because what it asks for is not expressible as one include/exclude pair.
 */
struct rev_list {
	struct oid_array include;
	struct oid_array exclude;
	struct oid_array include2;
	struct oid_array exclude2;
	int has_second;
};
int rev_list_parse(struct repo *r, int argc, char **argv, struct rev_list *out);
void rev_list_release(struct rev_list *l);
/* the commits the argument list names, newest first */
void rev_list_run(struct repo *r, const struct rev_list *l,
		  void (*fn)(const oid_t *, const struct commit *, void *),
		  void *data);

int commit_peel(struct repo *r, const oid_t *oid, enum obj_type want, oid_t *out);
void read_commit(struct repo *r, const oid_t *oid, struct commit *c);
void read_tree_obj(struct repo *r, const oid_t *oid, struct tree *t);
char *commit_message_line(const struct commit *c);
i64 commit_time(const struct commit *c);
void parse_ident(const char *raw, char **name, char **email);

/* ------------------------------------------------------------------ */
/* the prompt history                                                  */

/*
 * One prompt as it appears in the history, joined from the prompt object
 * and the commit and path it was reached through.
 */
struct prompt_ref {
	struct prompt *prompt;   /* borrowed */
	oid_t commit;
	char *commit_sha;        /* hex */
	char *path;
};
struct prompt_list {
	struct prompt_ref *e;
	size_t nr, alloc;
};
#define PROMPT_LIST_INIT { NULL, 0, 0 }
void prompt_list_release(struct prompt_list *l);

/* every prompt reachable from every ref, folded with uncommitted files,
 * ordered for replay */
void collect_prompts(struct repo *r, struct prompt_list *out);
/* as above, but restricted to one revision's tree, plus the work tree */
void collect_prompts_from_ref(struct repo *r, const char *rev,
			      struct prompt_list *out);
/* grouped by session, sessions in start order */
struct session_group {
	struct session *session;   /* NULL for unattributed prompts */
	struct prompt_list prompts;
};
struct session_groups {
	struct session_group *g;
	size_t nr, alloc;
};
void group_by_session(struct repo *r, const struct prompt_list *in,
		      struct session_groups *out);
void session_groups_release(struct session_groups *g);

char *new_prompt_id(void);
char *new_session_id(void);
char *slugify(const char *text, size_t maxlen);
i64 now_epoch(void);
void now_iso8601(struct buf *out);
void epoch_to_iso8601(i64 t, struct buf *out);
void local_tz_offset(int *sign, int *hours, int *mins);
i64 parse_timestamp(const char *s);
int date_to_iso8601(const char *s, struct buf *out);

/* ------------------------------------------------------------------ */
/* replay output                                                       */

void replay_markdown(struct repo *r, const struct session_groups *g,
		     struct buf *out, int stat_only);
void replay_json(struct repo *r, const struct session_groups *g,
		 struct buf *out);
void replay_text(struct repo *r, const struct session_groups *g,
		 struct buf *out);
void replay_layout(struct repo *r, const struct session_groups *g,
		   const char *dir);

/*
 * The same three documents, in one flat chronology: every prompt in the order
 * it was written, whatever session it was said in.  The grouped documents above
 * answer "what was said in this conversation"; these answer "what happened,
 * in what order", which is a different question once a task spans several
 * sessions that were interleaved.
 */
void replay_flat_markdown(struct repo *r, const struct prompt_list *pl,
			  const struct session_groups *sg, struct buf *out);
void replay_flat_text(struct repo *r, const struct prompt_list *pl,
		      const struct session_groups *sg, struct buf *out);
void replay_flat_json(const struct prompt_list *pl, struct buf *out);

/*
 * Every prompt in the order it was written, whatever session it was said in.
 * The order is the sequence number, not the timestamp -- see history.c for why
 * -- and the caller owns the result.
 */
struct prompt_ref *flat_order_alloc(const struct prompt_list *pl);
/* the session a prompt names, when its file is in the store */
const struct session *session_by_id(const struct session_groups *g,
				    const char *id);
/* the whole history as it stands, gathered and grouped */
void load_groups(struct repo *r, struct prompt_list *pl,
		 struct session_groups *sg);

/*
 * Everything known about one session, one prompt per line.  Shared with
 * `show`, which accepts a session id the way it accepts a revision.
 */
int session_show(struct repo *r, const char *id);

/* ------------------------------------------------------------------ */
/* transports                                                          */

/*
 * A remote is one of:
 *   /path or file:///path   local, a plain shared directory
 *   gp://host:port/path     gitprompt's own HTTP transport
 *   https://... .git        a git repository used as a carrier (GitHub)
 */
struct remote {
	char *name;
	char *url;
};
struct remote_list {
	struct remote *e;
	size_t nr, alloc;
};
#define REMOTE_LIST_INIT { NULL, 0, 0 }
void remote_list_release(struct remote_list *l);
void remote_list_load(struct repo *r, struct remote_list *out);
int remote_get_url(struct repo *r, const char *name, char **out);
int remote_add(struct repo *r, const char *name, const char *url);
int remote_remove(struct repo *r, const char *name);
int remote_set_url(struct repo *r, const char *name, const char *url);
void remote_rename(struct repo *r, const char *from, const char *to);
void remotes_of(struct repo *r, struct remote_list *out);  /* with urls */

void remote_read_refs(struct repo *r, const char *url, const char *name,
		      void (*fn)(const char *refname, const oid_t *oid,
				 void *data), void *data);
void remote_fetch_objects(struct repo *r, const char *url, const char *name,
			  const struct oid_array *wants);
void remote_push_objects(struct repo *r, const char *url, const char *name,
			 const struct oid_array *haves,
			 const struct oid_array *wants);
char *remote_tracking_ref(const char *name, const char *refname);
void remote_update_local_ref(struct repo *r, const char *name,
			     const char *refname, const oid_t *oid);

/* the second half of clone: fill an empty repository from a url, leaving the
 * remote's default branch checked out.  `submodule add` fills a submodule with
 * the same call (cmd_remote.c) */
int remote_clone_into(struct repo *r, const char *remote, const char *url);

/* just the fetching half of the above, without a branch to check out: every
 * branch of `url` comes in under `refs/remotes/<remote>/`.  `submodule update`
 * fetches a submodule that is already there with this (cmd_remote.c) */
int remote_fetch_all(struct repo *r, const char *remote, const char *url);

/* ------------------------------------------------------------------ */
/* command dispatch                                                    */

struct command {
	const char *name;
	int (*fn)(struct repo *r, int argc, char **argv);
	const char *summary;
	const char *usage;
};

/* small argument helper, shared by the commands */
struct opts {
	char **args;
	int nargs;
	int nf;                 /* number of flags seen */
	struct { const char *name; const char *value; } flags[64];
};
/*
 * `allows` is the NULL-terminated list of option names this command accepts;
 * a name ending in '=' takes a value.  Anything else in argv is refused, so
 * the list is the whole of what the command understands.
 */
void opts_init(struct opts *o, int argc, char **argv,
	       const char *const *allows);
int opts_flag(const struct opts *o, const char *name);
const char *opts_value(const struct opts *o, const char *name);
const char *opts_arg(const struct opts *o, int i);
int opts_count(const struct opts *o);

extern const struct command commands[];
extern const size_t commands_nr;

int cmd_help(struct repo *r, int argc, char **argv);
int cmd_version(struct repo *r, int argc, char **argv);

/* command implementations, grouped by file */
int cmd_init(struct repo *, int, char **);
int cmd_clone(struct repo *, int, char **);
int cmd_config(struct repo *, int, char **);
int cmd_hash_object(struct repo *, int, char **);
int cmd_cat_file(struct repo *, int, char **);
int cmd_ls_tree(struct repo *, int, char **);
int cmd_write_tree(struct repo *, int, char **);
int cmd_commit_tree(struct repo *, int, char **);
int cmd_rev_parse(struct repo *, int, char **);
int cmd_rev_list(struct repo *, int, char **);
int cmd_merge_base(struct repo *, int, char **);
int cmd_update_ref(struct repo *, int, char **);
int cmd_symbolic_ref(struct repo *, int, char **);
int cmd_count_objects(struct repo *, int, char **);
int cmd_verify_objects(struct repo *, int, char **);
int cmd_check_ref_format(struct repo *, int, char **);
int cmd_for_each_ref(struct repo *, int, char **);
int cmd_ls_files(struct repo *, int, char **);

int cmd_add(struct repo *, int, char **);
int cmd_rm(struct repo *, int, char **);
int cmd_mv(struct repo *, int, char **);
int cmd_clean(struct repo *, int, char **);
int cmd_status(struct repo *, int, char **);
int cmd_commit(struct repo *, int, char **);
int cmd_log(struct repo *, int, char **);
int cmd_shortlog(struct repo *, int, char **);
int cmd_archive(struct repo *, int, char **);
int cmd_notes(struct repo *, int, char **);
int cmd_replace(struct repo *, int, char **);
int cmd_apply(struct repo *, int, char **);
int cmd_range_diff(struct repo *, int, char **);
int cmd_rerere(struct repo *, int, char **);
int cmd_worktree(struct repo *, int, char **);
int cmd_submodule(struct repo *, int, char **);
int cmd_sparse_checkout(struct repo *, int, char **);
/*
 * The credential helper protocol: what `credential.helper` names is run with
 * the operation as its argument, and the credential travels on files because
 * there is no way to be on both ends of a program otherwise (cmd_credential.c).
 */
int cmd_credential(struct repo *, int, char **);
/*
 * Whether a sparse-checkout pattern list leaves this path out of the work tree.
 * No is the answer whenever sparse checkout is off, which is the usual case, and
 * the patterns are read once per repository (cmd_sparse.c).
 */
int sparse_skips(struct repo *r, const char *relpath);
void sparse_forget(void);
/*
 * The working directory other than this one that has `branch` checked out, or
 * NULL -- what `checkout` refuses on, since one branch has one HEAD (cmd_worktree.c).
 */
char *worktree_other_holder(struct repo *, const char *branch);

int cmd_show(struct repo *, int, char **);
int cmd_diff(struct repo *, int, char **);
int cmd_reset(struct repo *, int, char **);
int cmd_reflog(struct repo *, int, char **);
int cmd_blame(struct repo *, int, char **);
int cmd_grep(struct repo *, int, char **);
int cmd_describe(struct repo *, int, char **);
int cmd_bisect(struct repo *, int, char **);

int cmd_prompt(struct repo *, int, char **);
int cmd_capture(struct repo *, int, char **);
int cmd_response(struct repo *, int, char **);
/*
 * Record an answer against the prompt it belongs to: the newest one in the
 * history, or the one named.  `force` replaces an answer that is already there.
 * Shared with `rerun --record`, which has the answer in hand rather than on a
 * command line, so this is the recording itself and not the argument parsing.
 */
int record_response(struct repo *r, const char *prompt_id, const char *body,
		    const char *model, const char *date, int force);
int cmd_outcome(struct repo *, int, char **);
int cmd_session(struct repo *, int, char **);
int cmd_replay(struct repo *, int, char **);
int cmd_attach(struct repo *, int, char **);
int cmd_rerun(struct repo *, int, char **);
int cmd_timeline(struct repo *, int, char **);
int cmd_log_prompt(struct repo *, int, char **);
int cmd_stats(struct repo *, int, char **);

int cmd_branch(struct repo *, int, char **);
int cmd_checkout(struct repo *, int, char **);
int cmd_switch(struct repo *, int, char **);
int cmd_merge(struct repo *, int, char **);
int cmd_cherry_pick(struct repo *, int, char **);
int cmd_rebase(struct repo *, int, char **);
int cmd_revert(struct repo *, int, char **);
int cmd_stash(struct repo *, int, char **);
int cmd_tag(struct repo *, int, char **);

/*
 * Whether a replay is stopped part way, and which kind, so that a commit made
 * while one is -- which would record the replayed commit by hand and leave the
 * state behind naming it -- can be refused.  `kind`, when given, is the
 * caller's to free.
 */
int replay_in_progress(struct repo *r, char **kind);

int cmd_remote(struct repo *, int, char **);
int cmd_push(struct repo *, int, char **);
int cmd_fetch(struct repo *, int, char **);
int cmd_pull(struct repo *, int, char **);
int cmd_serve(struct repo *, int, char **);

int cmd_fsck(struct repo *, int, char **);
int cmd_gc(struct repo *, int, char **);
int cmd_repack(struct repo *, int, char **);
int cmd_prune(struct repo *, int, char **);

/* shared plumbing helpers used across command files */
int load_tree_flat(struct repo *r, const oid_t *tree, const char *prefix,
		   void (*fn)(const char *path, u32 mode, const oid_t *oid,
			      void *data), void *data);
int index_write_tree_cb(struct repo *r, struct index_state *istate, oid_t *out);
char *abbrev_oid(const oid_t *oid);
/* hash one work-tree path into the object store and record it in the index */
int stage_worktree_path(struct repo *r, const char *relpath);
int diff_buffers(const char *a_label, const void *a, size_t alen,
		 const char *b_label, const void *b, size_t blen,
		 struct buf *out, int stat_only);
void diff_trees(struct repo *r, const oid_t *old_tree, const oid_t *new_tree,
		struct buf *out, int stat_only);

/* ------------------------------------------------------------------ */
/* the line diff engine (cmd_work.c)                                   */

/* one line, as a slice of the buffer it was split out of */
struct dline {
	const char *p;
	size_t len;
};

/* one line of the edit script, in the order the diff reads */
struct oline {
	char op;                /* ' ', '-', '+' */
	const char *p;
	size_t len;
	size_t a, b;            /* 0-based indices into a[] and b[] */
};

struct stat_counts {
	long add, del;
};

void diff_split_lines(const void *data, size_t len, struct dline **out, size_t *nr);
int lcs_diff(const struct dline *a, size_t na, const struct dline *b, size_t nb,
	     struct oline **out, size_t *nout, struct stat_counts *counts);

/*
 * Whether `path` is a prompt file in `dir`, which is the configured prompt
 * directory; `rest`, when given, is what is left of the path after it.
 */
int is_prompt_path(const char *dir, size_t dl, const char *path,
		   const char **rest);

/* ------------------------------------------------------------------ */
/* tracing a prompt's code (trace.c)                                   */

/*
 * One step of a commit's history: the change one prompt asked for, held as the
 * pair of trees the step is the difference between.  A step with no prompt is
 * the work that was already in the tree when the commit's first prompt was
 * recorded -- nobody asked for it, so nothing is named for it.
 */
struct trace_step {
	char *prompt;           /* p_..., or NULL */
	oid_t from;
	oid_t to;
	int have_from;          /* 0 when the step starts from an empty tree */
};

struct trace {
	struct trace_step *e;
	size_t nr;
	char *why;              /* why there are no steps, or NULL */
};

/*
 * The steps a commit went through, oldest first.  A commit whose prompts all
 * carry snapshots has one step per prompt, plus a step before the first when
 * work predates it; the steps telescope from the parent's tree to the commit's
 * own, so they are the commit's whole change and nothing else.  A commit that
 * carries no prompt, or a prompt with no snapshot, gets no steps and a `why`,
 * and the caller falls back to the commit-level prompt list.
 *
 * The caller releases the result with trace_release.
 */
void trace_of_commit(struct repo *r, const struct commit *c, struct trace *out);
void trace_release(struct trace *t);
/* the steps' diffs, each under the prompt it belongs to */
void trace_render(struct repo *r, const struct trace *t, struct buf *out);
/*
 * For a file as the commit holds it, the prompt each line came from, NULL for a
 * line that predates the commit's first prompt.  `nr` is the file's line count
 * and the ids are borrowed from the trace.  -1 when the commit has no chain to
 * trace the file through, or does not hold the file at all.
 */
int trace_file_prompts(struct repo *r, const struct trace *t, const char *path,
		       const char ***out, size_t *nr);

/* check a tree out onto the work tree; both used by checkout and merge */
void write_blob_to_worktree(struct repo *r, const char *relpath, const oid_t *oid);
void restore_all_from_index(struct repo *r, const struct index_state *ist);

/* ------------------------------------------------------------------ */
/* renames (rename.c)                                                  */

/*
 * A path one side lost and a path it gained, holding the same object.  Both
 * strings are owned by the list and freed with it.
 */
struct rename_pair {
	char *from;
	char *to;
};

struct rename_list {
	struct rename_pair *e;
	size_t nr, alloc;
};

/*
 * Every rename that turned `base` into `side`, in two passes: paths holding
 * the same object, then paths whose texts are still at least half the same.
 * The objects are read through `odb`, so a store of packed files works.
 */
void renames_between(struct odb *odb, struct index_state *base,
		     struct index_state *side, struct rename_list *out);
const struct rename_pair *rename_by_from(const struct rename_list *rl,
					 const char *from);
const struct rename_pair *rename_by_to(const struct rename_list *rl,
				       const char *to);
void rename_list_release(struct rename_list *rl);

/*
 * An unfinished merge: MERGE_HEAD points at the revision being merged in,
 * and the paths still in conflict are the ones the index holds above stage 0.
 */
int merge_in_progress(struct repo *r, oid_t *other);
void merge_state_write(struct repo *r, const oid_t *other, const char *subject);
void merge_state_clear(struct repo *r);
int merge_message(struct repo *r, struct buf *out);

/* ------------------------------------------------------------------ */
/* sockets and HTTP (net.c)                                            */

/*
 * The transport `serve` speaks and `gp://` reads.  It is HTTP with a
 * Content-Length and nothing else: no chunked bodies, no keep-alive, no
 * compression, one request per connection.  Bodies are read into memory, so
 * every length is bounded by what the caller is prepared to hold.
 */
int net_init(void);
int net_strerror(char *buf, size_t n);

/*
 * Listen on `host` (NULL or "loopback" for 127.0.0.1, "any" for every
 * interface).  Port 0 asks the system to choose; *got_port receives what it
 * chose, which is the only way to talk to a server that was just started.
 */
gp_socket net_listen(const char *host, int port, int *got_port);
gp_socket net_accept(gp_socket listener);
void net_close(gp_socket fd);
/* connect to host:port, reporting the reason on failure */
gp_socket net_connect(const char *host, int port);

/* a buffered reader over a socket, so a request can be read a line at a time */
struct http_in {
	gp_socket fd;
	u8 buf[4096];
	size_t len, pos;
};
void http_in_init(struct http_in *in, gp_socket fd);
/* one byte, or -1 at end of input */
int http_get(struct http_in *in);
/* exactly n bytes; fewer only when the peer closed early */
size_t http_read(struct http_in *in, void *dst, size_t n);
/* one line, with CR and LF stripped; -1 when nothing was left to read */
int http_line(struct http_in *in, struct buf *out);

/* read a request head; -1 when the connection carried no request at all */
int http_read_request(struct http_in *in, struct buf *method, struct buf *path,
		      struct buf *headers, size_t *content_length);
/* write a response with an exact body length, and flush it */
int http_respond(gp_socket fd, int status, const char *ctype, const void *body,
		 size_t len);

/* gp://host[:port]/path -- the one URL form the transport understands */
struct gp_url {
	char host[256];
	int port;
	char path[512];
};
int gp_url_parse(const char *url, struct gp_url *out);

/*
 * Send one request and collect the response.  *status receives the status
 * code and `body` the response body, whatever the code was: a refusal carries
 * an explanation in its body, and the caller should print it.
 */
int http_request(const struct gp_url *u, const char *method, const char *path,
		 const void *body, size_t len, int *status, struct buf *body_out);

#endif /* GP_H */
