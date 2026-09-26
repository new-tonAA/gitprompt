/*
 * net.c - sockets, and the sliver of HTTP that `serve` and `gp://` are built
 * on.
 *
 * gitprompt's own transport is deliberately not git's wire protocol.  It does
 * not have to be: the store is an ordinary git object store, so the exchange it
 * needs is the one the local transport already performs -- read the other
 * side's refs, copy across the objects that are missing here.  All this file
 * adds is somewhere to carry those bytes.
 *
 * The framing is HTTP with a Content-Length, which is the smallest thing both
 * ends can agree on without a library.  Bodies of unknown length are never sent
 * and never read, so there is no chunked encoding, no streaming, and no
 * compression; one request is answered and the connection is closed.  That is
 * every case gitprompt has, and refusing to guess at the rest keeps this small
 * enough to read in one sitting.
 *
 * The same server also answers git's *dumb* HTTP protocol -- /info/refs,
 * /HEAD, /objects/... -- because a store that is a real git object store should
 * be clonable by the real git, and it costs a few lines to hand out the objects
 * that are already here.
 */
#include "gp.h"

#ifdef _WIN32
/*
 * winsock2.h before windows.h, always: windows.h pulls in the older winsock.h
 * when it gets there first, and the two declare the socket functions
 * differently, so the second one included is the one that fails to compile.
 * WIN32_LEAN_AND_MEAN keeps windows.h from reaching for it at all.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#endif

#define GP_DEFAULT_PORT 9418

static int net_ready;

int net_init(void)
{
	if (net_ready)
		return 0;
#ifdef _WIN32
	{
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			gp_error("cannot start Windows sockets");
			return -1;
		}
	}
#else
	/*
	 * A write to a socket whose far end has gone raises SIGPIPE, and the
	 * default action for it is to kill the process -- a server that dies
	 * because a client hung up mid-response would be a server that stops
	 * answering everyone.  The write fails on its own; that is enough.
	 */
	signal(SIGPIPE, SIG_IGN);
#endif
	net_ready = 1;
	return 0;
}

int net_strerror(char *buf, size_t n)
{
	if (!n)
		return -1;
	buf[0] = '\0';
#ifdef _WIN32
	{
		DWORD err = (DWORD)WSAGetLastError();
		DWORD len = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
						   FORMAT_MESSAGE_IGNORE_INSERTS,
					   NULL, err, 0, buf, (DWORD)n, NULL);
		/* the system message arrives with a line ending on it */
		while (len && (buf[len - 1] == '\r' || buf[len - 1] == '\n' ||
			       buf[len - 1] == '.'))
			buf[--len] = '\0';
		if (!len)
			snprintf(buf, n, "socket error %lu",
				 (unsigned long)err);
	}
#else
	snprintf(buf, n, "%s", strerror(errno));
#endif
	return 0;
}

static void report(const char *what)
{
	char msg[256];

	net_strerror(msg, sizeof msg);
	gp_error("%s: %s", what, msg);
}

/* an IPv4 address, from a literal or from the resolver */
static int resolve4(const char *host, struct in_addr *out)
{
	unsigned long addr = inet_addr(host);
	struct hostent *he;

	if (addr != INADDR_NONE) {
		out->s_addr = (u32)addr;
		return 0;
	}
	he = gethostbyname(host);
	if (!he || he->h_length != 4 || !he->h_addr_list || !he->h_addr_list[0])
		return -1;
	memcpy(out, he->h_addr_list[0], 4);
	return 0;
}

static const char *host_for_message(const char *host)
{
	return (host && *host && strcmp(host, "any")) ? host : "127.0.0.1";
}

