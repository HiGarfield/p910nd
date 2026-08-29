/*
 * Regression test for Bug: a job never completes when the printer stops
 * accepting data after the last byte was already delivered.
 *
 * The defect
 * ----------
 * `eof_sent` -- the flag that ends a job -- was ONLY raised inside
 * writeBuffer(), and both copy loops only call writeBuffer() once select()
 * has reported the printer writable:
 *
 *   - unidirectional: want_write = (bytes > 0 || eof_read), so after the
 *     network EOF has been seen the loop still waits for the printer even
 *     when the buffer is empty; and
 *   - bidirectional:  prepBuffer() arms writefds[outfd] whenever
 *     `bytes != 0 || eof_read`, i.e. also on a bare eof_read.
 *
 * But once `eof_read && bytes == 0` every byte that was read has already been
 * handed to the printer (bytes == 0 implies totalin == totalout), so there is
 * nothing left to write and the writability of the printer is irrelevant.
 * A printer that goes offline, is unplugged, or whose driver stops reporting
 * POLLOUT right after the last byte was accepted therefore makes the daemon
 * wait for an event that can never arrive:
 *
 *   - unidirectional: select() with no timeout blocks forever;
 *   - bidirectional:  the 100 ms select() timeout turns it into an endless
 *                     poll loop.
 *
 * Either way copy_stream() never returns, the client connection is held open
 * forever, and -- because server() accepts and serves one connection at a
 * time -- no other client can ever be served again.  That violates the
 * requirement that a permanent printer fault must not wedge the daemon.
 *
 * The fix
 * -------
 * mark_eof_if_drained() raises eof_sent as soon as `eof_read && bytes == 0`,
 * independently of the output's writability.  It is called from writeBuffer()
 * (preserving the existing "drained by this write" completion) and directly
 * by both copy loops.
 *
 * What this test proves
 * ---------------------
 * 1. Reproduce the exact state: a printer that has accepted data and is now
 *    completely full, i.e. permanently unwritable.  The precondition is
 *    *asserted*, not assumed (select() must report "not writable"), so the
 *    test cannot silently degenerate into a no-op on another platform.
 * 2. A zero-byte job (client connects, sends nothing, shuts its send side)
 *    must complete in BOTH unidirectional and bidirectional mode.  Pre-fix
 *    both hang, so the alarm() backstop fires and the test fails.
 * 3. Regression guard: a normal 64 KiB job through a draining printer is
 *    still delivered byte-for-byte and copy_stream() still returns 0.  This
 *    proves eof_sent is not being raised early (which would truncate jobs).
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>

#define JOB_BYTES 65536

static pid_t g_client = -1;

static void on_alarm(int sig)
{
	(void)sig;
	if (g_client > 0)
		(void)kill(g_client, SIGKILL);
	fprintf(stderr,
	        "FAIL: copy_stream() did not return while the printer was "
	        "permanently unwritable (job hung)\n");
	_exit(1);
}

/* Write to a non-blocking pipe until it refuses: it is now completely full
 * and, because nobody ever reads it, permanently unwritable. */
static void fill_pipe(int fd)
{
	static char chunk[4096];
	for (;;)
	{
		ssize_t n = write(fd, chunk, sizeof(chunk));
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			assert(errno == EAGAIN || errno == EWOULDBLOCK);
			return;
		}
	}
}

/* Assert the printer really is unwritable: without this the scenario would
 * not be reproduced and the test could pass for the wrong reason. */
static void assert_printer_unwritable(int fd)
{
	fd_set w;
	struct timeval tv;
	int rc;

	FD_ZERO(&w);
	FD_SET(fd, &w);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	rc = select(fd + 1, NULL, &w, NULL, &tv);
	if (rc != 0)
	{
		fprintf(stderr, "FAIL: precondition not met, printer fd=%d is still "
		                "writable (select rc=%d)\n",
		        fd, rc);
		exit(1);
	}
}

/*
 * Zero-byte job against a printer that is already completely full.
 * Returns the copy_stream() result; hangs forever (aborted by alarm()) if
 * job completion still depends on the printer becoming writable.
 */
