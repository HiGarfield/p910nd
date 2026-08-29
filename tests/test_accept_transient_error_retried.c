/*
 * Regression test for Bug: server()'s accept() loop killed the daemon on any
 * error that was not EINTR/ECONNABORTED or a resource-shortage errno.
 *
 * The defect
 * ----------
 * The loop was:
 *
 *      if (errno == EINTR || errno == ECONNABORTED)
 *              continue;
 *      if (errno == EMFILE || errno == ENFILE ||
 *          errno == ENOBUFS || errno == ENOMEM) {
 *              ... accept_backoff(); continue;
 *      }
 *      break;                  <-- everything else falls through to exit(1)
 *
 * so every remaining errno -- including the network-oriented ones a listening
 * socket reports while the network is flapping (ENETDOWN, ENETUNREACH,
 * EHOSTUNREACH, EHOSTDOWN, ENONET) and protocol-level hiccups (ECONNRESET,
 * EPROTO) -- tore the daemon down.  The listening socket stays bound through
 * such a fault and the network comes back, but nothing is served any more
 * because the process is gone.  That breaks the requirement that temporary
 * network faults must be handled.
 *
 * The fix
 * -------
 * accept_error_is_fatal() names the only errors retrying can never repair
 * (the listening descriptor is closed, is not a socket, is not listening, or
 * its protocol has no accept()).  Everything else logs (rate-limited), waits
 * in accept_backoff() -- which blocks in select() and therefore burns no CPU
 * -- and keeps serving.
 *
 * What this test proves
 * ---------------------
 * 1. Classification: unit assertions on accept_error_is_fatal() (a pure
 *    function, platform independent).
 * 2. End to end, with a deterministically injected transient accept() error:
 *    accept() is made to fail with ENETDOWN a few times before the real one
 *    runs, and the daemon must still complete a real job -- proven by the job
 *    bytes appearing in the printer device file.  Pre-fix the daemon exits on
 *    the very first ENETDOWN and the device file stays empty.
 *    The injection is a compile-time interposition of the accept() identifier
 *    in this translation unit only; it touches no production code and behaves
 *    identically on every platform.
 * 3. Regression guard: the same job with no injected error still works, and a
 *    fatal accept() error still terminates the daemon (so a genuinely broken
 *    listening socket is not silently retried forever).
 */
#define _GNU_SOURCE

/*
 * <sys/socket.h> must be seen BEFORE `accept` is redefined, otherwise the
 * header's own declaration of accept() would be renamed too and clash with
 * the static interposer below.
 */
#include <sys/socket.h>
#include <sys/types.h>

#define main p910nd_original_main

static int my_accept(int s, struct sockaddr *addr, socklen_t *len);

#define accept my_accept
#include "../p910nd.c"
#undef accept
#undef main

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define FIRST_PORT 9100
#define LAST_PORT 9109
#define JOB "HELLO-PRINTER-AFTER-TRANSIENT-ACCEPT-ERROR"

/* Injection state; only the child process ever runs the interposer. */
static int g_accept_errno = 0;   /* errno to report, 0 = do not fail */
static int g_accept_left = 0;    /* how many more calls should fail */

static int my_accept(int s, struct sockaddr *addr, socklen_t *len)
{
	if (g_accept_left > 0)
	{
		g_accept_left--;
		errno = g_accept_errno;
		return -1;
	}
	/* Parenthesised so the object-like macro above cannot expand here. */
	return (accept)(s, addr, len);
}

static char g_devpath[256];
static pid_t g_child = -1;

static void reap_child(void)
{
	if (g_child > 0)
	{
		int status;
		(void)kill(g_child, SIGKILL);
		(void)waitpid(g_child, &status, 0);
		g_child = -1;
	}
}

/* Pick a port in 9100..9109 that nothing is listening on. */
static int pick_free_port(void)
{
	int port;
	for (port = FIRST_PORT; port <= LAST_PORT; port++)
	{
		int s;
		int one = 1;
		struct sockaddr_in sa;

		s = socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0)
			continue;
		(void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons((unsigned short)port);
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) == 0)
		{
			(void)close(s);
			return port;
		}
		(void)close(s);
	}
	return -1;
}

