/*
 * Regression test: in unidirectional mode a hard network read error (TCP
 * RST -> ECONNRESET) used to make copy_stream() bail out immediately via
 * "goto out", discarding the bytes that had already been read from the
 * network but not yet written to the printer.  With a fast network and a
 * slow printer the daemon's internal buffer holds several KiB of pending
 * data at the moment the peer resets, so the job was silently truncated.
 *
 * Fix: a hard read error no longer abandons the copy.  readBuffer() marks
 * eof_read, and the unidirectional loop keeps draining the already-buffered
 * bytes to the printer (exactly like the bidirectional drain logic) before
 * exiting.  When every read byte reaches the printer the lingering error
 * flag is cleared on the exit path and copy_stream() returns 0, so a job
 * that was fully delivered is not misreported as failed.
 *
 * Triggering a *hard* read error deterministically follows the same recipe
 * as the bidirectional counterpart: a TCP listener with a small receive
 * window so the client keeps unacknowledged data in flight, and a
 * SO_LINGER-0 close that therefore sends RST rather than a clean FIN.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <signal.h>

#define JOB_BYTES (3 * BUFFER_SIZE) /* 24576: exceeds the 8192 internal buffer */

int main(void)
{
	int lfd, netfd, prn_sv[2], rep[2];
	pid_t client, prn;
	struct sockaddr_in addr;
	struct linger ling;
	char data[JOB_BYTES];
	long received = -1;
	double elapsed = -1.0;
	int sz, one = 1;
	size_t i;
	socklen_t alen;

	for (i = 0; i < sizeof(data); i++)
		data[i] = (char)(i * 11 + 1);

	assert(pipe(rep) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
	/* Slow printer: small send buffer + non-blocking so the daemon's
	 * writes hit EAGAIN/partial writes while it consumes slowly. */
	sz = 2048;
	assert(setsockopt(prn_sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz)) == 0);
	assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);

	/* TCP listener with a small receive window so the client keeps
	 * unacknowledged data in flight and SO_LINGER-0 close sends RST. */
	assert((lfd = socket(AF_INET, SOCK_STREAM, 0)) >= 0);
	assert(setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
	/* Large receive window so the client's whole job is acknowledged and
	 * buffered by the kernel before the reset; the RST then cannot discard
	 * bytes the daemon has already received, isolating the bug we exercise:
	 * bytes the daemon has *read into its internal buffer* but not yet
	 * *written to the printer* at the moment read() returns ECONNRESET. */
	sz = 131072;
	assert(setsockopt(lfd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz)) == 0);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	assert(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	assert(listen(lfd, 4) == 0);
	alen = sizeof(addr);
	assert(getsockname(lfd, (struct sockaddr *)&addr, &alen) == 0);

	(void)alarm(120);

	bidir = 0;
	log_to_stdout = 0;

	client = fork();
	assert(client >= 0);
	if (client == 0)
	{
		size_t off = 0;
		int cfd;
		(void)close(lfd);
		(void)close(prn_sv[0]);
		(void)close(prn_sv[1]);
		(void)close(rep[0]);
		(void)close(rep[1]);
		assert((cfd = socket(AF_INET, SOCK_STREAM, 0)) >= 0);
		if (connect(cfd, (struct sockaddr *)&addr, alen) < 0)
		{
			perror("connect");
			_exit(2);
		}
		while (off < sizeof(data))
		{
			ssize_t n = write(cfd, data + off, sizeof(data) - off);
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				perror("client write");
				_exit(2);
			}
			off += (size_t)n;
		}
		/* Let the daemon pull data into its internal buffer (and let the
		 * slow printer fall behind), then reset so the very next read is
		 * ECONNRESET while several KiB are still pending in the buffer. */
		(void)usleep(300000);
		ling.l_onoff = 1;
		ling.l_linger = 0;
		(void)setsockopt(cfd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
		(void)close(cfd);
		_exit(0);
	}

	prn = fork();
	assert(prn >= 0);
	if (prn == 0)
	{
		char b;
		long got = 0;
		(void)close(lfd);
		(void)close(prn_sv[0]);
		(void)close(rep[0]);
		for (;;)
		{
			ssize_t r = read(prn_sv[1], &b, 1);
			if (r < 0)
			{
				if (errno == EINTR)
					continue;
				perror("consumer read");
				_exit(3);
			}
			if (r == 0)
				break;
			got += 1;
			/* ~1250 B/s: draining ~24 KiB takes ~20 s, well under the
			 * 120 s alarm.  Slow enough that the daemon's internal buffer
			 * holds several KiB when the peer resets, exposing the
			 * read-error-before-drain path. */
			(void)usleep(800);
		}
		if (write(rep[1], &got, sizeof(got)) != (ssize_t)sizeof(got))
			_exit(4);
		_exit(0);
	}

	(void)close(prn_sv[1]);
	(void)close(rep[1]);
	netfd = accept(lfd, NULL, NULL);
	assert(netfd >= 0);
	(void)close(lfd);

	{
		struct timeval t0, t1;
		int rc;
		(void)gettimeofday(&t0, NULL);
		rc = copy_stream(netfd, prn_sv[0]);
		(void)gettimeofday(&t1, NULL);
		elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
		(void)rc;
		fprintf(stderr, "copy_stream rc=%d in %.1f s (unidir, hard read error)\n", rc, elapsed);
		if (rc != 0)
		{
			fprintf(stderr, "FAIL: copy_stream returned %d on a fully delivered job\n", rc);
			return 1;
		}
	}

	(void)close(netfd);
	(void)close(prn_sv[0]);
	if (read(rep[0], &received, sizeof(received)) != (ssize_t)sizeof(received))
		received = -1;
	(void)close(rep[0]);

	(void)waitpid(client, NULL, 0);
	(void)waitpid(prn, NULL, 0);

	fprintf(stderr, "printer received %ld of %d job bytes after unidir network read error\n",
		received, JOB_BYTES);
	if (received != (long)JOB_BYTES)
	{
		fprintf(stderr, "FAIL: job truncated to %ld bytes (data lost)\n", received);
		return 1;
	}
	fprintf(stderr, "PASS: unidir buffered data fully drained to printer after network read error\n");
	return 0;
}
