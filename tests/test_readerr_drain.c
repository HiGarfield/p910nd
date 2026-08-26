/*
 * Regression test: after a hard network read error (TCP RST -> ECONNRESET),
 * copy_stream() must keep draining data already buffered to the printer
 * instead of abandoning it at a fixed 10 s wall-clock deadline.
 *
 * Bug: the old code did
 *     if ((err & READ_ERR) && now.tv_sec - last_read_time.tv_sec >= 10)
 *         break;
 * With a slow printer the 10 s deadline fired while several KiB were
 * still waiting in the daemon's internal buffer, truncating the job.
 *
 * The fix leaves the loop only once the buffer is fully drained
 * (bytes == 0); select() keeps blocking meanwhile, so no CPU is burned.
 *
 * Triggering a *hard* read error deterministically:
 *  - The network side is real TCP; only TCP can deliver ECONNRESET.
 *  - SO_LINGER-0 close sends RST only while unacknowledged data exists;
 *    once everything is ACKed it degrades to a clean FIN (EOF path,
 *    which never exercises READ_ERR).
 *  - The listener's SO_RCVBUF is therefore shrunk so the advertised
 *    window (~4 KiB) is smaller than the 32 KiB job.  The client can
 *    only push 4 KiB at a time; after the daemon has pulled ~12.8 KiB
 *    into its buffers the client still has ~20 KiB unacknowledged in
 *    flight, so SO_LINGER-0 close provably sends RST.  The daemon's
 *    receive queue is empty by then, so the very next read returns
 *    ECONNRESET (no data comes before the error).
 *
 * At RST time the daemon holds 8192 B (BUFFER_SIZE) internally plus
 * 4608 B in the printer socket.  The printer consumes at ~330 B/s:
 *  - draining 8192+4608 = 12800 B takes ~38 s;
 *  - the buggy 10 s deadline fires right after the first 4608 B block
 *    is written, abandoning the remaining 3584 B -> 9216 B total,
 *    ~14 s elapsed;
 *  - the fixed code drains everything -> 12800 B, ~28 s elapsed.
 * So assertions received >= 11000 && elapsed >= 20 s separate them.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <netinet/in.h>

#define SEND_BYTES (4 * BUFFER_SIZE) /* 32768: exceeds the ~4 KiB TCP window */

int main(void)
{
	int lfd, netfd, prn_sv[2], rep[2];
	pid_t client, prn;
	struct sockaddr_in addr;
	struct linger ling;
	char data[SEND_BYTES];
	long received = -1;
	double elapsed = -1.0;
	int sz, one = 1;
	size_t i;
	socklen_t alen;

	for (i = 0; i < sizeof(data); i++)
		data[i] = (char)(i * 11 + 1);

	assert(pipe(rep) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);

	/* Shrink the printer-side socket buffer so the daemon's writes hit
	 * EAGAIN/partial writes while the printer consumes slowly. */
	sz = 2048;
	assert(setsockopt(prn_sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz)) == 0);
	assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);

	/* TCP listener with a small receive window (inherited by the
	 * accepted socket) so the client keeps unacknowledged data in
	 * flight and SO_LINGER-0 close really sends RST. */
	assert((lfd = socket(AF_INET, SOCK_STREAM, 0)) >= 0);
	assert(setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
	sz = 2048;
	assert(setsockopt(lfd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz)) == 0);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	assert(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	assert(listen(lfd, 4) == 0);
	alen = sizeof(addr);
	assert(getsockname(lfd, (struct sockaddr *)&addr, &alen) == 0);

	/* Bounds the test: the fixed code drains the last block in ~38 s. */
	(void)alarm(45);

	bidir = 1;
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
		/* Let the daemon pull data into its buffers, but keep the bulk
		 * unacknowledged so the reset below is a real RST. */
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
			/* ~390 B/s actual throughput: consuming 4608 B takes ~12 s,
			 * so the buggy 10 s deadline still fires mid-drain, while the
			 * whole ~14 KiB drain (measured ~37 s) keeps a comfortable
			 * margin under the 45 s alarm. */
			(void)usleep(2200);
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
		(void)gettimeofday(&t0, NULL);
		(void)copy_stream(netfd, prn_sv[0]);
		(void)gettimeofday(&t1, NULL);
		elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
	}

	(void)close(netfd);
	(void)close(prn_sv[0]);
	if (read(rep[0], &received, sizeof(received)) != (ssize_t)sizeof(received))
		received = -1;
	(void)close(rep[0]);

	(void)waitpid(client, NULL, 0);
	(void)waitpid(prn, NULL, 0);

	fprintf(stderr,
		"printer received %ld bytes in %.1f s after network read error\n",
		received, elapsed);
	/*
	 * Buggy code: 10 s deadline fires right after the first 4608 B block
	 * is written (~14 s in), abandoning the remaining 3584 B -> 9216 B,
	 * ~14 s elapsed.
	 * Fixed code: drains all 12800 B (8192 internal + 4608 in socket),
	 * copy_stream returns after ~28 s.
	 */
	if (elapsed < 20.0 || received < 11000 || received > (long)SEND_BYTES)
	{
		fprintf(stderr,
			"FAIL: drained %ld bytes in %.1f s (a 10 s truncation leaves ~9216 B/~14 s)\n",
			received, elapsed);
		return 1;
	}
	fprintf(stderr, "PASS: buffered data fully drained to printer after network read error\n");
	return 0;
}
