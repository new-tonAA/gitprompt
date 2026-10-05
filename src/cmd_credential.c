/*
 * credential -- the credential helper protocol, as git defines it.
 *
 * A credential is a handful of `key=value` lines on stdin, ended by a blank
 * line: protocol, host, path, username, password.  `fill` answers with what it
 * could find -- the same keys -- while `approve` says the answer worked and
 * `reject` says it did not.
 *
 * The searching is done by helpers: programs named by `credential.helper`,
 * which may be named more than once and are then a list.  Each is run with the
 * operation as its argument and the credential on its stdin, and what it
 * writes back is read the same way -- protocol, host, path, username, password
 * -- with a later helper the later word on a field and the search stopping as
 * soon as there is both a username and a password.
 *
 * The helper comes in the three shapes git gives it: `!` and a shell line, a
 * path, or a bare name that means `git credential-<name>`.  The bare name is
 * what makes the ordinary `store`, `cache` and `manager` work, and it is a
 * call to git rather than to gitprompt on purpose: those helpers are git's,
 * and a machine that has a helper configured has git there to run it.
 *
 * Why this is here at all: gitprompt hands the authentication for an https or
 * ssh remote to git itself (see run_git in cmd_remote.c), so this is not what
 * makes a push work.  It is what lets a helper a user has already configured
 * be driven from gitprompt, and what lets `gitprompt credential fill` be the
 * other end of one.
 */
#include "gp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define cred_pid() ((long)_getpid())
#else
#include <unistd.h>
#define cred_pid() ((long)getpid())
#endif

struct cred {
	char *protocol;
	char *host;
	char *path;
	char *username;
	char *password;
};

static void cred_release(struct cred *c)
{
	free(c->protocol);
	free(c->host);
	free(c->path);
	free(c->username);
	free(c->password);
	memset(c, 0, sizeof *c);
}

static void cred_put(char **slot, const char *value)
{
	free(*slot);
	*slot = xstrdup(value);
}

/*
 * The stream is `key=value` lines and the blank line that ends it belongs to
 * the protocol rather than to the credential, so reading stops there; a stream
 * that ends without one is read to the end.  Keys this does not know are
 * passed over rather than refused: a helper may be handed more than gitprompt
 * has a use for, and guessing what a `wwwauth[]` is for would be worse than
 * leaving it.
 */
static void cred_parse_line(struct cred *c, const char *line, size_t len)
{
	const char *eq;
	char *key, *value;

	while (len && (line[len - 1] == '\r' || line[len - 1] == '\n'))
		len--;
	eq = memchr(line, '=', len);
	if (!eq)
		return;
	key = xstrndup(line, (size_t)(eq - line));
	value = xstrndup(eq + 1, len - (size_t)(eq - line) - 1);

	if (!strcmp(key, "protocol"))
		cred_put(&c->protocol, value);
	else if (!strcmp(key, "host"))
		cred_put(&c->host, value);
	else if (!strcmp(key, "path"))
		cred_put(&c->path, value);
	else if (!strcmp(key, "username"))
		cred_put(&c->username, value);
	else if (!strcmp(key, "password"))
		cred_put(&c->password, value);
	free(key);
	free(value);
}

static void cred_read_stream(struct cred *c, const char *data, size_t len)
{
	size_t i = 0;

	while (i < len) {
		size_t start = i, end;

		while (i < len && data[i] != '\n')
			i++;
		end = i;
		if (i < len)
			i++;
		if (end == start || (end == start + 1 && data[start] == '\r'))
			break;
		cred_parse_line(c, data + start, end - start);
	}
}

static int cred_read_stdin(struct cred *c)
{
	struct buf b;
	char chunk[4096];
	size_t n;
	int rc = 0;

	buf_init(&b);
	while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0)
		buf_add(&b, chunk, n);
	if (ferror(stdin))
		rc = -1;
	else
		cred_read_stream(c, b.b ? (const char *)b.b : "", b.len);
	buf_release(&b);
	return rc;
}

/*
 * The credential as it goes to a helper: the fields that are known, in the
 * order git writes them, and nothing else -- there is no blank line at the
 * end, because the end of the stream is the end of the request.
 */
static void cred_format(const struct cred *c, struct buf *out)
{
	if (c->protocol)
		buf_addf(out, "protocol=%s\n", c->protocol);
	if (c->host)
		buf_addf(out, "host=%s\n", c->host);
	if (c->path)
		buf_addf(out, "path=%s\n", c->path);
	if (c->username)
		buf_addf(out, "username=%s\n", c->username);
	if (c->password)
		buf_addf(out, "password=%s\n", c->password);
}

static const char *cred_temp_dir(void)
{
	const char *d = getenv("TMPDIR");

	if (!d || !*d)
		d = getenv("TEMP");
	if (!d || !*d)
		d = getenv("TMP");
	if (!d || !*d)
		d = "/tmp";
	return d;
}

/*
 * One helper, one operation.
 *
 * The command is written into a script and that script is run, rather than the
 * command being handed to `sh -c`.  On Windows the only way to a shell is a
 * command line, and there every word is split before sh sees it: the C
 * runtime's quoting is not the quoting cmd.exe and msys sh agree on, so
 * `sh -c "prog arg"` arrives as `sh -c prog` and the rest is lost.  A space is
 * not a corner case here -- the temporary directory usually has one -- so the
 * command is put somewhere there is nothing to split, and the one word left on
 * the command line is the script's own path, quoted for whichever shell reads
 * it.
 *
 * The credential is put in a file and the helper's stdin taken from it, because
 * a pipe is open in one direction only: `popen` either gives a program its
 * input or reads its output, never both.  The helper's answer comes back the
 * second way.  Both files are named for the process so two runs do not walk on
 * each other, and both are removed at the end.
 *
 * A helper that fails is not fatal.  git moves on to the next one, and reads
 * whatever it wrote on the way out; a helper saying nothing about a credential
 * that is not for it is how it says so.
 */
