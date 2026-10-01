/*
 * cmd_plumb.c - init, config, and the plumbing that talks to the object
 * store and the refs directly.
 */
#include "gp.h"

#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define gp_getcwd2(b, n) _getcwd((b), (int)(n))
#else
#include <unistd.h>
#define gp_getcwd2(b, n) getcwd((b), (n))
#endif

/* read a whole file or stdin, whichever the caller asked for */
static int slurp_input(const char *file, struct buf *out)
{
	if (file && strcmp(file, "-"))
		return read_file(file, out);
	{
		char chunk[65536];
		size_t n;
		buf_reset(out);
		while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0)
			buf_add(out, chunk, n);
		return 0;
	}
}

/* ------------------------------------------------------------------ */
/* init                                                                */

int cmd_init(struct repo *unused, int argc, char **argv)
{
	struct opts o;
	const char *dir;
	const char *branch;
	int bare;
	int existed;
	char *gpdir;
	const char *root;
	char cwd[4096];

	(void)unused;
	opts_init(&o, argc, argv, (const char *const[]){
		"--bare", "-b=", "--initial-branch=", NULL });
	bare = opts_flag(&o, "--bare");
	dir = opts_arg(&o, 0);

	if (opts_flag(&o, "-b") || opts_flag(&o, "--initial-branch"))
		branch = opts_value(&o, "-b") ? opts_value(&o, "-b")
					      : opts_value(&o, "--initial-branch");
	else
		branch = NULL;

	if (!gp_getcwd2(cwd, sizeof cwd))
		gp_die("cannot determine the current directory");

	if (!dir || !*dir)
		dir = ".";
	root = dir;
	/* init <dir> creates <dir>, not just <dir>/.gitprompt */
	if (strcmp(dir, ".")) {
		if (mkdir_p(dir) < 0)
			gp_die("cannot create directory %s", dir);
	}

	gpdir = bare ? xstrfmt("%s", root) : xstrfmt("%s/.gitprompt", root);

	existed = is_directory(gpdir) && is_file(xstrfmt("%s/HEAD", gpdir));
	if (existed)
		printf("Reinitialized existing gitprompt repository in %s/\n",
		       gpdir);
	else
		printf("Initialized empty gitprompt repository in %s/\n", gpdir);

	{
		char *p;
		p = xstrfmt("%s/objects", gpdir);
		if (mkdir_p(p) < 0)
			gp_die("cannot create %s", p);
		free(p);
		p = xstrfmt("%s/refs/heads", gpdir);
		mkdir_p(p);
		free(p);
		p = xstrfmt("%s/refs/tags", gpdir);
		mkdir_p(p);
		free(p);
		p = xstrfmt("%s/refs/remotes", gpdir);
		mkdir_p(p);
		free(p);
	}

	/* HEAD before config, so a repository is recognisable the moment it
	 * exists rather than a step later */
	{
		struct ref_store rs;
		char *target = xstrfmt("refs/heads/%s", branch ? branch : "main");
		refs_init(&rs, gpdir);
		refs_set_head(&rs, target);
		free(target);
		refs_release(&rs);
	}

	{
		char *cfg = xstrfmt("%s/config", gpdir);
		struct buf b;
		buf_init(&b);
		buf_addstr(&b, "[core]\n");
		buf_addf(&b, "\trepositoryformatversion = 0\n");
		buf_addf(&b, "\tfilemode = %s\n",
#ifdef _WIN32
			 "false"
#else
			 "true"
#endif
			);
		buf_addf(&b, "\tbare = %s\n", bare ? "true" : "false");
		buf_addstr(&b, "[gitprompt]\n");
		buf_addstr(&b, "\tpromptDir = prompts\n");
		write_file(cfg, b.b, b.len);
		buf_release(&b);
		free(cfg);
	}

	/*
	 * A prompt history that arrived as a plain `git clone` has the files and
	 * nothing else: git copies the tree and leaves the index, refs and
	 * objects behind.  Taking whatever already sits under the prompt
	 * directory into the index is what makes such a clone a repository that
	 * can be committed and pushed again -- one command, rather than "clone
	 * it, then work out what to add".  A reinit is left alone: the index is
	 * the user's by then, not ours to restage.
	 */
	if (!existed && !bare) {
		struct repo r;

		if (repo_open(&r, root) == 0) {
			char *pd = xstrdup(repo_prompt_dir(&r));

			if (is_directory(pd)) {
				char *args[1];

				args[0] = pd;
				if (cmd_add(&r, 1, args) == 0)
					printf("Adopted the existing %s/ tree.\n", pd);
			}
			free(pd);
			repo_release(&r);
		}
	}

	free(gpdir);
	return 0;
}

