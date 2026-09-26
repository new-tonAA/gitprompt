/*
 * json.c - the prompt and session file format, and a small JSON reader.
 *
 * A prompt is a markdown file with a frontmatter block:
 *
 *     ---
 *     id: p_vw8xgbp7
 *     session: s_1790354733_42fkw2
 *     seq: 1
 *     timestamp: 2026-09-26T00:45:33+08:00
 *     author: Your Name <you@example.com>
 *     ---
 *     Build a login page with email and password fields.
 *
 * That file, byte for byte, is the blob in the object store.  There is no
 * second representation to keep in step, which is what lets `status`
 * compare a working file against the index by hashing it.
 *
 * The JSON reader exists for one job: the refs.json that a carrier
 * repository holds when gitprompt is hosted on GitHub.
 */
#include "gp.h"

#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#include <process.h>
#define gp_getpid() ((long)_getpid())
#else
#include <unistd.h>
#define gp_getpid() ((long)getpid())
#endif

/* ------------------------------------------------------------------ */
/* the document model                                                  */

enum jtype { J_NULL, J_TRUE, J_FALSE, J_NUM, J_STR, J_ARR, J_OBJ };

struct jval {
	enum jtype type;
	double num;
	char *str;
	struct jval **items;
	size_t nr, alloc;
	char **keys;
	struct jval **vals;
	size_t nkv, kalloc;
};

static struct jval *jnew(enum jtype t)
{
	struct jval *v = xcalloc(1, sizeof *v);
	v->type = t;
	return v;
}

void json_free(struct jval *v)
{
	size_t i;
	if (!v)
		return;
	for (i = 0; i < v->nr; i++)
		json_free(v->items[i]);
	for (i = 0; i < v->nkv; i++) {
		free(v->keys[i]);
		json_free(v->vals[i]);
	}
	free(v->items);
	free(v->keys);
	free(v->vals);
	free(v->str);
	free(v);
}

static void jarr_push(struct jval *v, struct jval *item)
{
	if (v->nr == v->alloc) {
		v->alloc = v->alloc ? v->alloc * 2 : 8;
		v->items = xrealloc(v->items, v->alloc * sizeof(*v->items));
	}
	v->items[v->nr++] = item;
}

static void jobj_push(struct jval *v, char *key, struct jval *val)
{
	if (v->nkv == v->kalloc) {
		v->kalloc = v->kalloc ? v->kalloc * 2 : 8;
		v->keys = xrealloc(v->keys, v->kalloc * sizeof(*v->keys));
		v->vals = xrealloc(v->vals, v->kalloc * sizeof(*v->vals));
	}
	v->keys[v->nkv] = key;
	v->vals[v->nkv] = val;
	v->nkv++;
}

/* ------------------------------------------------------------------ */
/* parsing                                                             */

struct jparser {
	const char *p, *end;
};

static void jskip(struct jparser *jp)
{
	while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' ||
				   *jp->p == '\n' || *jp->p == '\r'))
		jp->p++;
}

static void jexpect(struct jparser *jp, char c)
{
	jskip(jp);
	if (jp->p >= jp->end || *jp->p != c)
		gp_die("corrupt JSON: expected '%c'", c);
	jp->p++;
}