/* Create and truncate the printer device file.  open_printer() opens it with
 * a plain O_WRONLY (no O_CREAT), so the file must already exist -- otherwise
 * the daemon would sit in its ten-second open retry loop. */
static void reset_device(void)
{
	int fd = open(g_devpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
	{
		perror("open devpath");
		exit(1);
	}
	(void)close(fd);
}

/* Read the whole printer device file into buf; returns the byte count. */
static size_t slurp(const char *path, char *buf, size_t cap)
{
	FILE *f = fopen(path, "r");
	size_t n = 0;
	if (f == NULL)
		return 0;
	n = fread(buf, 1, cap, f);
	fclose(f);
	return n;
}

/*
 * Run one job through a real server() child.
 *
 * fail_errno / fail_count: injected accept() failures before the real accept.
 * Returns 1 when the job bytes reached the printer device.
 */
static int run_job(int port, int fail_errno, int fail_count)
{
	pid_t child;
	int sv[2] = {-1, -1};
	int i;
	char got[512];
	size_t n;

	reset_device();

	child = fork();
	assert(child >= 0);
	if (child == 0)
	{
		(void)signal(SIGALRM, SIG_DFL);
		(void)alarm(120);
		g_accept_errno = fail_errno;
		g_accept_left = fail_count;
		bindaddr = (char *)"127.0.0.1";
		device = g_devpath;
		log_to_stdout = 0;
		server((int)('0' + (port - BASEPORT)));
		_exit(0);
	}
	g_child = child;

	/* Give the daemon time to bind and reach accept(). */
	(void)sleep(1);

	for (i = 0; i < 80; i++)
	{
		struct sockaddr_in sa;
		int c = socket(AF_INET, SOCK_STREAM, 0);
		if (c < 0)
			break;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons((unsigned short)port);
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (connect(c, (struct sockaddr *)&sa, sizeof(sa)) == 0)
		{
			sv[0] = c;
			break;
		}
		(void)close(c);
		(void)usleep(250000);
	}
	if (sv[0] < 0)
	{
		reap_child();
		return 0;
	}
	{
		size_t off = 0;
		const char *job = JOB;
		size_t len = strlen(job);
		while (off < len)
		{
			ssize_t w = write(sv[0], job + off, len - off);
			if (w < 0)
			{
				if (errno == EINTR)
					continue;
				break;
			}
			off += (size_t)w;
		}
		(void)shutdown(sv[0], SHUT_WR);
	}

	for (i = 0; i < 80; i++)
	{
		n = slurp(g_devpath, got, sizeof(got) - 1);
		if (n >= strlen(JOB))
			break;
		(void)usleep(250000);
	}
	got[n < sizeof(got) ? n : sizeof(got) - 1] = '\0';
	(void)close(sv[0]);
	(void)sv[1];
	reap_child();
	if (n < strlen(JOB))
	{
		fprintf(stderr, "  (printer device holds %lu bytes, expected %lu)\n",
		        (unsigned long)n, (unsigned long)strlen(JOB));
		return 0;
	}
	return memcmp(got, JOB, strlen(JOB)) == 0;
}

static void test_classification(void)
{
	/* Transient network / protocol errors: must be retried, not fatal. */
	assert(accept_error_is_fatal(ENETDOWN) == 0);
	assert(accept_error_is_fatal(ENETUNREACH) == 0);
	assert(accept_error_is_fatal(EHOSTUNREACH) == 0);
	assert(accept_error_is_fatal(EHOSTDOWN) == 0);
	assert(accept_error_is_fatal(ECONNRESET) == 0);
	assert(accept_error_is_fatal(ECONNABORTED) == 0);
	assert(accept_error_is_fatal(EINTR) == 0);
	assert(accept_error_is_fatal(EAGAIN) == 0);
	/* Resource shortages: retried (with backoff) by the caller. */
	assert(accept_error_is_fatal(EMFILE) == 0);
	assert(accept_error_is_fatal(ENFILE) == 0);
	assert(accept_error_is_fatal(ENOBUFS) == 0);
	assert(accept_error_is_fatal(ENOMEM) == 0);

	/* Only errors retrying can never repair are fatal. */
	assert(accept_error_is_fatal(EBADF) == 1);
	assert(accept_error_is_fatal(ENOTSOCK) == 1);
	assert(accept_error_is_fatal(EINVAL) == 1);
	assert(accept_error_is_fatal(EOPNOTSUPP) == 1);
	assert(accept_error_is_fatal(ENOPROTOOPT) == 1);

	fprintf(stderr, "PASS: accept() error classification is correct\n");
}

int main(void)
{
	int port;

	(void)signal(SIGPIPE, SIG_IGN);
	(void)alarm(180);

	(void)snprintf(g_devpath, sizeof(g_devpath), "/tmp/p910nd-accept-test-%ld.dev",
	               (long)getpid());

	test_classification();

	port = pick_free_port();
	if (port < 0)
	{
		fprintf(stderr, "FAIL: no free port in %d..%d\n", FIRST_PORT, LAST_PORT);
		return 1;
	}

	/* Regression guard: an undisturbed daemon still completes the job. */
	if (!run_job(port, 0, 0))
	{
		fprintf(stderr, "FAIL: baseline job (no injected error) did not complete\n");
		(void)remove(g_devpath);
		return 1;
	}
	fprintf(stderr, "PASS: baseline job completes with no accept() error\n");

	/* The bug: a transient network error on accept() must not kill the
	 * daemon -- it must retry and then serve the very same connection. */
	if (!run_job(port, ENETDOWN, 3))
	{
		fprintf(stderr,
		        "FAIL: daemon did not survive a transient accept() error "
		        "(ENETDOWN); it must retry and serve the pending connection\n");
		(void)remove(g_devpath);
		return 1;
	}
	fprintf(stderr,
	        "PASS: daemon retried after transient accept() errors and served the job\n");

	/*
	 * A fatal accept() error must still terminate the daemon, so a genuinely
	 * broken listening socket is not retried forever.
	 */
	{
		pid_t child = fork();
		int status;
		int exited = 0;
		int i;

		assert(child >= 0);
		if (child == 0)
		{
			(void)signal(SIGALRM, SIG_DFL);
			(void)alarm(60);
			g_accept_errno = ENOTSOCK;
			g_accept_left = 1000000; /* fail forever */
			bindaddr = (char *)"127.0.0.1";
			device = g_devpath;
			log_to_stdout = 0;
			server((int)('0' + (port - BASEPORT)));
			_exit(0);
		}
		g_child = child;
		for (i = 0; i < 80 && !exited; i++)
		{
			struct sockaddr_in sa;
			int c = socket(AF_INET, SOCK_STREAM, 0);
			if (c >= 0)
			{
				memset(&sa, 0, sizeof(sa));
				sa.sin_family = AF_INET;
				sa.sin_port = htons((unsigned short)port);
				sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				(void)connect(c, (struct sockaddr *)&sa, sizeof(sa));
				(void)close(c);
			}
			if (waitpid(child, &status, WNOHANG) == child)
				exited = 1;
			else
				(void)usleep(250000);
		}
		if (exited)
			reap_child();
		else
			g_child = -1;
		reap_child();
		if (!exited)
		{
			fprintf(stderr,
			        "FAIL: a fatal accept() error (ENOTSOCK) did not stop the daemon\n");
			(void)remove(g_devpath);
			return 1;
		}
	}
	fprintf(stderr, "PASS: a fatal accept() error still stops the daemon\n");

	(void)remove(g_devpath);
	fprintf(stderr,
	        "PASS: transient accept() errors are retried, fatal ones are not\n");
	return 0;
}