gp_socket net_listen(const char *host, int port, int *got_port)
{
	gp_socket fd;
	struct sockaddr_in sa;
	struct in_addr addr;
	int on = 1;
	socklen_t optlen = (socklen_t)sizeof on;

	if (net_init() < 0)
		return GP_SOCKET_INVALID;

	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	if (!host || !*host || !strcmp(host, "loopback")) {
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	} else if (!strcmp(host, "any")) {
		sa.sin_addr.s_addr = htonl(INADDR_ANY);
	} else if (resolve4(host, &addr) < 0) {
		gp_error("cannot resolve '%s'", host);
		return GP_SOCKET_INVALID;
	} else {
		sa.sin_addr = addr;
	}

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == GP_SOCKET_INVALID) {
		report("socket");
		return GP_SOCKET_INVALID;
	}
	/* so restarting the server does not have to wait for the old socket */
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, optlen);
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		char msg[256];
		net_strerror(msg, sizeof msg);
		gp_error("cannot listen on %s:%d: %s", host_for_message(host),
			 port, msg);
		net_close(fd);
		return GP_SOCKET_INVALID;
	}
	if (listen(fd, 16) < 0) {
		report("listen");
		net_close(fd);
		return GP_SOCKET_INVALID;
	}

	if (got_port) {
		struct sockaddr_in got;
		socklen_t glen = (socklen_t)sizeof got;

		/* the port asked for, unless it was 0 and the system chose */
		*got_port = port;
		if (getsockname(fd, (struct sockaddr *)&got, &glen) == 0)
			*got_port = ntohs(got.sin_port);
	}
	return fd;
}

gp_socket net_accept(gp_socket listener)
{
	gp_socket fd = accept(listener, NULL, NULL);

	if (fd == GP_SOCKET_INVALID)
		return GP_SOCKET_INVALID;
	/*
	 * A client that connects and then says nothing must not hold the one
	 * thread the server has forever.  Nothing gitprompt sends is large
	 * enough for this to be a real limit.
	 */
#ifdef _WIN32
	{
		DWORD ms = 30000;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms,
			   (int)sizeof ms);
	}
#else
	{
		struct timeval tv;
		tv.tv_sec = 30;
		tv.tv_usec = 0;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv,
			   (socklen_t)sizeof tv);
	}
#endif
	return fd;
}

void net_close(gp_socket fd)
{
	if (fd == GP_SOCKET_INVALID)
		return;
#ifdef _WIN32
	closesocket(fd);
#else
	close(fd);
#endif
}

gp_socket net_connect(const char *host, int port)
{
	gp_socket fd;
	struct sockaddr_in sa;
	struct in_addr addr;

	if (net_init() < 0)
		return GP_SOCKET_INVALID;

	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	if (resolve4(host, &addr) < 0) {
		gp_error("cannot resolve '%s'", host);
		return GP_SOCKET_INVALID;
	}
	sa.sin_addr = addr;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == GP_SOCKET_INVALID) {
		report("socket");
		return GP_SOCKET_INVALID;
	}
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		char msg[256];
		net_strerror(msg, sizeof msg);
		gp_error("cannot connect to %s:%d: %s", host, port, msg);
		net_close(fd);
		return GP_SOCKET_INVALID;
	}
	return fd;
}

