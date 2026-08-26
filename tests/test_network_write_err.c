/*
 * Regression test: when writing printer responses back to the network fails
 * (peer closed -> EPIPE), copy_stream() must switch to discard mode for the
 * printer->network direction instead of crashing or spinning, while the
 * network->printer job is still delivered in full.
 *
 * This exercises the branch in copy_stream() whose `result = 0;` assignment
 * was a dead store (clang-tidy DeadStores): after writeBuffer() reports a
 * network write error the daemon sets printerToNetworkBuffer.outfd = -1 and
 * discards further printer data, then must still drain the network buffer to
 * the printer and exit cleanly on EOF.
 *
 * Scenario:
 *  - Network client sends a 24 KiB job and closes at once (FIN).
 *  - The printer pushes a response burst first, so the daemon has data to
 *    write back to the network after the peer is already gone -> EPIPE.
 *  - The daemon must deliver all 24 KiB to the printer and return 0.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>

#define JOB_BYTES (3 * BUFFER_SIZE) /* 24 KiB network -> printer */
#define RESP_BYTES 16384            /* printer -> network burst */

int main(void)
{
	int net_sv[2], prn_sv[2], rep[2];
	pid_t client, prn;
	char job[JOB_BYTES], resp[RESP_BYTES];
	long printer_got = -1;
	size_t i;

	/* Real p910nd does this in main(); the daemon must survive the EPIPE. */
	(void)signal(SIGPIPE, SIG_IGN);

	for (i = 0; i < sizeof(job); i++)
		job[i] = (char)(i * 7 + 3);
	memset(resp, 0x5a, sizeof(resp));

	assert(pipe(rep) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
	assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);

	/* Bounds the test. */
	(void)alarm(30);

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
		while (off < sizeof(job))
		{
			ssize_t n = write(net_sv[1], job + off, sizeof(job) - off);
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				_exit(2);
			}
			off += (size_t)n;
		}
		/* FIN: the daemon's writes of printer responses now fail with EPIPE. */
		(void)close(net_sv[1]);
		_exit(0);
	}

	prn = fork();
	assert(prn >= 0);
	if (prn == 0)
	{
		char b;
		long got = 0;
		ssize_t n;
		size_t off = 0;
		(void)close(net_sv[0]);
		(void)close(net_sv[1]);
		(void)close(prn_sv[0]);
		(void)close(rep[0]);
		/* Response burst; the socket buffer absorbs it (non-blocking). */
		while (off < sizeof(resp))
		{
			n = write(prn_sv[1], resp + off, sizeof(resp) - off);
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				break; /* socket buffer full: enough responses queued */
			}
			off += (size_t)n;
		}
		/* Drain the network job. */
		while (read(prn_sv[1], &b, 1) == 1)
			got += 1;
		if (write(rep[1], &got, sizeof(got)) != (ssize_t)sizeof(got))
			_exit(4);
		_exit(0);
	}

	(void)close(net_sv[1]);
	(void)close(prn_sv[1]);
	(void)close(rep[1]);

	assert(copy_stream(net_sv[0], prn_sv[0]) == 0);

	(void)close(net_sv[0]);
	(void)close(prn_sv[0]);
	if (read(rep[0], &printer_got, sizeof(printer_got)) != (ssize_t)sizeof(printer_got))
		printer_got = -1;
	(void)close(rep[0]);

	(void)waitpid(client, NULL, 0);
	(void)waitpid(prn, NULL, 0);

	fprintf(stderr, "printer received %ld of %d job bytes after network write error\n",
		printer_got, JOB_BYTES);
	/*
	 * The network->printer job must arrive intact even though the
	 * printer->network direction hit EPIPE and switched to discard mode.
	 */
	if (printer_got != (long)JOB_BYTES)
	{
		fprintf(stderr, "FAIL: job truncated (got %ld)\n", printer_got);
		return 1;
	}
	fprintf(stderr, "PASS: job fully delivered; printer response discarded after EPIPE\n");
	return 0;
}