/* ------------------------------------------------------------------ */
/* config                                                              */

struct list_ctx {
	int seen;
};

static void list_one(const char *k, const char *v, void *ud)
{
	struct list_ctx *c = ud;
	printf("%s=%s\n", k, v);
	c->seen++;
}

int cmd_config(struct repo *r, int argc, char **argv)
{
	struct opts o;
	int global;
	const char *key, *value;

	opts_init(&o, argc, argv, (const char *const[]){
		"--global", "-g", "--list", "-l", "--unset", "--get",
		"--set", NULL });
	global = opts_flag(&o, "--global") || opts_flag(&o, "-g");

	if (opts_flag(&o, "--list") || opts_flag(&o, "-l")) {
		struct list_ctx c = { 0 };
		repo_config_list(r, global, list_one, &c);
		return 0;
	}

	key = opts_arg(&o, 0);
	if (!key) {
		gp_error("config: expected a key\nusage: gitprompt config [--global] "
			 "[--list] [--unset] <key> [<value>]");
		return 1;
	}
	value = opts_arg(&o, 1);
	if (!value && opts_flag(&o, "--set")) {
		gp_error("config --set: expected a value");
		return 1;
	}

	if (opts_flag(&o, "--unset")) {
		if (repo_config_unset(r, key) < 0) {
			gp_error("config: no such key: %s", key);
			return 1;
		}
		return 0;
	}

	if (value) {
		if (repo_config_set(r, key, value, global) < 0) {
			gp_error("config: cannot write configuration");
			return 1;
		}
		return 0;
	}

	{
		char *got = NULL;
		if (repo_config_get(r, key, &got) < 0) {
			/* git exits 1 with no output for a missing key */
			return 1;
		}
		printf("%s\n", got);
		free(got);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* hash-object                                                         */

int cmd_hash_object(struct repo *r, int argc, char **argv)
{
	struct opts o;
	enum obj_type type = OBJ_BLOB;
	struct odb *odb;
	int write_it, i, rc = 0;

	opts_init(&o, argc, argv, (const char *const[]){
		"-t=", "-w", "--stdin", NULL });
	write_it = opts_flag(&o, "-w");
	if (opts_value(&o, "-t")) {
		type = obj_type_from_name(opts_value(&o, "-t"));
		if (type == OBJ_NONE)
			gp_die("invalid object type '%s'", opts_value(&o, "-t"));
	}
	/* hashing needs no store; only storing does.  odb_hash leaves a NULL
	 * store alone when it is not writing, so the same call serves both
	 * cases and the dereference lives in one place. */
	if (write_it && !r)
		gp_die("hash-object -w: not in a gitprompt repository");
	odb = r ? &r->odb : NULL;

	if (opts_flag(&o, "--stdin") || o.nargs == 0) {
		struct buf b;
		oid_t oid;
		char hex[GP_SHA1_HEXSZ + 1];
		buf_init(&b);
		slurp_input(NULL, &b);
		if (write_it)
			rc = odb_write(odb, type, b.b, b.len, &oid);
		else
			rc = odb_hash(odb, type, b.b, b.len, &oid, 0);
		buf_release(&b);
		if (rc < 0) {
			gp_error("cannot hash stdin");
			return 1;
		}
		oid_hex(&oid, hex);
		printf("%s\n", hex);
		return 0;
	}

	for (i = 0; i < o.nargs; i++) {
		struct buf b;
		oid_t oid;
		char hex[GP_SHA1_HEXSZ + 1];

		buf_init(&b);
		if (slurp_input(o.args[i], &b) < 0) {
			gp_error("cannot open '%s'", o.args[i]);
			buf_release(&b);
			rc = 1;
			continue;
		}
		if (write_it)
			rc = odb_write(odb, type, b.b, b.len, &oid);
		else
			rc = odb_hash(odb, type, b.b, b.len, &oid, 0);
		buf_release(&b);
		if (rc < 0) {
			gp_error("cannot hash '%s'", o.args[i]);
			rc = 1;
			continue;
		}
		oid_hex(&oid, hex);
		printf("%s\n", hex);
	}
	return rc;
}

/* ------------------------------------------------------------------ */
/* cat-file                                                            */

static void print_tree_pretty(struct repo *r, const oid_t *oid)
{
	struct tree t = TREE_INIT;
	size_t i;

	read_tree_obj(r, oid, &t);
	for (i = 0; i < t.nr; i++) {
		char hex[GP_SHA1_HEXSZ + 1];
		const char *type = (t.e[i].mode & 0170000) == 0040000 ? "tree" : "blob";
		oid_hex(&t.e[i].oid, hex);
		printf("%06o %s %s\t%s\n", t.e[i].mode, type, hex, t.e[i].name);
	}
	tree_release(&t);
}

int cmd_cat_file(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *what = NULL;
	oid_t oid;
	enum obj_type type;
	struct buf b;

	opts_init(&o, argc, argv, (const char *const[]){
		"-t", "-s", "-p", "-e", NULL });
	if (opts_flag(&o, "-t")) what = "type";
	else if (opts_flag(&o, "-s")) what = "size";
	else if (opts_flag(&o, "-p")) what = "pretty";
	else if (opts_flag(&o, "-e")) what = "exists";
	else {
		/* gitprompt also accepts a bare object name and pretty-prints */
		what = "pretty";
	}

	{
		const char *rev = opts_arg(&o, 0);
		if (!rev)
			gp_die("cat-file: expected an object\nusage: gitprompt cat-file "
			       "(-t|-s|-p|-e) <object>");
		if (resolve_rev(r, rev, &oid) < 0) {
			if (!strcmp(what, "exists"))
				return 1;
			gp_die("not a valid object name: %s", rev);
		}
	}

	if (!strcmp(what, "exists"))
		return odb_exists(&r->odb, &oid) ? 0 : 1;

	buf_init(&b);
	if (odb_read(&r->odb, &oid, &type, &b) < 0) {
		buf_release(&b);
		gp_die("cannot read object %s", opts_arg(&o, 0));
	}

	if (!strcmp(what, "type")) {
		printf("%s\n", obj_type_name(type));
	} else if (!strcmp(what, "size")) {
		printf("%lu\n", (unsigned long)b.len);
	} else if (type == OBJ_TREE) {
		print_tree_pretty(r, &oid);
	} else {
		fwrite(b.b, 1, b.len, stdout);
	}
	buf_release(&b);
	return 0;
}

/* ------------------------------------------------------------------ */
/* ls-tree                                                             */

struct lst_ctx {
	int recursive;
	int name_only;
};

static void lst_cb(const char *path, u32 mode, const oid_t *oid, void *ud)
{
	struct lst_ctx *c = ud;
	char hex[GP_SHA1_HEXSZ + 1];
	oid_hex(oid, hex);
	if (c->name_only)
		printf("%s\n", path);
	else
		printf("%06o %s %s\t%s\n", mode,
		       (mode & 0170000) == 0040000 ? "tree" : "blob", hex, path);
}

int cmd_ls_tree(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct lst_ctx c = { 0, 0 };
	oid_t tree;

	opts_init(&o, argc, argv, (const char *const[]){
		"-r", "--name-only", NULL });
	c.recursive = opts_flag(&o, "-r");
	c.name_only = opts_flag(&o, "--name-only");

	if (opts_arg(&o, 0)) {
		if (resolve_rev_tree(r, opts_arg(&o, 0), &tree) < 0)
			gp_die("not a tree: %s", opts_arg(&o, 0));
	} else if (refs_head(&r->refs, &tree) == 0) {
		struct commit cm = COMMIT_INIT;
		read_commit(r, &tree, &cm);
		tree = cm.tree;
		commit_release(&cm);
	} else {
		gp_die("ls-tree: no tree given and HEAD does not exist");
	}

	if (c.recursive) {
		load_tree_flat(r, &tree, "", lst_cb, &c);
	} else {
		struct tree t = TREE_INIT;
		size_t i;
		read_tree_obj(r, &tree, &t);
		for (i = 0; i < t.nr; i++) {
			char hex[GP_SHA1_HEXSZ + 1];
			oid_hex(&t.e[i].oid, hex);
			if (c.name_only)
				printf("%s\n", t.e[i].name);
			else
				printf("%06o %s %s\t%s\n", t.e[i].mode,
				       (t.e[i].mode & 0170000) == 0040000
						? "tree" : "blob",
				       hex, t.e[i].name);
		}
		tree_release(&t);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* write-tree / commit-tree                                            */

int cmd_write_tree(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct index_state ist;
	oid_t tree;
	char hex[GP_SHA1_HEXSZ + 1];

	opts_init(&o, argc, argv, NULL);
	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));
	if (write_tree_from_index(r, &ist, &tree) < 0) {
		index_release(&ist);
		gp_die("cannot write tree");
	}
	index_release(&ist);
	oid_hex(&tree, hex);
	printf("%s\n", hex);
	return 0;
}

int cmd_commit_tree(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct commit c = COMMIT_INIT;
	struct buf ident, msg, treebuf;
	oid_t tree_oid, out;
	char hex[GP_SHA1_HEXSZ + 1];
	int i;

	opts_init(&o, argc, argv, (const char *const[]){
		"-p=", "-m=", "-F=", NULL });
	if (!opts_arg(&o, 0))
		gp_die("commit-tree: expected a tree\nusage: gitprompt commit-tree "
		       "<tree> [-p <parent>]... [-m <msg>]");
	if (resolve_rev_tree(r, opts_arg(&o, 0), &tree_oid) < 0)
		gp_die("not a tree: %s", opts_arg(&o, 0));
	c.tree = tree_oid;

	for (i = 0; i < o.nf; i++) {
		if (!strcmp(o.flags[i].name, "-p") && o.flags[i].value) {
			oid_t p;
			if (resolve_rev(r, o.flags[i].value, &p) < 0)
				gp_die("bad parent: %s", o.flags[i].value);
			oid_array_append(&c.parents, &p);
		}
	}

	buf_init(&ident);
	buf_init(&msg);
	buf_init(&treebuf);

	repo_ident_with_time(r, &ident);
	/* separate allocations: commit_release frees each in turn */
	c.author = xstrdup(buf_cstr(&ident));
	c.committer = xstrdup(buf_cstr(&ident));

	if (opts_value(&o, "-m"))
		buf_addstr(&msg, opts_value(&o, "-m"));
	else if (opts_value(&o, "-F"))
		read_file(opts_value(&o, "-F"), &msg);
	else
		slurp_input(NULL, &msg);
	if (msg.len && msg.b[msg.len - 1] != '\n')
		buf_addch(&msg, '\n');
	c.message = xstrdup(buf_cstr(&msg));

	{
		char *sess = repo_current_session(r);
		if (sess)
			c.session = sess;
	}

	commit_format(&c, &treebuf);
	if (odb_write(&r->odb, OBJ_COMMIT, treebuf.b, treebuf.len, &out) < 0)
		gp_die("cannot write commit");
	oid_hex(&out, hex);
	printf("%s\n", hex);

	free(c.session);
	commit_release(&c);
	buf_release(&ident);
	buf_release(&msg);
	buf_release(&treebuf);
	return 0;
}

/* ------------------------------------------------------------------ */
/* rev-parse                                                           */

int cmd_rev_parse(struct repo *r, int argc, char **argv)
{
	struct opts o;
	int i, rc = 0;
	int short_form;

	opts_init(&o, argc, argv, (const char *const[]){ "--short", NULL });
	short_form = opts_flag(&o, "--short");

	if (!o.nargs) {
		gp_die("rev-parse: expected a revision");
	}
	for (i = 0; i < o.nargs; i++) {
		oid_t oid;
		char hex[GP_SHA1_HEXSZ + 1];
		if (resolve_rev(r, o.args[i], &oid) < 0) {
			gp_error("ambiguous argument '%s': unknown revision", o.args[i]);
			rc = 128;
			continue;
		}
		oid_hex(&oid, hex);
		if (short_form) {
			hex[7] = '\0';
			printf("%s\n", hex);
		} else {
			printf("%s\n", hex);
		}
	}
	return rc;
}

/* ------------------------------------------------------------------ */
/* update-ref / symbolic-ref / for-each-ref                            */

int cmd_update_ref(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *name, *newrev, *oldrev;
	oid_t newoid, oldoid;
	int had_old;

	opts_init(&o, argc, argv, NULL);
	name = opts_arg(&o, 0);
	newrev = opts_arg(&o, 1);
	oldrev = opts_arg(&o, 2);
	if (!name || !newrev)
		gp_die("update-ref: expected <ref> <new> [<old>]");

	if (resolve_rev(r, newrev, &newoid) < 0)
		gp_die("update-ref: not a valid object name: %s", newrev);
	had_old = refs_read(&r->refs, name, &oldoid) == 0;
	if (oldrev) {
		oid_t want;
		if (resolve_rev(r, oldrev, &want) < 0)
			gp_die("update-ref: not a valid object name: %s", oldrev);
		if (!had_old || !oid_equal(&oldoid, &want))
			gp_die("update-ref: %s does not match the old value", name);
	}
	if (refs_write(&r->refs, name, &newoid) < 0)
		gp_die("update-ref: cannot write %s", name);
	refs_reflog(&r->refs, name, had_old ? &oldoid : &null_oid, &newoid,
		    "update-ref");
	return 0;
}

int cmd_symbolic_ref(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *name, *target;

	opts_init(&o, argc, argv, NULL);
	name = opts_arg(&o, 0);
	target = opts_arg(&o, 1);
	if (!name)
		gp_die("symbolic-ref: expected a name");

	if (!target) {
		char *t = refs_head_target(&r->refs);
		/* look up any symbolic ref, not only HEAD */
		if (strcmp(name, "HEAD") != 0) {
			free(t);
			t = NULL;
		}
		if (name[0] == '\0')
			gp_die("symbolic-ref: empty name");
		if (t && !strcmp(name, "HEAD")) {
			printf("%s\n", t);
			free(t);
			return 0;
		}
		free(t);
		t = NULL;
		{
			/* read the file directly for a non-HEAD symref */
			char *path = repo_git_path(r, "%s", name);
			struct buf b;
			buf_init(&b);
			if (read_file(path, &b) == 0) {
				char *s = (char *)b.b;
				size_t n = b.len;
				while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
					s[--n] = '\0';
				if (n > 5 && !memcmp(s, "ref: ", 5))
					t = xstrdup(s + 5);
			}
			buf_release(&b);
			free(path);
		}
		if (!t) {
			gp_error("ref %s is not a symbolic ref", name);
			return 1;
		}
		printf("%s\n", t);
		free(t);
		return 0;
	}

	if (refs_check_name(target) < 0)
		gp_die("symbolic-ref: refusing to point at an invalid name: %s", target);
	if (!strcmp(name, "HEAD"))
		return refs_set_head(&r->refs, target) < 0 ? 1 : 0;
	{
		char *path = repo_git_path(r, "%s", name);
		char *line = xstrfmt("ref: %s\n", target);
		char *dir = xstrdup(path);
		char *slash = strrchr(dir, '/');
		int rc;
		if (slash) {
			*slash = '\0';
			mkdir_p(dir);
		}
		free(dir);
		rc = write_file(path, line, strlen(line));
		free(line);
		free(path);
		return rc < 0 ? 1 : 0;
	}
}

struct fer_ctx {
	struct repo *r;
	const char *prefix;
};

static void fer_one(const char *name, const oid_t *oid, void *ud)
{
	struct fer_ctx *c = ud;
	char hex[GP_SHA1_HEXSZ + 1];
	enum obj_type t;

	oid_hex(oid, hex);
	if (odb_type_of(&c->r->odb, oid, &t) < 0)
		printf("%s %s\n", hex, name);
	else
		printf("%s %s %s\n", hex, obj_type_name(t), name);
}

int cmd_for_each_ref(struct repo *r, int argc, char **argv)
{
	struct opts o;
	const char *prefix;
	char *p;

	opts_init(&o, argc, argv, NULL);
	prefix = opts_arg(&o, 0);
	if (!prefix)
		prefix = "refs/";
	p = xstrdup(prefix);
	{
		struct fer_ctx c;
		c.r = r;
		c.prefix = p;
		refs_list(&r->refs, p, fer_one, &c);
		refs_list_packed(&r->refs, p, fer_one, &c);
	}
	free(p);
	return 0;
}

/* ------------------------------------------------------------------ */
/* ls-files / count-objects / verify-objects / check-ref-format        */

int cmd_ls_files(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct index_state ist;
	size_t i;
	int show_stage;

	opts_init(&o, argc, argv, (const char *const[]){
		"-s", "--stage", NULL });
	show_stage = opts_flag(&o, "-s") || opts_flag(&o, "--stage");

	memset(&ist, 0, sizeof ist);
	index_read(&ist, repo_index_path(r));
	for (i = 0; i < ist.nr; i++) {
		if (o.nargs) {
			int j, matched = 0;
			for (j = 0; j < o.nargs; j++)
				if (!strncmp(ist.e[i].path, o.args[j], strlen(o.args[j])))
					matched = 1;
			if (!matched)
				continue;
		}
		if (show_stage) {
			char hex[GP_SHA1_HEXSZ + 1];
			oid_hex(&ist.e[i].oid, hex);
			printf("%06o %s %u\t%s\n", ist.e[i].mode, hex,
			       (unsigned)ist.e[i].stage, ist.e[i].path);
		} else {
			printf("%s\n", ist.e[i].path);
		}
	}
	index_release(&ist);
	return 0;
}

int cmd_count_objects(struct repo *r, int argc, char **argv)
{
	struct opts o;
	size_t n;

	opts_init(&o, argc, argv, (const char *const[]){ "-v", NULL });
	n = odb_count(&r->odb);
	if (opts_flag(&o, "-v")) {
		size_t packed = odb_count_packed(&r->odb);

		printf("count: %lu\n", (unsigned long)n);
		/* git reports these in kilobytes, rounded down */
		printf("size: %lu\n",
		       (unsigned long)(odb_loose_size(&r->odb) / 1024));
		printf("in-pack: %lu\n", (unsigned long)packed);
		printf("packs: %lu\n", (unsigned long)odb_nr_packs(&r->odb));
		printf("size-pack: %lu\n",
		       (unsigned long)(odb_pack_size(&r->odb) / 1024));
	} else {
		printf("%lu objects\n", (unsigned long)n);
	}
	return 0;
}

struct verify_ctx {
	struct repo *r;
	int bad;
	int checked;
};

static int verify_one(const oid_t *oid, void *ud)
{
	struct verify_ctx *c = ud;
	/* odb_read resets the buffer it is handed, so it has to arrive
	 * empty: an uninitialised one would have its garbage pointer written
	 * through and then freed */
	struct buf b = BUF_INIT;
	enum obj_type t;
	oid_t recomputed;

	c->checked++;
	if (odb_read(&c->r->odb, oid, &t, &b) < 0) {
		gp_error("%s: unreadable", abbrev_oid(oid));
		c->bad++;
		return 0;
	}
	if (odb_hash(&c->r->odb, t, b.b, b.len, &recomputed, 0) < 0 ||
	    !oid_equal(&recomputed, oid)) {
		gp_error("%s: content does not hash to its name", abbrev_oid(oid));
		c->bad++;
	}
	buf_release(&b);
	return 0;
}

int cmd_verify_objects(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct verify_ctx c = { NULL, 0, 0 };

	opts_init(&o, argc, argv, NULL);
	c.r = r;
	/* both forms: a hash checked only where it is loose would say nothing
	 * about the objects a fetch left packed */
	odb_foreach(&r->odb, verify_one, &c);
	printf("Checked %d object(s)\n", c.checked);
	return c.bad ? 1 : 0;
}

int cmd_check_ref_format(struct repo *r, int argc, char **argv)
{
	struct opts o;
	(void)r;
	opts_init(&o, argc, argv, NULL);
	if (!opts_arg(&o, 0))
		gp_die("check-ref-format: expected a name");
	return refs_check_name(opts_arg(&o, 0)) == 0 ? 0 : 1;
}