static char *jparse_string_raw(struct jparser *jp)
{
	struct buf b;
	size_t n;
	char *s;

	buf_init(&b);
	if (jp->p >= jp->end || *jp->p != '"')
		gp_die("corrupt JSON: expected a string");
	jp->p++;
	while (jp->p < jp->end && *jp->p != '"') {
		if (*jp->p == '\\') {
			jp->p++;
			if (jp->p >= jp->end)
				break;
			switch (*jp->p) {
			case 'n': buf_addch(&b, '\n'); break;
			case 't': buf_addch(&b, '\t'); break;
			case 'r': buf_addch(&b, '\r'); break;
			case 'b': buf_addch(&b, '\b'); break;
			case 'f': buf_addch(&b, '\f'); break;
			case '/': buf_addch(&b, '/'); break;
			case '"': buf_addch(&b, '"'); break;
			case '\\': buf_addch(&b, '\\'); break;
			case 'u': {
				/* only the BMP escapes our own writer emits */
				unsigned code = 0;
				int i;
				for (i = 0; i < 4 && jp->p + 1 < jp->end; i++) {
					int c = (unsigned char)jp->p[1];
					int d;
					if (c >= '0' && c <= '9')
						d = c - '0';
					else if (c >= 'a' && c <= 'f')
						d = c - 'a' + 10;
					else if (c >= 'A' && c <= 'F')
						d = c - 'A' + 10;
					else
						break;
					code = code * 16 + (unsigned)d;
					jp->p++;
				}
				if (code < 0x80) {
					buf_addch(&b, (int)code);
				} else if (code < 0x800) {
					buf_addch(&b, 0xC0 | (code >> 6));
					buf_addch(&b, 0x80 | (code & 0x3F));
				} else {
					buf_addch(&b, 0xE0 | (code >> 12));
					buf_addch(&b, 0x80 | ((code >> 6) & 0x3F));
					buf_addch(&b, 0x80 | (code & 0x3F));
				}
				break;
			}
			default: buf_addch(&b, *jp->p); break;
			}
			jp->p++;
			continue;
		}
		buf_addch(&b, *jp->p);
		jp->p++;
	}
	jexpect(jp, '"');
	s = buf_detach(&b, &n);
	return s;
}

static struct jval *jparse_value(struct jparser *jp)
{
	jskip(jp);
	if (jp->p >= jp->end)
		gp_die("corrupt JSON: unexpected end of input");

	if (*jp->p == '{') {
		struct jval *o = jnew(J_OBJ);
		jp->p++;
		jskip(jp);
		if (jp->p < jp->end && *jp->p == '}') {
			jp->p++;
			return o;
		}
		for (;;) {
			char *key;
			struct jval *val;
			jskip(jp);
			key = jparse_string_raw(jp);
			jexpect(jp, ':');
			val = jparse_value(jp);
			jobj_push(o, key, val);
			jskip(jp);
			if (jp->p < jp->end && *jp->p == ',') {
				jp->p++;
				continue;
			}
			jexpect(jp, '}');
			break;
		}
		return o;
	}
	if (*jp->p == '[') {
		struct jval *a = jnew(J_ARR);
		jp->p++;
		jskip(jp);
		if (jp->p < jp->end && *jp->p == ']') {
			jp->p++;
			return a;
		}
		for (;;) {
			jarr_push(a, jparse_value(jp));
			jskip(jp);
			if (jp->p < jp->end && *jp->p == ',') {
				jp->p++;
				continue;
			}
			jexpect(jp, ']');
			break;
		}
		return a;
	}
	if (*jp->p == '"') {
		struct jval *s = jnew(J_STR);
		s->str = jparse_string_raw(jp);
		return s;
	}
	if (jp->end - jp->p >= 4 && !memcmp(jp->p, "true", 4)) {
		jp->p += 4;
		return jnew(J_TRUE);
	}
	if (jp->end - jp->p >= 5 && !memcmp(jp->p, "false", 5)) {
		jp->p += 5;
		return jnew(J_FALSE);
	}
	if (jp->end - jp->p >= 4 && !memcmp(jp->p, "null", 4)) {
		jp->p += 4;
		return jnew(J_NULL);
	}
	{
		char *end;
		double d = strtod(jp->p, &end);
		struct jval *n;
		if (end == jp->p)
			gp_die("corrupt JSON: unexpected byte '%c'", *jp->p);
		jp->p = end;
		n = jnew(J_NUM);
		n->num = d;
		return n;
	}
}

struct jval *json_parse(const char *s, size_t len)
{
	struct jparser jp;
	jp.p = s;
	jp.end = s + len;
	return jparse_value(&jp);
}

