/*
 * Formal proof test for the "arbitrary timing / ordering" requirement:
 *
 *   "The printer device, the network, and p910nd's startup may become ready
 *    in ANY order, with ANY (unbounded) interval between them, and the daemon
 *    must still work correctly."
 *
 * This exercises the hardest sub-case of that requirement: the printer (sink)
 * is NOT ready when the network peer connects and dumps the whole job at once.
 * The daemon must:
 *   - not lose any bytes (correctness),
 *   - not busy-loop / deadlock the CPU while waiting for the printer,
 *   - handle the arbitrarily-large speed difference (fast network, late/
 *     slow printer) by blocking in select() rather than spinning,
 *   - deliver every byte once the printer finally becomes available, no
 *     matter how long the gap.
 *
 * We use a multi-second consumer delay (an "unbounded" gap, bounded here only
 * by the alarm) combined with a job larger than BUFFER_SIZE so the daemon is
 * forced to buffer and then block on a not-yet-writable printer.  The test
 * fails if bytes are truncated, or if the copier burns significant CPU while
 * the printer is absent (proof that it blocks instead of spinning).
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <time.h>

#define JOB_BYTES (20 * BUFFER_SIZE) /* 160 KiB, far exceeds one buffer */

static double cpu_now(void)
{
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) < 0)
	{
		perror("getrusage");
		exit(1);
	}
	return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6 +
	       (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
}

int main(void)
{
	int net_sv[2], prn_sv[2], rep[2];
	pid_t client, prn;
	char data[JOB_BYTES];
	long received = -1;
	size_t i;
	double cpu;

	for (i = 0; i < sizeof(data); i++)
		data[i] = (char)(i * 23 + 7);

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
	assert(pipe(rep) == 0);

	/* Non-blocking printer end, as open_printer() opens the real device. */
	assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);

	/* Bounds the test; the printer gap here is a few seconds. */
	(void)alarm(30);

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
		/* Dump the entire job immediately; the printer is not ready yet. */
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
		ssize_t r;
		(void)close(net_sv[0]);
		(void)close(net_sv[1]);
		(void)close(prn_sv[0]);
		(void)close(rep[0]);
		/* The printer arrives LATE: wait well past the time the network
		 * has already delivered everything into the daemon's buffer. */
		(void)sleep(3);
		while ((r = read(prn_sv[1], buf, sizeof(buf))) > 0)
		{
			got += (long)r;
			/* Slow consumer too, so speed differences are exercised. */
			(void)usleep(1000);
		}
		if (write(rep[1], &got, sizeof(got)) != (ssize_t)sizeof(got))
			_exit(4);
		_exit(0);
	}

	(void)close(net_sv[1]);
	(void)close(prn_sv[1]);
	(void)close(rep[1]);

	bidir = 0;
	log_to_stdout = 0;

	cpu = cpu_now();
	/* Both unidirectional and the late-printer timing must work. */
	assert(copy_stream(net_sv[0], prn_sv[0]) == 0);
	cpu = cpu_now() - cpu;

	(void)close(net_sv[0]);
	(void)close(prn_sv[0]);
	if (read(rep[0], &received, sizeof(received)) != (ssize_t)sizeof(received))
		received = -1;
	(void)close(rep[0]);

	(void)waitpid(client, NULL, 0);
	(void)waitpid(prn, NULL, 0);

	fprintf(stderr,
		"ordering: printer ready %ds after network; %ld/%d bytes, copier CPU=%.3fs\n",
		3, received, JOB_BYTES, cpu);

	if (received != (long)JOB_BYTES)
	{
		fprintf(stderr, "FAIL: job truncated to %ld bytes (lost data while printer absent)\n",
			received);
		return 1;
	}
	if (cpu > 0.05)
	{
		fprintf(stderr, "FAIL: copier consumed %.3fs CPU while printer absent (busy-loop/deadlock)\n",
			cpu);
		return 1;
	}
	fprintf(stderr,
		"PASS: full %d bytes delivered despite late/unknown-order printer, no busy loop (CPU %.3fs)\n",
		JOB_BYTES, cpu);
	return 0;
}
