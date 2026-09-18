/*
 * Regression test for BUG-014 (clue 2): the printer->network direction must
 * not be paced by the 100 ms read throttle once the host has finished sending
 * the job (network EOF).  Before the fix, every printer read armed a 100 ms
 * timer that cleared the printer read fd, capping the return stream at roughly
 * BUFFER_SIZE per 100 ms (~80 KB/s) and needlessly slowing large status / PJL /
 * SNMP replies.
 *
 * A pty cannot show this (its own flow control is the binding constraint), so
 * we use a socketpair for the "printer": with no flow control the daemon reads
 * up to BUFFER_SIZE per call, and the throttle -- if still armed -- is the only
 * thing pacing the response.  The daemon side runs in a child (copy_stream_ex);
 * the parent feeds the job and a 200 KB response and times delivery.
 *
 * Mutation check: reverting the "if (!networkToPrinterBuffer.eof_read)" guard so
 * the timer is always armed makes a 200 KB post-EOF response take several
 * seconds here, so the < 2 s bound below fails.
 */
#define main p910nd_main
#include "../p910nd.c"
#undef main

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static double now_sec(void)
{
	struct timeval tv;
	(void)gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

int main(void)
{
	int ns[2], ps[2];
	pid_t pid;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, ns) < 0)
	{
		perror("socketpair ns");
		return 1;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, ps) < 0)
	{
		perror("socketpair ps");
		return 1;
	}

	pid = fork();
	if (pid < 0)
	{
		perror("fork");
		return 1;
	}
	if (pid == 0)
	{
		/* Daemon side: serve the job over the socketpairs. */
		(void)close(ns[1]);
		(void)close(ps[1]);
		bidir = 1;
		idle_timeout = 100;	/* keep the idle bound out of the way */
		{
			int fd_closed = 0, lp_closed = 0;
			(void)copy_stream_ex(ns[0], ps[0], &fd_closed, &lp_closed);
		}
		_exit(0);
	}

	/* Parent side: act as both the network peer and the printer peer. */
	(void)close(ns[0]);
	(void)close(ps[0]);

	{
		char job[200];
		int i;
		for (i = 0; i < (int)sizeof(job); ++i)
			job[i] = (char)i;

		if (write(ns[1], job, sizeof(job)) != (ssize_t)sizeof(job))
		{
			perror("write job");
			return 1;
		}
		/* Host finished sending: signal EOF on the network direction. */
		(void)shutdown(ns[1], SHUT_WR);

		/* Drain the job the daemon wrote to the printer side. */
		{
			char buf[4096];
			int got = 0;
			while (got < (int)sizeof(job))
			{
				ssize_t n = read(ps[1], buf, sizeof(buf));
				if (n <= 0)
					break;
				got += (int)n;
			}
			if (got != (int)sizeof(job))
			{
				fprintf(stderr, "job not echoed to printer: %d/%d\n", got,
						(int)sizeof(job));
				return 1;
			}
		}

		{
			const int RESP = 200 * 1024;
			unsigned char *resp = malloc((size_t)RESP);
			unsigned char *rcv = malloc((size_t)RESP);
			int wrem = RESP, rrem = RESP;
			double t0, t1;
			fd_set rd, wr;
			int maxfd = (ns[1] > ps[1]) ? ns[1] : ps[1];

			if (resp == NULL || rcv == NULL)
			{
				perror("malloc");
				return 1;
			}
			for (i = 0; i < RESP; ++i)
				resp[i] = (unsigned char)(i % 251);

			t0 = now_sec();
			for (;;)
			{
				int ready;
				FD_ZERO(&rd);
				FD_ZERO(&wr);
				if (rrem > 0)
					FD_SET(ns[1], &rd);
				if (wrem > 0)
					FD_SET(ps[1], &wr);
				if (rrem == 0 && wrem == 0)
					break;
				ready = select(maxfd + 1, &rd, &wr, NULL, NULL);
				if (ready < 0)
				{
					perror("select");
					return 1;
				}
				if (wrem > 0 && FD_ISSET(ps[1], &wr))
				{
					ssize_t n = write(ps[1], resp + (RESP - wrem),
									 (size_t)wrem);
					if (n > 0)
						wrem -= (int)n;
					else if (n < 0 && errno != EAGAIN)
						break;
				}
				if (rrem > 0 && FD_ISSET(ns[1], &rd))
				{
					ssize_t n = read(ns[1], rcv + (RESP - rrem),
									(size_t)rrem);
					if (n > 0)
						rrem -= (int)n;
					else if (n < 0 && errno != EAGAIN)
						break;
				}
			}
			/* Signal printer EOF so the daemon finishes promptly. */
			(void)shutdown(ps[1], SHUT_WR);
			t1 = now_sec();

			if (rrem != 0 || memcmp(resp, rcv, (size_t)RESP) != 0)
			{
				fprintf(stderr, "response mismatch: %d/%d bytes\n",
						RESP - rrem, RESP);
				return 1;
			}
			if (t1 - t0 > 2.0)
			{
				fprintf(stderr,
						"post-EOF printer->network too slow: %.2fs "
						"(throttle still armed after network EOF?)\n",
						t1 - t0);
				return 1;
			}
			fprintf(stderr,
					"post-EOF 200 KB response forwarded in %.2fs\n",
					t1 - t0);
			free(resp);
			free(rcv);
		}
	}

	{
		int status;
		(void)waitpid(pid, &status, 0);
	}
	return 0;
}