const char *json_get_str(const struct jval *o, const char *key, const char *dflt)
{
	size_t i;
	if (!o || o->type != J_OBJ)
		return dflt;
	for (i = 0; i < o->nkv; i++)
		if (!strcmp(o->keys[i], key))
			return o->vals[i]->type == J_STR ? o->vals[i]->str : dflt;
	return dflt;
}

i64 json_get_int(const struct jval *o, const char *key, i64 dflt)
{
	size_t i;
	if (!o || o->type != J_OBJ)
		return dflt;
	for (i = 0; i < o->nkv; i++)
		if (!strcmp(o->keys[i], key))
			return o->vals[i]->type == J_NUM ? (i64)o->vals[i]->num : dflt;
	return dflt;
}

size_t json_obj_len(const struct jval *o)
{
	return (o && o->type == J_OBJ) ? o->nkv : 0;
}

const char *json_obj_key(const struct jval *o, size_t i)
{
	return (o && o->type == J_OBJ && i < o->nkv) ? o->keys[i] : "";
}

const char *json_obj_val_str(const struct jval *o, size_t i)
{
	if (!o || o->type != J_OBJ || i >= o->nkv)
		return "";
	return o->vals[i]->type == J_STR ? o->vals[i]->str : "";
}

i64 json_obj_val_int(const struct jval *o, size_t i)
{
	if (!o || o->type != J_OBJ || i >= o->nkv)
		return 0;
	return o->vals[i]->type == J_NUM ? (i64)o->vals[i]->num : 0;
}

/* ------------------------------------------------------------------ */
/* writing                                                             */

void json_quote(struct buf *out, const char *s)
{
	buf_addch(out, '"');
	if (s) {
		for (; *s; s++) {
			unsigned char c = (unsigned char)*s;
			switch (c) {
			case '"':  buf_addstr(out, "\\\""); break;
			case '\\': buf_addstr(out, "\\\\"); break;
			case '\n': buf_addstr(out, "\\n"); break;
			case '\t': buf_addstr(out, "\\t"); break;
			case '\r': buf_addstr(out, "\\r"); break;
			case '\b': buf_addstr(out, "\\b"); break;
			case '\f': buf_addstr(out, "\\f"); break;
			default:
				if (c < 0x20)
					buf_addf(out, "\\u%04x", c);
				else
					buf_addch(out, c);   /* UTF-8 passes through */
			}
		}
	}
	buf_addch(out, '"');
}

static void json_quote_array(struct buf *out, char **items, size_t nr)
{
	size_t i;
	buf_addch(out, '[');
	for (i = 0; i < nr; i++) {
		if (i)
			buf_addch(out, ',');
		json_quote(out, items[i]);
	}
	buf_addch(out, ']');
}

/* ------------------------------------------------------------------ */
/* file parsing helpers                                                */

static void trim(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
		s[--n] = '\0';
}

static char *trim_left(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

/*
 * Read a frontmatter block.  Calls fn for each key/value pair.  Returns 1
 * if a block was found, and stores a pointer to the body in *body.
 */
static int read_frontmatter(const void *data, size_t len,
			    void (*fn)(const char *k, const char *v, void *),
			    void *ud, const char **body)
{
	const char *p = data, *end = p + len;
	int in_front = 0, seen = 0;

	*body = NULL;
	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		size_t llen = eol ? (size_t)(eol - p) : (size_t)(end - p);

		if (llen >= 3 && !memcmp(p, "---", 3) && llen <= 4) {
			if (!seen) {
				seen = 1;
				in_front = 1;
			} else {
				*body = eol ? eol + 1 : end;
				return 1;
			}
			p = eol ? eol + 1 : end;
			continue;
		}
		if (in_front) {
			const char *colon = memchr(p, ':', llen);
			if (colon) {
				char *key = xstrndup(p, (size_t)(colon - p));
				char *raw = xstrndup(colon + 1,
						     llen - (size_t)(colon - p) - 1);
				char *val = trim_left(raw);
				trim(key);
				trim(val);
				if (key[0])
					fn(key, val, ud);
				free(key);
				free(raw);
			}
		}
		p = eol ? eol + 1 : end;
	}
	return 0;
}

