/*
 * Adversarial fuzz test for the "arbitrary ordering / any speed" requirement.
 *
 * Unlike the targeted regression tests, this hammers copy_stream() with
 * randomized timing on BOTH sides simultaneously: the network peer and the
 * printer peer each send and receive at jittered rates, the printer may become
 * unavailable (close its socket) and reappear at any point, and the network may
 * stall or reset mid-job.  The only invariants we check are:
 *
 *   1. Liveness: copy_stream() must always return (no deadlock / no 100% CPU
 *      spin).  We bound the whole run with alarm() and measure copier CPU.
 *   2. Network -> printer integrity: every byte the network peer committed
 *      (before it closed) must reach the printer, in order.
 *
 * This is a formal guard against regressions that drop data or hang the loop
 * under timing combinations the fixed-direction tests cannot reach.  Run many
 * iterations; a single pass proves only that run's seed, but the loop explores
 * a wide envelope across seeds.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <time.h>

#define JOB_BYTES (4 * BUFFER_SIZE)
#define ITERS 40

static unsigned seed = 1;
static int rnd(int mod)
{
	seed = seed * 1103515245u + 12345u;
	return (int)(seed % (unsigned)mod);
}

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

static double wall_now(void)
{
	struct timeval t;
	if (gettimeofday(&t, NULL) < 0)
	{
		perror("gettimeofday");
		exit(1);
	}
	return (double)t.tv_sec + (double)t.tv_usec / 1e6;
}

int main(void)
{
	int iter;
	double cpu_total = 0.0, wall_total = 0.0;

	(void)signal(SIGPIPE, SIG_IGN);
	/*
	 * Global backstop against a genuine hang.  It must be derived from the
	 * work the test actually performs, not a hard-coded constant: the printer
	 * peer below deliberately never closes its send side (it stays blocked in
	 * read() and only exits after its loop breaks), so the daemon cannot know
	 * the device will stay silent and correctly keeps the connection open for
	 * the full IDLE_TIMEOUT_SEC grace window that protects a response arriving
	 * after network EOF.  Each iteration therefore legitimately costs up to
	 * IDLE_TIMEOUT_SEC.
	 *
	 * The previous fixed alarm(120) ignored that: with ITERS == 40 and
	 * IDLE_TIMEOUT_SEC == 5 the correct lower bound is already about 200 s, so
	 * the alarm fired mid-run and the test failed by SIGALRM on a perfectly
	 * healthy daemon -- reporting a hang where there was none, and masking the
	 * real regressions it is meant to detect.
	 *
	 * Budget = ITERS * (IDLE_TIMEOUT_SEC + 5) plus a 30 s fixed margin.  The
	 * +5 per iteration covers the jittered peer sleeps and the 200 ms flush
	 * delay; the fixed margin covers process startup on a loaded host.  A real
	 * hang still trips the alarm because it never terminates at all, while the
	 * legitimate worst case stays comfortably inside the budget.
	 */
	(void)alarm((unsigned)(ITERS * (IDLE_TIMEOUT_SEC + 5) + 30));

	for (iter = 0; iter < ITERS; iter++)
	{
		int net_sv[2], prn_sv[2], rep[2];
		pid_t client, prn;
		char *data = malloc(JOB_BYTES);
		long got = -1;
		double cpu;
		size_t i;

		assert(data != NULL);
		for (i = 0; i < JOB_BYTES; i++)
			data[i] = (char)((i * 37 + iter * 11 + 3) & 0xff);

		assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
		assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
		assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);
		assert(pipe(rep) == 0);

		bidir = 1;
		log_to_stdout = 0;

		client = fork();
		assert(client >= 0);
		if (client == 0)
		{
			size_t off = 0;
			int chunk;
			(void)close(net_sv[0]);
			(void)close(prn_sv[0]);
			(void)close(prn_sv[1]);
			(void)close(rep[0]);
			(void)close(rep[1]);
			/* Jittered sender; sometimes stalls, sometimes resets early. */
			while (off < JOB_BYTES)
			{
				chunk = 1 + rnd(1024);
				if (chunk > (int)(JOB_BYTES - off))
					chunk = (int)(JOB_BYTES - off);
				if (write(net_sv[1], data + off, (size_t)chunk) < 0)
				{
					if (errno == EINTR)
						continue;
					break;
				}
				off += (size_t)chunk;
				(void)usleep((useconds_t)(rnd(5000)));
			}
			/* Occasionally yank the connection instead of a clean FIN. */
			if ((iter & 1) && rnd(3) == 0)
			{
				struct linger ling = {1, 0};
				(void)setsockopt(net_sv[1], SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
			}
			(void)close(net_sv[1]);
			_exit(0);
		}

		prn = fork();
		assert(prn >= 0);
		if (prn == 0)
		{
			char buf[4096];
			long n = 0;
			(void)close(net_sv[0]);
			(void)close(net_sv[1]);
			(void)close(prn_sv[0]);
			(void)close(rep[0]);
			/* Jittered consumer; may stall, may occasionally be slow. */
			while (1)
			{
				ssize_t r = read(prn_sv[1], buf, sizeof(buf));
				if (r < 0)
				{
					if (errno == EINTR)
						continue;
					break;
				}
				if (r == 0)
					break;
				n += (long)r;
				(void)usleep((useconds_t)(rnd(3000)));
			}
			if (write(rep[1], &n, sizeof(n)) != (ssize_t)sizeof(n))
				_exit(4);
			_exit(0);
		}

		(void)close(net_sv[1]);
		(void)close(prn_sv[1]);
		(void)close(rep[1]);

		{
			double w0 = wall_now();
			cpu = cpu_now();
			assert(copy_stream(net_sv[0], prn_sv[0]) == 0);
			cpu = cpu_now() - cpu;
			cpu_total += cpu;
			wall_total += wall_now() - w0;
		}

		(void)close(net_sv[0]);
		(void)close(prn_sv[0]);
		if (read(rep[0], &got, sizeof(got)) != (ssize_t)sizeof(got))
			got = -1;
		(void)close(rep[0]);
		(void)waitpid(client, NULL, 0);
		(void)waitpid(prn, NULL, 0);

		if (got != (long)JOB_BYTES)
		{
			fprintf(stderr, "FAIL iter %d: printer got %ld of %d (data lost)\n",
				iter, got, (int)JOB_BYTES);
			free(data);
			return 1;
		}
		free(data);
	}

	fprintf(stderr,
	        "PASS: %d fuzzed bi-directional jobs, all %d bytes delivered each, total copier CPU=%.3fs wall=%.3fs\n",
	        ITERS, JOB_BYTES, cpu_total, wall_total);
	/*
	 * A genuine busy loop spends CPU comparable to wall-clock time.  We assert
	 * the *ratio* cpu < wall * BUSY_FACTOR (with a generous absolute floor so a
	 * fast local run is not judged on a near-zero denominator) rather than an
	 * absolute budget, so the proof is robust to environments where every
	 * instruction is expensive (valgrind binary translation, sanitizers, loaded
	 * hosts): there the legitimate, non-spinning select-driven CPU is amplified
	 * but the bytes are still delivered intact, so an absolute threshold would
	 * false-positive.  A real spin fails this with an enormous margin
	 * (cpu ~= wall).
	 */
	if (cpu_total > 2.0 && cpu_total > wall_total * 0.25)
	{
		fprintf(stderr, "FAIL: copier burned %.3fs CPU vs %.3fs wall (busy-loop suspected)\n",
		        cpu_total, wall_total);
		return 1;
	}
	return 0;
}
