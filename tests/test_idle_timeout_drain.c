/*
 * Regression test: copy_stream() must NOT abort the bidirectional loop on
 * the idle timeout while data is still pending.
 *
 * Background: last_read_time advances only when a network read yields
 * bytes.  Once the peer has sent EOF and all its bytes have been pulled
 * into the buffers, that clock stops.  If the printer side then stalls for
 * longer than IDLE_TIMEOUT_SEC with buffered bytes still undrained, the
 * old code broke out of the loop and silently truncated the print job
 * (dropping both queued socket data and bytes still in the internal
 * circular buffer).
 *
 * Here the printer consumer deliberately pauses after the first
 * JOB_BYTES - BUFFER_SIZE bytes have been delivered, forcing the daemon to
 * sit on a full socket buffer and a full internal buffer for longer than
 * IDLE_TIMEOUT_SEC.  The fix must wait out the stall and deliver every
 * byte; an idle-timeout exit would truncate the job and fail the check.
 *
 * Compiled with -DIDLE_TIMEOUT_SEC=1 to keep the test fast.
 */
#define _GNU_SOURCE
#define IDLE_TIMEOUT_SEC 1
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>

#define JOB_BYTES (3 * BUFFER_SIZE) /* 24 KiB, larger than the printer socket buffer */

int main(void)
{
	int net_sv[2], prn_sv[2], rep[2];
	pid_t client, prn;
	char data[JOB_BYTES];
	long received = -1;
	int sz;
	size_t i;

	for (i = 0; i < sizeof(data); i++)
		data[i] = (char)(i * 13 + 5);

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
	assert(pipe(rep) == 0);

	/* Shrink the printer-side socket buffer so the daemon's write() must
	 * hit EAGAIN once the consumer pauses. */
	sz = 4096;
	assert(setsockopt(prn_sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz)) == 0);
	assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);

	bidir = 1;
	log_to_stdout = 0;

	client = fork();
	assert(client >= 0);
	if (client == 0)
	{
		size_t off = 0;
		(void)close(net_sv[0]);
		(void)close(prn_sv[0]);
		(void)close(prn_sv[1]);
		(void)close(rep[0]);
		(void)close(rep[1]);
		while (off < sizeof(data))
		{
			ssize_t n = write(net_sv[1], data + off, sizeof(data) - off);
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				perror("client write");
				_exit(2);
			}
			off += (size_t)n;
		}
		(void)shutdown(net_sv[1], SHUT_WR);
		_exit(0);
	}

	prn = fork();
	assert(prn >= 0);
	if (prn == 0)
	{
		char buf[4096];
		long got = 0;
		int paused = 0;
		(void)close(net_sv[0]);
		(void)close(net_sv[1]);
		(void)close(prn_sv[0]);
		(void)close(rep[0]);
		for (;;)
		{
			ssize_t r = read(prn_sv[1], buf, sizeof(buf));
			if (r < 0)
			{
				if (errno == EINTR)
					continue;
				perror("consumer read");
				_exit(3);
			}
			if (r == 0)
				break;
			got += (long)r;
			if (!paused && got >= JOB_BYTES - (long)BUFFER_SIZE)
			{
				paused = 1;
				/* Stall well past IDLE_TIMEOUT_SEC so the daemon is left
				 * with undrained data for longer than the timeout. */
				(void)usleep(2500000);
			}
			else
			{
				(void)usleep(20000);
			}
		}
		if (write(rep[1], &got, sizeof(got)) != (ssize_t)sizeof(got))
			_exit(4);
		_exit(0);
	}

	(void)close(net_sv[1]);
	(void)close(prn_sv[1]);
	(void)close(rep[1]);

	(void)copy_stream(net_sv[0], prn_sv[0]);

	(void)close(net_sv[0]);
	(void)close(prn_sv[0]);
	if (read(rep[0], &received, sizeof(received)) != (ssize_t)sizeof(received))
		received = -1;
	(void)close(rep[0]);

	(void)waitpid(client, NULL, 0);
	(void)waitpid(prn, NULL, 0);

	if (received != (long)JOB_BYTES)
	{
		fprintf(stderr, "FAIL: printer received %ld of %d bytes (job truncated by idle timeout?)\n",
			received, (int)JOB_BYTES);
		return 1;
	}
	fprintf(stderr, "PASS: all %d bytes delivered despite printer stall\n", (int)JOB_BYTES);
	return 0;
}