static void parse_inline_list(const char *s, char ***out, size_t *nr)
{
	const char *p = s;
	*out = NULL;
	*nr = 0;
	while (*p == ' ')
		p++;
	if (*p != '[')
		return;
	p++;
	for (;;) {
		const char *start;
		size_t n;
		while (*p == ' ')
			p++;
		if (*p == ']' || !*p)
			break;
		start = p;
		while (*p && *p != ',' && *p != ']')
			p++;
		n = (size_t)(p - start);
		while (n && start[n - 1] == ' ')
			n--;
		if (n) {
			*out = xrealloc(*out, (*nr + 1) * sizeof(char *));
			(*out)[(*nr)++] = xstrndup(start, n);
		}
		if (*p == ',') {
			p++;
			continue;
		}
		break;
	}
}

/* ------------------------------------------------------------------ */
/* prompts                                                             */

void prompt_release(struct prompt *p)
{
	size_t i;
	free(p->id);
	free(p->session);
	free(p->timestamp);
	free(p->author);
	free(p->model);
	for (i = 0; i < p->nr_tags; i++)
		free(p->tags[i]);
	free(p->tags);
	free(p->outcome);
	free(p->parent_prompt);
	for (i = 0; i < p->nr_attachments; i++)
		free(p->attachments[i]);
	free(p->attachments);
	free(p->path);
	free(p->body);
	memset(p, 0, sizeof *p);
}

void prompt_to_file(const struct prompt *p, struct buf *out)
{
	buf_reset(out);
	buf_addstr(out, "---\n");
	buf_addf(out, "id: %s\n", p->id ? p->id : "");
	buf_addf(out, "session: %s\n", (p->session && p->session[0])
					? p->session : "-");
	buf_addf(out, "seq: %d\n", p->seq);
	buf_addf(out, "timestamp: %s\n", p->timestamp ? p->timestamp : "");
	buf_addf(out, "author: %s\n", p->author ? p->author : "");
	if (p->model)
		buf_addf(out, "model: %s\n", p->model);
	if (p->nr_tags) {
		size_t i;
		buf_addstr(out, "tags: [");
		for (i = 0; i < p->nr_tags; i++)
			buf_addf(out, "%s%s", i ? ", " : "", p->tags[i]);
		buf_addstr(out, "]\n");
	}
	if (p->outcome)
		buf_addf(out, "outcome: %s\n", p->outcome);
	if (p->parent_prompt)
		buf_addf(out, "parent_prompt: %s\n", p->parent_prompt);
	if (p->nr_attachments) {
		size_t i;
		buf_addstr(out, "attachments: [");
		for (i = 0; i < p->nr_attachments; i++)
			buf_addf(out, "%s%s", i ? ", " : "", p->attachments[i]);
		buf_addstr(out, "]\n");
	}
	buf_addstr(out, "---\n");
	if (p->body)
		buf_addstr(out, p->body);
}

struct fm_ctx {
	struct prompt *p;
};

static void prompt_fm_kv(const char *key, const char *val, void *ud)
{
	struct fm_ctx *c = ud;
	struct prompt *p = c->p;

	if (!strcmp(key, "id"))
		p->id = xstrdup(val);
	else if (!strcmp(key, "session")) {
		if (strcmp(val, "-") && val[0])
			p->session = xstrdup(val);
	} else if (!strcmp(key, "seq"))
		p->seq = atoi(val);
	else if (!strcmp(key, "timestamp"))
		p->timestamp = xstrdup(val);
	else if (!strcmp(key, "author"))
		p->author = xstrdup(val);
	else if (!strcmp(key, "model") && val[0])
		p->model = xstrdup(val);
	else if (!strcmp(key, "outcome") && val[0])
		p->outcome = xstrdup(val);
	else if (!strcmp(key, "parent_prompt") && val[0])
		p->parent_prompt = xstrdup(val);
	else if (!strcmp(key, "tags"))
		parse_inline_list(val, &p->tags, &p->nr_tags);
	else if (!strcmp(key, "attachments"))
		parse_inline_list(val, &p->attachments, &p->nr_attachments);
}