static void test_zero_byte_job(int use_bidir)
{
	int net_sv[2];
	int prn[2];
	pid_t client;
	int rc;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
	assert(pipe(prn) == 0);
	assert(fcntl(prn[1], F_SETFL, O_NONBLOCK) == 0);
	fill_pipe(prn[1]);
	assert_printer_unwritable(prn[1]);

	client = fork();
	assert(client >= 0);
	if (client == 0)
	{
		(void)signal(SIGALRM, SIG_DFL);
		(void)alarm(60);
		(void)close(net_sv[0]);
		(void)close(prn[0]);
		(void)close(prn[1]);
		/* A job with no data at all, then EOF. */
		(void)shutdown(net_sv[1], SHUT_WR);
		for (;;)
			(void)pause();
	}
	g_client = client;
	(void)close(net_sv[1]);

	bidir = use_bidir;
	log_to_stdout = 0;
	rc = copy_stream(net_sv[0], prn[1]);

	if (rc != 0)
	{
		fprintf(stderr, "FAIL: zero-byte job (bidir=%d) returned %d\n",
		        use_bidir, rc);
		(void)kill(client, SIGKILL);
		(void)waitpid(client, NULL, 0);
		exit(1);
	}
	(void)kill(client, SIGKILL);
	(void)waitpid(client, NULL, 0);
	g_client = -1;
	(void)close(net_sv[0]);
	(void)close(prn[0]);
	(void)close(prn[1]);
	fprintf(stderr, "PASS: zero-byte job completes with an unwritable printer (bidir=%d)\n",
	        use_bidir);
}

/* Regression guard: a real job must still be delivered byte-for-byte and must
 * still complete normally, i.e. eof_sent is not raised prematurely. */
static void test_full_job_still_delivered(void)
{
	int net_sv[2];
	int prn[2];
	pid_t client, sink;
	char data[JOB_BYTES];
	FILE *f;
	size_t i, n;
	int rc;

	for (i = 0; i < sizeof(data); i++)
		data[i] = (char)(i * 31 + 7);

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
	assert(pipe(prn) == 0);

	f = tmpfile();
	assert(f != NULL);

	client = fork();
	assert(client >= 0);
	if (client == 0)
	{
		size_t off = 0;
		(void)signal(SIGALRM, SIG_DFL);
		(void)alarm(60);
		(void)close(net_sv[0]);
		(void)close(prn[0]);
		(void)close(prn[1]);
		while (off < sizeof(data))
		{
			ssize_t w = write(net_sv[1], data + off, sizeof(data) - off);
			if (w < 0)
			{
				if (errno == EINTR)
					continue;
				_exit(2);
			}
			off += (size_t)w;
		}
		(void)shutdown(net_sv[1], SHUT_WR);
		for (;;)
			(void)pause();
	}

	sink = fork();
	assert(sink >= 0);
	if (sink == 0)
	{
		(void)signal(SIGALRM, SIG_DFL);
		(void)alarm(60);
		(void)close(net_sv[0]);
		(void)close(net_sv[1]);
		(void)close(prn[1]);
		for (;;)
		{
			char buf[8192];
			ssize_t r = read(prn[0], buf, sizeof(buf));
			if (r < 0)
			{
				if (errno == EINTR)
					continue;
				_exit(3);
			}
			if (r == 0)
				break;
			if (fwrite(buf, 1, (size_t)r, f) != (size_t)r)
				_exit(4);
		}
		(void)fflush(f);
		_exit(0);
	}

	(void)close(net_sv[1]);
	(void)close(prn[0]);

	bidir = 0;
	log_to_stdout = 0;
	rc = copy_stream(net_sv[0], prn[1]);

	(void)kill(client, SIGKILL);
	(void)waitpid(client, NULL, 0);
	(void)close(net_sv[0]);
	(void)close(prn[1]);
	(void)waitpid(sink, NULL, 0);

	if (rc != 0)
	{
		fprintf(stderr, "FAIL: full job returned %d\n", rc);
		exit(1);
	}
	rewind(f);
	{
		char got[JOB_BYTES];
		n = fread(got, 1, sizeof(got), f);
		if (n != sizeof(data) || memcmp(got, data, sizeof(data)) != 0)
		{
			fprintf(stderr, "FAIL: full job truncated (%lu of %lu bytes)\n",
			        (unsigned long)n, (unsigned long)sizeof(data));
			exit(1);
		}
	}
	fclose(f);
	fprintf(stderr, "PASS: %lu-byte job still delivered byte-for-byte\n",
	        (unsigned long)JOB_BYTES);
}

int main(void)
{
	/*
	 * Backstop for the hang.  The bidirectional zero-byte job legitimately
	 * costs the full IDLE_TIMEOUT_SEC grace window (the daemon keeps a
	 * bidirectional stream open for a late printer response), so the budget
	 * is derived from that, not hard-coded.
	 */
	(void)signal(SIGPIPE, SIG_IGN);
	(void)signal(SIGALRM, on_alarm);
	(void)alarm((unsigned)(2 * (IDLE_TIMEOUT_SEC + 15)));

	test_zero_byte_job(0);
	test_zero_byte_job(1);
	test_full_job_still_delivered();

	fprintf(stderr, "PASS: jobs complete even when the printer never becomes "
	                "writable again\n");
	return 0;
}