static void cred_helper_run(const char *helper, const char *op,
			    const struct cred *in, struct buf *out)
{
	struct buf body, script, cmd;
	char *infile, *scriptpath;
	const char *tdir = cred_temp_dir();
	FILE *f;

	/*
	 * The paths go into the script inside single quotes and onto the
	 * command line inside double ones, so a temporary directory holding
	 * either quote, a percent or a newline cannot be written down safely.
	 * Refused rather than misquoted.
	 */
	if (strpbrk(tdir, "'\"%\r\n")) {
		gp_error("cannot use '%s' as a temporary directory", tdir);
		return;
	}

	infile = xstrfmt("%s/gitprompt-credential-%ld.in", tdir, cred_pid());
	scriptpath = xstrfmt("%s/gitprompt-credential-%ld.sh", tdir, cred_pid());

	buf_init(&body);
	cred_format(in, &body);
	if (write_file(infile, body.b, body.len) < 0) {
		gp_error("cannot write %s", infile);
		buf_release(&body);
		free(infile);
		free(scriptpath);
		return;
	}
	buf_release(&body);

	/*
	 * The command is `<helper> <operation>`, which is what git builds: an
	 * `!` announces a shell line and the rest of the value is used as it
	 * stands, a path is used as it stands, and anything else is a name for
	 * `git credential-<name>`.  Only the credential's own path is quoted,
	 * and with single quotes so the shell cannot take a `$` or a backtick
	 * in a temporary directory for something to expand.
	 */
	buf_init(&script);
	if (helper[0] == '!')
		buf_addstr(&script, helper + 1);
	else if (strpbrk(helper, "/\\"))
		buf_addstr(&script, helper);
	else {
		buf_addstr(&script, "git credential-");
		buf_addstr(&script, helper);
	}
	buf_addf(&script, " %s < '%s'\n", op, infile);
	if (write_file(scriptpath, script.b, script.len) < 0) {
		gp_error("cannot write %s", scriptpath);
		buf_release(&script);
		remove(infile);
		free(infile);
		free(scriptpath);
		return;
	}
	buf_release(&script);

	buf_init(&cmd);
#ifdef _WIN32
	buf_addf(&cmd, "sh \"%s\"", scriptpath);
#else
	buf_addf(&cmd, "sh '%s'", scriptpath);
#endif
	f = popen(buf_cstr(&cmd), "r");
	buf_release(&cmd);
	if (f) {
		char chunk[4096];
		size_t n;

		while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
			buf_add(out, chunk, n);
		pclose(f);
	}

	remove(infile);
	remove(scriptpath);
	free(infile);
	free(scriptpath);
}

static void cred_write(const struct cred *c)
{
	struct buf b;

	buf_init(&b);
	cred_format(c, &b);
	if (b.len)
		fwrite(b.b, 1, b.len, stdout);
	buf_release(&b);
}

/* An operation that is not one, and no operation at all, are the same answer. */
static void cred_usage(void)
{
	gp_error("usage: gitprompt credential fill\n"
		 "   credential approve\n"
		 "   credential reject");
}

int cmd_credential(struct repo *r, int argc, char **argv)
{
	struct opts o;
	struct slist helpers = SLIST_INIT;
	struct cred c;
	const char *op;
	size_t i;

	opts_init(&o, argc, argv, (const char *const[]){ NULL });
	memset(&c, 0, sizeof c);

	if (o.nargs != 1) {
		cred_usage();
		return 129;
	}
	op = opts_arg(&o, 0);
	if (strcmp(op, "fill") && strcmp(op, "approve") && strcmp(op, "reject")) {
		cred_usage();
		return 129;
	}

	if (cred_read_stdin(&c) < 0) {
		gp_error("cannot read the credential from stdin");
		cred_release(&c);
		return 1;
	}

	repo_config_get_all(r, "credential.helper", &helpers);

	if (!strcmp(op, "fill")) {
		/*
		 * A credential that already has both halves is not asked
		 * about: there is nothing to fill in, and a helper that gave
		 * a different password would be answering a question nobody
		 * asked.
		 */
		if (!(c.username && c.password)) {
			for (i = 0; i < helpers.nr; i++) {
				struct buf answer;

				buf_init(&answer);
				cred_helper_run(helpers.v[i], "get", &c, &answer);
				cred_read_stream(&c,
						 answer.b ? (const char *)answer.b : "",
						 answer.len);
				buf_release(&answer);
				if (c.username && c.password)
					break;
			}
		}
		if (!c.password) {
			/*
			 * git asks the user here.  gitprompt does not: a
			 * command that stops for a password on a terminal it
			 * was not given is a command that hangs where nobody
			 * is watching, which is worse for a tool whose job is
			 * to run unattended.
			 */
			gp_error("no credential helper supplied a password for %s%s%s",
				 c.protocol ? c.protocol : "the remote",
				 c.host ? "://" : "", c.host ? c.host : "");
			slist_release(&helpers);
			cred_release(&c);
			return 1;
		}
		cred_write(&c);
	} else {
		/* a helper is told `store` when the answer worked and `erase`
		 * when it did not, and what it says back is not read */
		const char *helper_op = strcmp(op, "approve") ? "erase" : "store";

		for (i = 0; i < helpers.nr; i++) {
			struct buf ignored;

			buf_init(&ignored);
			cred_helper_run(helpers.v[i], helper_op, &c, &ignored);
			buf_release(&ignored);
		}
	}

	slist_release(&helpers);
	cred_release(&c);
	return 0;
}
