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
};
#define COMMIT_INIT { { {0} }, OID_ARRAY_INIT, NULL, NULL, NULL, NULL }
void commit_release(struct commit *c);
void commit_parse(struct commit *c, const void *data, size_t len);
void commit_format(const struct commit *c, struct buf *out);

/* ------------------------------------------------------------------ */
/* prompt and session objects                                          */

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
};
#define PROMPT_INIT { NULL,NULL,0,NULL,0,NULL,NULL,NULL,0,NULL,NULL,NULL,0,NULL,NULL }
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
	struct buf tmp;
	struct pack *packs;     /* the object packs, loaded on demand */
	size_t nr_packs, alloc_packs;
	int packs_loaded;
};
#define ODB_INIT { NULL, BUF_INIT, NULL, 0, 0, 0 }
void odb_init(struct odb *o, const char *dir);
void odb_release(struct odb *o);

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
	char *dir;              /* .gitprompt */
	char *ident;            /* cached identity, for reflog lines */
};
#define REF_STORE_INIT { NULL, NULL }
void refs_init(struct ref_store *r, const char *gpdir);
void refs_release(struct ref_store *r);

int refs_read(struct ref_store *r, const char *name, oid_t *out);
int refs_write(struct ref_store *r, const char *name, const oid_t *oid);
int refs_delete(struct ref_store *r, const char *name);
int refs_exists(struct ref_store *r, const char *name);
void refs_reflog(struct ref_store *r, const char *name, const oid_t *old,
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
	char *gpdir;            /* the .gitprompt directory */
	struct odb odb;
	struct ref_store refs;
};
#define REPO_INIT { NULL, NULL, ODB_INIT, REF_STORE_INIT }

int repo_find(struct repo *r, const char *start);      /* search upwards */
int repo_open(struct repo *r, const char *dir);
void repo_release(struct repo *r);

const char *repo_index_path(struct repo *r);
const char *repo_head_path(struct repo *r);
char *repo_git_path(struct repo *r, const char *fmt, ...);

int repo_config_get(struct repo *r, const char *key, char **out);
int repo_config_set(struct repo *r, const char *key, const char *value,
		    int global);
int repo_config_unset(struct repo *r, const char *key);
void repo_config_list(struct repo *r, int global,
		      void (*fn)(const char *k, const char *v, void *),
		      void *data);
/* resolved identity */
void repo_ident(struct repo *r, struct buf *out);      /* "Name <email>" */
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
/* the working tree                                                    */

int write_tree_from_index(struct repo *r, const struct index_state *istate,
			  oid_t *out);
/* check out a tree into the work tree; force==0 refuses to clobber */
int checkout_tree(struct repo *r, const oid_t *tree, int force, int update_index);
int read_tree_into_index(struct repo *r, struct index_state *istate,
			 const oid_t *tree, const char *prefix);

/* paths */
void path_normalize(const char *in, struct buf *out);   /* no leading ./, / */
int path_is_ignored(struct repo *r, const char *relpath);
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
int commit_peel(struct repo *r, const oid_t *oid, enum obj_type want, oid_t *out);
void read_commit(struct repo *r, const oid_t *oid, struct commit *c);
void read_tree_obj(struct repo *r, const oid_t *oid, struct tree *t);
char *commit_message_line(const struct commit *c);
i64 commit_time(const struct commit *c);

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
int cmd_status(struct repo *, int, char **);
int cmd_commit(struct repo *, int, char **);
int cmd_log(struct repo *, int, char **);
int cmd_show(struct repo *, int, char **);
int cmd_diff(struct repo *, int, char **);
int cmd_reset(struct repo *, int, char **);
int cmd_reflog(struct repo *, int, char **);
int cmd_describe(struct repo *, int, char **);

int cmd_prompt(struct repo *, int, char **);
int cmd_capture(struct repo *, int, char **);
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
int cmd_tag(struct repo *, int, char **);

int cmd_remote(struct repo *, int, char **);
int cmd_push(struct repo *, int, char **);
int cmd_fetch(struct repo *, int, char **);
int cmd_pull(struct repo *, int, char **);
int cmd_serve(struct repo *, int, char **);

int cmd_fsck(struct repo *, int, char **);
int cmd_gc(struct repo *, int, char **);

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
