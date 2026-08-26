/*
 * Regression test for the idle-timeout comparison in copy_stream().
 *
 * Bug: the old code compared only tv_sec:
 *     now.tv_sec - last_read_time.tv_sec >= IDLE_TIMEOUT_SEC
 * When last_read_time.tv_usec exceeds now.tv_usec this can fire up to
 * one second EARLY, tearing down a job on a slow printer before the full
 * idle period has elapsed.
 *
 * Two checks:
 *  1. Unit test of idle_timeout_elapsed() across the microsecond
 *     boundary (deterministic, does not depend on wall-clock timing).
 *  2. Integration test: with IDLE_TIMEOUT_SEC=2 the daemon must block in
 *     copy_stream() for at least ~2 s of network silence before the idle
 *     timeout fires; an early return means the precision bug is back.
 */
#define _GNU_SOURCE
#define IDLE_TIMEOUT_SEC 2
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <time.h>

static double now_mono(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
	{
		perror("clock_gettime");
		exit(1);
	}
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Unit-level check of the timeout predicate. */
static void test_timeout_predicate(void)
{
	struct timeval last, now;

	/* Baseline: 0.5 s elapsed. */
	last.tv_sec = 100;
	last.tv_usec = 500000;
	now.tv_sec = 100;
	now.tv_usec = 900000;
	assert(idle_timeout_elapsed(&now, &last, 1) == 0);

	/* Exactly 1.0 s: tv_sec diff == 1 and usec now >= usec last. */
	now.tv_sec = 101;
	now.tv_usec = 500000;
	assert(idle_timeout_elapsed(&now, &last, 1) == 1);

	/* 1.5 s elapsed: tv_sec diff == 1, usec now (999999) > usec last. */
	now.tv_sec = 101;
	now.tv_usec = 999999;
	assert(idle_timeout_elapsed(&now, &last, 1) == 1);

	/*
	 * The old bug: tv_sec diff reaches 1 while usec now (100000) is
	 * still smaller than usec last (900000) - only 0.2 s has actually
	 * elapsed, so the timeout must NOT fire.
	 */
	last.tv_sec = 100;
	last.tv_usec = 900000;
	now.tv_sec = 101;
	now.tv_usec = 100000;
	assert(idle_timeout_elapsed(&now, &last, 1) == 0);

	/* Same instant, but 1.0 s really elapsed (usec equal). */
	now.tv_sec = 101;
	now.tv_usec = 900000;
	assert(idle_timeout_elapsed(&now, &last, 1) == 1);

	/* Larger timeout value. */
	assert(idle_timeout_elapsed(&now, &last, 2) == 0);
	now.tv_sec = 102;
	now.tv_usec = 900000;
	assert(idle_timeout_elapsed(&now, &last, 2) == 1);

	fprintf(stderr, "PASS: idle_timeout_elapsed() boundary cases\n");
}

/* Integration: copy_stream() must not return early while the peer stays
 * connected and silent. */
static void test_no_early_timeout(void)
{
	int net_sv[2], prn_sv[2], rep[2];
	pid_t client, prn;
	char data[1024];
	long received = -1;
	double t0, elapsed;
	size_t i;

	for (i = 0; i < sizeof(data); i++)
		data[i] = (char)(i * 7 + 3);

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
	assert(pipe(rep) == 0);
	assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);

	bidir = 1;
	log_to_stdout = 0;

	client = fork();
	assert(client >= 0);
	if (client == 0)
	{
		struct timespec ts;
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
		/* Stay connected: no SHUT_WR, no close.  Only the idle timeout
		 * may end the copy; the process outlives it. */
		ts.tv_sec = 3;
		ts.tv_nsec = 0;
		(void)nanosleep(&ts, NULL);
		_exit(0);
	}

	prn = fork();
	assert(prn >= 0);
	if (prn == 0)
	{
		char buf[64];
		long got = 0;
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
			(void)usleep(1000);
		}
		if (write(rep[1], &got, sizeof(got)) != (ssize_t)sizeof(got))
			_exit(4);
		_exit(0);
	}

	(void)close(net_sv[1]);
	(void)close(prn_sv[1]);
	(void)close(rep[1]);

	t0 = now_mono();
	(void)copy_stream(net_sv[0], prn_sv[0]);
	elapsed = now_mono() - t0;

	(void)close(net_sv[0]);
	(void)close(prn_sv[0]);
	if (read(rep[0], &received, sizeof(received)) != (ssize_t)sizeof(received))
		received = -1;
	(void)close(rep[0]);

	(void)waitpid(client, NULL, 0);
	(void)waitpid(prn, NULL, 0);

	fprintf(stderr, "copy_stream returned after %.2fs (IDLE_TIMEOUT_SEC=%d)\n",
		elapsed, IDLE_TIMEOUT_SEC);

	if (received != (long)sizeof(data))
	{
		fprintf(stderr, "FAIL: printer received %ld of %d bytes\n",
			received, (int)sizeof(data));
		exit(1);
	}
	if (elapsed < 1.8)
	{
		fprintf(stderr, "FAIL: idle timeout fired early at %.2fs\n", elapsed);
		exit(1);
	}
	if (elapsed > 4.5)
	{
		fprintf(stderr, "FAIL: copy_stream stuck for %.2fs\n", elapsed);
		exit(1);
	}
	fprintf(stderr, "PASS: no early idle timeout, job complete in %.2fs\n", elapsed);
}

int main(void)
{
	test_timeout_predicate();
	test_no_early_timeout();
	fprintf(stderr, "PASS: all idle-timeout precision tests\n");
	return 0;
}