/*
 * Returns 1 if the file carried a frontmatter block, 0 if it was just
 * prose.  Either way the body is filled in, so a hand-written file with no
 * metadata can still be recorded.
 */
int prompt_from_file(struct prompt *p, const void *data, size_t len)
{
	struct fm_ctx c;
	const char *body = NULL;
	int had;

	prompt_release(p);
	c.p = p;
	had = read_frontmatter(data, len, prompt_fm_kv, &c, &body);
	if (had && body)
		p->body = xstrndup(body, (size_t)((const char *)data + len - body));
	else if (had)
		p->body = xstrdup("");
	else
		p->body = xstrndup((const char *)data, len);
	if (p->timestamp)
		p->ts = parse_timestamp(p->timestamp);
	return had;
}

void prompt_to_json(const struct prompt *p, struct buf *out)
{
	buf_reset(out);
	buf_addch(out, '{');

	buf_addstr(out, "\"attachments\":");
	json_quote_array(out, p->attachments, p->nr_attachments);
	buf_addstr(out, ",\"author\":");
	json_quote(out, p->author ? p->author : "");
	buf_addstr(out, ",\"body\":");
	json_quote(out, p->body ? p->body : "");
	buf_addstr(out, ",\"id\":");
	json_quote(out, p->id ? p->id : "");
	buf_addstr(out, ",\"model\":");
	json_quote(out, p->model ? p->model : "");
	buf_addstr(out, ",\"outcome\":");
	json_quote(out, p->outcome ? p->outcome : "");
	buf_addstr(out, ",\"parent_prompt\":");
	json_quote(out, p->parent_prompt ? p->parent_prompt : "");
	buf_addstr(out, ",\"path\":");
	json_quote(out, p->path ? p->path : "");
	buf_addf(out, ",\"seq\":%d", p->seq);
	buf_addstr(out, ",\"session\":");
	json_quote(out, p->session ? p->session : "");
	buf_addstr(out, ",\"tags\":");
	json_quote_array(out, p->tags, p->nr_tags);
	buf_addstr(out, ",\"timestamp\":");
	json_quote(out, p->timestamp ? p->timestamp : "");

	buf_addch(out, '}');
}

/* ------------------------------------------------------------------ */
/* sessions                                                            */

void session_release(struct session *s)
{
	free(s->id);
	free(s->title);
	free(s->started_at);
	free(s->ended_at);
	free(s->author);
	free(s->notes);
	memset(s, 0, sizeof *s);
}

/*
 * A session is recorded the same way a prompt is: a small markdown file,
 * living beside the prompts so that it travels with them and is readable
 * without tooling.
 */
void session_to_file(const struct session *s, struct buf *out)
{
	buf_reset(out);
	buf_addstr(out, "---\n");
	buf_addf(out, "id: %s\n", s->id ? s->id : "");
	buf_addf(out, "title: %s\n", s->title ? s->title : "");
	buf_addf(out, "started_at: %s\n", s->started_at ? s->started_at : "");
	if (s->ended_at)
		buf_addf(out, "ended_at: %s\n", s->ended_at);
	buf_addf(out, "author: %s\n", s->author ? s->author : "");
	if (s->notes)
		buf_addf(out, "notes: %s\n", s->notes);
	buf_addstr(out, "---\n");
}

static void session_fm_kv(const char *key, const char *val, void *ud)
{
	struct session *s = ud;

	if (!strcmp(key, "id"))
		s->id = xstrdup(val);
	else if (!strcmp(key, "title"))
		s->title = xstrdup(val);
	else if (!strcmp(key, "started_at"))
		s->started_at = xstrdup(val);
	else if (!strcmp(key, "ended_at") && val[0])
		s->ended_at = xstrdup(val);
	else if (!strcmp(key, "author"))
		s->author = xstrdup(val);
	else if (!strcmp(key, "notes") && val[0])
		s->notes = xstrdup(val);
}

