/*
 * Regression test: bidirectional copy_stream() must not busy-loop when the
 * printer side reaches EOF while the network side is quiet.
 *
 * The printer fd is opened O_NONBLOCK and the printer-to-network buffer
 * uses detectEof=0, so a zero-length read does NOT set eof_read.  An EOF'd
 * fd is permanently "readable" for select(), so before the fix every loop
 * iteration woke select() immediately, performed a no-op read, and spun at
 * ~100% CPU until the network side closed.
 *
 * The test runs copy_stream() in THIS process (so getrusage(RUSAGE_SELF)
 * measures exactly the copying loop) between a silent network peer and an
 * EOF'd printer peer, and asserts the CPU consumed while idle is well
 * below what a 1.5 s busy loop would burn.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <time.h>

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
	int net_sv[2];
	int prn_sv[2];
	pid_t helper;
	int status;
	double t0, cpu;
	struct timespec ts;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
	assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);

	bidir = 1;
	log_to_stdout = 0;

	helper = fork();
	assert(helper >= 0);
	if (helper == 0)
	{
		/* Child plays the network client and the printer device. */
		(void)shutdown(prn_sv[1], SHUT_WR); /* printer -> EOF on daemon side */
		ts.tv_sec = 1;
		ts.tv_nsec = 500000000L; /* 1.5 s of network silence */
		nanosleep(&ts, NULL);
		(void)close(net_sv[1]); /* then close the network connection */
		_exit(0);
	}
	(void)close(net_sv[1]);
	(void)close(prn_sv[1]);

	t0 = cpu_now();
	(void)copy_stream(net_sv[0], prn_sv[0]);
	cpu = cpu_now() - t0;

	(void)close(net_sv[0]);
	(void)close(prn_sv[0]);
	(void)wait4(helper, &status, 0, NULL);

	fprintf(stderr, "copier CPU while idle = %.3fs (busy loop would be ~1.5s)\n", cpu);
	if (cpu > 0.75)
	{
		fprintf(stderr, "FAIL: copy_stream consumed %.3fs CPU, busy loop detected\n", cpu);
		return 1;
	}
	fprintf(stderr, "PASS: no busy loop, CPU %.3fs\n", cpu);
	return 0;
}
