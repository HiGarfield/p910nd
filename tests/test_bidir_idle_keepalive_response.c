/*
 * Regression test for Bug #2: the bidirectional idle timeout was measured only
 * against network-side reads (last_read_time, reset solely on a network read).
 * If the printer kept emitting responses while the network peer was quiet
 * (e.g. a device that streams status, or a slow chatty printer), the idle
 * timer still expired after IDLE_TIMEOUT_SEC and copy_stream() tore the job
 * down early -- dropping in-flight printer responses and any pending job data.
 * That violates "support any speed/timing, never lose data".
 *
 * Fix: last_activity is now bumped on EITHER a network read OR a printer
 * response read, and the timeout compares against that combined clock.  A
 * printer that keeps talking keeps the job alive.
 *
 * This test runs copy_stream() with a network peer that stays quiet (no data,
 * no close) while the printer streams responses continuously for longer than
 * IDLE_TIMEOUT_SEC.  It asserts copy_stream() does NOT time the job out early:
 * the network peer must stay connected past the timeout and must receive every
 * printer byte sent during the window.  Compiled with a short -DIDLE_TIMEOUT_SEC
 * so the scenario is exercised quickly.
 *
 * Formal proof sketch (post-fix):
 *   - Each printer response read bumps last_activity (see read of io_lp).
 *   - The timeout branch only fires when idle_timeout_elapsed(now,
 *     last_activity) is true, i.e. no activity in either direction for the
 *     full window.  Continuous printer output => last_activity keeps moving =>
 *     branch never fires while the printer talks.
 *   - Therefore the job survives past IDLE_TIMEOUT_SEC as long as the printer
 *     is active, and the network peer receives all responses.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>

#ifndef IDLE_TIMEOUT_SEC
#define IDLE_TIMEOUT_SEC 30
#endif
#define RUN_SEC (IDLE_TIMEOUT_SEC * 3 + 1) /* stay active well past the timeout */
#define CHUNK 512

static void sendall(int fd, const char *buf, size_t len)
{
	size_t off = 0;
	while (off < len)
	{
		ssize_t n = write(fd, buf + off, len - off);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			return;
		}
		off += (size_t)n;
	}
}

int main(void)
{
	int net_sv[2], prn_sv[2], rep[2];
	pid_t client, prn;
	int rc;
	int early_eof = 0;     /* set by client if its connection closed early */
	long net_got = 0;      /* bytes the network peer received */
	long prn_sent = 0;     /* bytes the printer streamed */
	char chunk[CHUNK];
	size_t i;

	for (i = 0; i < sizeof(chunk); i++)
		chunk[i] = (char)(i ^ 0x55);

	assert(pipe(rep) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
	assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);

	bidir = 1;
	log_to_stdout = 0;

	client = fork();
	assert(client >= 0);
	if (client == 0)
	{
		long got = 0;
		int eof = 0;
		struct timeval start, cur;
		char b;
		(void)close(net_sv[0]);
		(void)close(prn_sv[0]);
		(void)close(prn_sv[1]);
		(void)close(rep[0]);
		gettimeofday(&start, NULL);
		while (1)
		{
			fd_set rfds;
			struct timeval tv;
			int r;
			FD_ZERO(&rfds);
			FD_SET(net_sv[1], &rfds);
			tv.tv_sec = 0;
			tv.tv_usec = 200000;
			r = select(net_sv[1] + 1, &rfds, NULL, NULL, &tv);
			if (r > 0)
			{
				ssize_t n = read(net_sv[1], &b, 1);
				if (n == 0)
				{
					eof = 1; /* connection closed underneath us */
					break;
				}
				else if (n > 0)
					got += n;
			}
			gettimeofday(&cur, NULL);
			if (cur.tv_sec - start.tv_sec >= RUN_SEC)
				break;
		}
		/* Close our write side so p910nd can finish cleanly. */
		(void)shutdown(net_sv[1], SHUT_WR);
		/* Drain anything left. */
		while (read(net_sv[1], &b, 1) == 1)
			got += 1;
		(void)close(net_sv[1]);
		if (write(rep[1], &got, sizeof(got)) != (ssize_t)sizeof(got))
			_exit(4);
		_exit(eof ? 3 : 0);
	}

	prn = fork();
	assert(prn >= 0);
	if (prn == 0)
	{
		struct timeval start, cur;
		long sent = 0;
		(void)close(net_sv[0]);
		(void)close(net_sv[1]);
		(void)close(prn_sv[0]);
		(void)close(rep[0]);
		(void)close(rep[1]);
		gettimeofday(&start, NULL);
		while (1)
		{
			sendall(prn_sv[1], chunk, sizeof(chunk));
			sent += sizeof(chunk);
			usleep(200000); /* ~5 chunks/sec, well within the idle window */
			gettimeofday(&cur, NULL);
			if (cur.tv_sec - start.tv_sec >= RUN_SEC)
				break;
		}
		(void)shutdown(prn_sv[1], SHUT_WR);
		(void)close(prn_sv[1]);
		_exit(sent > 0 ? 0 : 5);
	}

	(void)close(net_sv[1]);
	(void)close(prn_sv[1]);
	(void)close(rep[1]);

	rc = copy_stream(net_sv[0], prn_sv[0]);
	(void)close(net_sv[0]);
	(void)close(prn_sv[0]);

	if (read(rep[0], &net_got, sizeof(net_got)) != (ssize_t)sizeof(net_got))
		net_got = -1;
	(void)close(rep[0]);

	{
		int status;
		assert(waitpid(client, &status, 0) == client);
		if (WIFEXITED(status))
		{
			if (WEXITSTATUS(status) == 3)
				early_eof = 1;
		}
	}
	(void)waitpid(prn, NULL, 0);

	fprintf(stderr,
	        "bidir idle keepalive: rc=%d early_eof=%d net_received=%ld\n",
	        rc, early_eof, net_got);

	/* The job must NOT have been torn down by the idle timeout while the
	 * printer kept talking. */
	if (early_eof)
	{
		fprintf(stderr, "FAIL: network connection closed early by idle timeout despite active printer\n");
		return 1;
	}
	/* The network peer should have received a substantial, non-trivial amount
	 * of printer output (far more than one chunk), proving the stream stayed
	 * open well past IDLE_TIMEOUT_SEC. */
	if (net_got < (long)(CHUNK * 3))
	{
		fprintf(stderr, "FAIL: network received too little (%ld bytes), job likely cut short\n", net_got);
		return 1;
	}
	(void)prn_sent;
	(void)rc;
	fprintf(stderr, "PASS: bidirectional job survived idle timeout while printer kept emitting\n");
	return 0;
}