int session_from_file(struct session *s, const void *data, size_t len)
{
	const char *body = NULL;
	int had;

	session_release(s);
	had = read_frontmatter(data, len, session_fm_kv, s, &body);
	if (s->started_at)
		s->started_ts = parse_timestamp(s->started_at);
	return had;
}

void session_to_json(const struct session *s, struct buf *out)
{
	buf_reset(out);
	buf_addch(out, '{');
	buf_addstr(out, "\"author\":");
	json_quote(out, s->author ? s->author : "");
	buf_addstr(out, ",\"ended_at\":");
	json_quote(out, s->ended_at ? s->ended_at : "");
	buf_addstr(out, ",\"id\":");
	json_quote(out, s->id ? s->id : "");
	buf_addstr(out, ",\"notes\":");
	json_quote(out, s->notes ? s->notes : "");
	buf_addstr(out, ",\"started_at\":");
	json_quote(out, s->started_at ? s->started_at : "");
	buf_addstr(out, ",\"title\":");
	json_quote(out, s->title ? s->title : "");
	buf_addch(out, '}');
}

/* ------------------------------------------------------------------ */
/* ids and slugs                                                       */

/*
 * Ids only have to be unique within a repository, so a hash of the clock,
 * the process id and a counter is plenty -- and needs no platform
 * randomness source.
 */
static u64 id_counter = 0;

static void id_rand_bytes(u8 *out, size_t n)
{
	struct buf seed;
	u8 digest[GP_SHA1_RAWSZ];
	size_t i = 0;

	buf_init(&seed);
	buf_addf(&seed, "%lld/%ld/%llu", (long long)now_epoch(), gp_getpid(),
		 (unsigned long long)(++id_counter));
	gp_sha1(seed.b, seed.len, digest);
	buf_release(&seed);
	while (i < n) {
		size_t chunk = n - i;
		if (chunk > GP_SHA1_RAWSZ)
			chunk = GP_SHA1_RAWSZ;
		memcpy(out + i, digest, chunk);
		i += chunk;
		if (i < n)
			gp_sha1(digest, GP_SHA1_RAWSZ, digest);
	}
}

char *new_prompt_id(void)
{
	static const char alpha[] = "abcdefghijklmnopqrstuvwxyz0123456789";
	u8 rnd[8];
	char out[11];
	int i;

	id_rand_bytes(rnd, sizeof rnd);
	memcpy(out, "p_", 2);
	for (i = 0; i < 8; i++)
		out[2 + i] = alpha[rnd[i] % (sizeof alpha - 1)];
	out[10] = '\0';
	return xstrdup(out);
}

char *new_session_id(void)
{
	static const char alpha[] = "abcdefghijklmnopqrstuvwxyz0123456789";
	u8 rnd[6];
	char suffix[8];
	int i;

	id_rand_bytes(rnd, sizeof rnd);
	for (i = 0; i < 6; i++)
		suffix[i] = alpha[rnd[i] % (sizeof alpha - 1)];
	suffix[6] = '\0';
	return xstrfmt("s_%lld_%s", (long long)now_epoch(), suffix);
}

/*
 * A filename-safe rendering of a prompt's first words: lower case, runs of
 * anything else collapsed to a single dash, trimmed, truncated.
 */
char *slugify(const char *text, size_t maxlen)
{
	struct buf b;
	const char *p;
	int last_dash = 0;
	char *s;

	buf_init(&b);
	for (p = text; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if (isalnum(c)) {
			buf_addch(&b, tolower(c));
			last_dash = 0;
		} else if (!last_dash && b.len) {
			buf_addch(&b, '-');
			last_dash = 1;
		}
		if (b.len >= maxlen)
			break;
	}
	while (b.len && b.b[b.len - 1] == '-')
		b.len--;
	if (b.b)
		b.b[b.len] = '\0';
	s = b.b ? xstrdup((const char *)b.b) : xstrdup("");
	buf_release(&b);
	return s;
}