static int write_all(gp_socket fd, const void *data, size_t len)
{
	const u8 *p = data;
	size_t sent = 0;

	while (sent < len) {
		size_t left = len - sent;
		int n;

		if (left > (size_t)0x7fffffff)
			left = (size_t)0x7fffffff;
		n = send(fd, (const char *)p + sent, (int)left, 0);
		if (n <= 0)
			return -1;
		sent += (size_t)n;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* reading a request                                                   */

void http_in_init(struct http_in *in, gp_socket fd)
{
	memset(in, 0, sizeof *in);
	in->fd = fd;
}

static int in_fill(struct http_in *in)
{
	int n;

	if (in->pos < in->len)
		return 1;
	in->pos = in->len = 0;
	n = recv(in->fd, (char *)in->buf, (int)sizeof in->buf, 0);
	if (n <= 0)
		return 0;
	in->len = (size_t)n;
	return 1;
}

int http_get(struct http_in *in)
{
	if (!in_fill(in))
		return -1;
	return in->buf[in->pos++];
}

size_t http_read(struct http_in *in, void *dst, size_t n)
{
	u8 *p = dst;
	size_t got = 0;

	while (got < n) {
		size_t avail;

		if (!in_fill(in))
			break;
		avail = in->len - in->pos;
		if (avail > n - got)
			avail = n - got;
		memcpy(p + got, in->buf + in->pos, avail);
		in->pos += avail;
		got += avail;
	}
	return got;
}

int http_line(struct http_in *in, struct buf *out)
{
	int c, any = 0;

	buf_reset(out);
	while ((c = http_get(in)) >= 0) {
		any = 1;
		if (c == '\n') {
			/* CRLF and bare LF both end a line */
			if (out->len && out->b[out->len - 1] == '\r')
				out->len--;
			return 0;
		}
		buf_addch(out, c);
	}
	return any ? 0 : -1;
}

static int ci_prefix(const char *s, const char *prefix)
{
	size_t i;

	for (i = 0; prefix[i]; i++) {
		char a = s[i];
		char b = prefix[i];

		if (a >= 'A' && a <= 'Z')
			a = (char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z')
			b = (char)(b - 'A' + 'a');
		if (a != b)
			return 0;
	}
	return 1;
}

int http_read_request(struct http_in *in, struct buf *method, struct buf *path,
		      struct buf *headers, size_t *content_length)
{
	struct buf line = BUF_INIT;
	char *sp;
	int rc = -1;

	if (content_length)
		*content_length = 0;
	buf_reset(method);
	buf_reset(path);
	if (headers)
		buf_reset(headers);

	if (http_line(in, &line) < 0 || !line.len)
		goto out;

	/* METHOD SP TARGET SP VERSION */
	sp = strchr(buf_cstr(&line), ' ');
	if (!sp)
		goto out;
	buf_add(method, line.b, (size_t)(sp - (char *)line.b));
	{
		const char *p = sp + 1;
		const char *end = strchr(p, ' ');
		size_t n = end ? (size_t)(end - p) : strlen(p);

		buf_add(path, p, n);
	}

	for (;;) {
		char *colon;

		if (http_line(in, &line) < 0 || !line.len)
			break;
		if (headers) {
			buf_add(headers, line.b, line.len);
			buf_addch(headers, '\n');
		}
		colon = strchr(buf_cstr(&line), ':');
		if (colon && content_length &&
		    ci_prefix(buf_cstr(&line), "content-length:"))
			*content_length = (size_t)strtoul(colon + 1, NULL, 10);
	}

	/* the query string is not part of the path anything is routed on */
	{
		char *q = memchr(path->b, '?', path->len);

		if (q)
			path->len = (size_t)(q - (char *)path->b);
	}
	rc = 0;
out:
	buf_release(&line);
	return rc;
}

static const char *status_text(int status)
{
	switch (status) {
	case 200: return "OK";
	case 400: return "Bad Request";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 409: return "Conflict";
	case 411: return "Length Required";
	case 413: return "Payload Too Large";
	default:  return "Error";
	}
}

int http_respond(gp_socket fd, int status, const char *ctype, const void *body,
		 size_t len)
{
	struct buf head = BUF_INIT;
	int rc = -1;

	buf_addf(&head, "HTTP/1.0 %d %s\r\n", status, status_text(status));
	buf_addf(&head, "Content-Type: %s\r\n",
		 ctype ? ctype : "text/plain; charset=utf-8");
	buf_addf(&head, "Content-Length: %llu\r\n",
		 (unsigned long long)len);
	/* one request per connection, so the client knows where the body ends */
	buf_addstr(&head, "Connection: close\r\n\r\n");
	if (write_all(fd, head.b, head.len) == 0 &&
	    (len == 0 || write_all(fd, body, len) == 0))
		rc = 0;
	buf_release(&head);
	return rc;
}

/* ------------------------------------------------------------------ */
/* the client side                                                     */

int gp_url_parse(const char *url, struct gp_url *out)
{
	const char *p, *slash;
	size_t hostlen;

	memset(out, 0, sizeof *out);
	out->port = GP_DEFAULT_PORT;
	out->path[0] = '/';

	if (!strncmp(url, "gp://", 5))
		p = url + 5;
	else if (!strncmp(url, "gp:", 3))
		p = url + 3;
	else {
		gp_error("'%s' is not a gp:// url", url);
		return -1;
	}
	if (!*p) {
		gp_error("gp://: no host; the form is gp://host[:port][/path]");
		return -1;
	}

	slash = strchr(p, '/');
	hostlen = slash ? (size_t)(slash - p) : strlen(p);
	{
		const char *colon = memchr(p, ':', hostlen);

		if (colon) {
			size_t n = (size_t)(colon - p);
			const char *portstr = colon + 1;

			if (!n || n >= sizeof out->host) {
				gp_error("gp://: the host name is empty or too long");
				return -1;
			}
			memcpy(out->host, p, n);
			out->host[n] = '\0';
			out->port = atoi(portstr);
			if (out->port <= 0 || out->port > 65535) {
				gp_error("gp://%s: '%s' is not a port", out->host,
					 portstr);
				return -1;
			}
		} else {
			if (!hostlen || hostlen >= sizeof out->host) {
				gp_error("gp://: the host name is empty or too long");
				return -1;
			}
			memcpy(out->host, p, hostlen);
			out->host[hostlen] = '\0';
		}
	}

	if (slash) {
		size_t plen = strlen(slash);

		if (plen >= sizeof out->path) {
			gp_error("gp://%s: the path is too long", out->host);
			return -1;
		}
		memcpy(out->path, slash, plen + 1);
		/* one trailing slash, so paths can be joined without checking */
		if (plen > 1 && out->path[plen - 1] == '/')
			out->path[plen - 1] = '\0';
	}
	return 0;
}

int http_request(const struct gp_url *u, const char *method, const char *path,
		 const void *body, size_t len, int *status, struct buf *body_out)
{
	gp_socket fd;
	struct buf req = BUF_INIT, line = BUF_INIT;
	struct http_in in;
	size_t clen = 0;
	int code = 0, rc = -1;

	if (status)
		*status = 0;
	if (body_out)
		buf_reset(body_out);

	fd = net_connect(u->host, u->port);
	if (fd == GP_SOCKET_INVALID)
		return -1;

	buf_addf(&req, "%s %s HTTP/1.0\r\n", method, path);
	buf_addf(&req, "Host: %s:%d\r\n", u->host, u->port);
	buf_addf(&req, "Content-Length: %llu\r\n", (unsigned long long)len);
	buf_addstr(&req, "Connection: close\r\n\r\n");
	if (len)
		buf_add(&req, body, len);

	if (write_all(fd, req.b, req.len) < 0) {
		report("cannot send the request");
		buf_release(&req);
		net_close(fd);
		return -1;
	}
	buf_release(&req);

	http_in_init(&in, fd);

	/* HTTP/1.x <code> <reason> */
	do {
		if (http_line(&in, &line) < 0)
			goto out;
	} while (!line.len);
	{
		char *sp = strchr(buf_cstr(&line), ' ');

		if (sp)
			code = (int)strtol(sp + 1, NULL, 10);
	}
	if (!code) {
		gp_error("the server at %s:%d did not answer with HTTP",
			 u->host, u->port);
		goto out;
	}

	for (;;) {
		char *v;

		if (http_line(&in, &line) < 0)
			goto out;
		if (!line.len)
			break;
		v = strchr(buf_cstr(&line), ':');
		if (v && ci_prefix(buf_cstr(&line), "content-length:"))
			clen = (size_t)strtoul(v + 1, NULL, 10);
	}

	if (status)
		*status = code;
	if (body_out) {
		size_t left = clen;
		u8 chunk[8192];

		while (left) {
			size_t want = left < sizeof chunk ? left : sizeof chunk;
			size_t got = http_read(&in, chunk, want);

			if (!got)
				break;
			buf_add(body_out, chunk, got);
			left -= got;
		}
	}
	rc = 0;
out:
	buf_release(&line);
	net_close(fd);
	return rc;
}
