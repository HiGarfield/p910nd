/*
 * Regression test for a misleading failure report in copy_stream(): when the
 * peer (printer OR network) closes its side AFTER the whole job has already
 * been delivered, copy_stream() used to return -1 (and log "copy_stream:
 * <errno>") even though every byte reached the printer.  With an arbitrary
 * timing (the requirement: printer/network/startup may become ready in ANY
 * order), the printer closing mid/after-job is common, so a completed job
 * must not be reported as failed.
 *
 * Root cause: a hard read error (e.g. ECONNRESET) set READ_ERR and eof_read
 * but the EOF-sent marker was only raised when the buffer was already empty
 * on entry to writeBuffer(); on the exit path the loop broke on
 * (READ_ERR && bytes==0) before that marker could be set, so the error flag
 * survived and rc became -1 despite totalin == totalout.
 *
 * Fix: in copy_stream(), if eof_sent is not set but the input hit EOF, there
 * are no pending bytes, and every read byte was already written
 * (totalin == totalout), the job is complete; clear the lingering error so a
 * successfully delivered job is reported as success.
 *
 * This test hammers the exact race: it runs the scenario many times with the
 * printer shutting down its send side mid-job while the network still pushes
 * data, and asserts, every single iteration, that copy_stream() returns 0 and
 * the printer received the full job.  A regression reintroduces the -1 (and a
 * data-integrity check would catch any truncation).
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <sys/resource.h>

#define JOB_BYTES (10 * BUFFER_SIZE)
#define RESP_BYTES 4096
#define ITERS 30

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

/* Run one job with the printer closing its send side mid-job.  Returns the
 * copy_stream() result and reports the printer byte count via *printer_got. */
static int run_once(long *printer_got)
{
	int net_sv[2], prn_sv[2], rep[2];
	pid_t client, prn;
	char job[JOB_BYTES], resp[RESP_BYTES];
	size_t i;
	int rc;

	for (i = 0; i < sizeof(job); i++)
		job[i] = (char)(i * 7 + 3);
	memset(resp, 0x5a, sizeof(resp));

	assert(pipe(rep) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
	assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);

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
		(void)usleep(50000);
		(void)close(net_sv[1]); /* FIN after everything is sent */
		_exit(0);
	}

	prn = fork();
	assert(prn >= 0);
	if (prn == 0)
	{
		size_t off = 0;
		ssize_t n;
		char b;
		long got = 0;
		(void)close(net_sv[0]);
		(void)close(net_sv[1]);
		(void)close(prn_sv[0]);
		(void)close(rep[0]);
		/* Send a response burst first (these must reach the network). */
		while (off < sizeof(resp))
		{
			n = write(prn_sv[1], resp + off, sizeof(resp) - off);
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				break;
			}
			off += (size_t)n;
		}
		/* Now act as a printer that closes its send side while the network
		 * is still pushing the rest of the (large) job. */
		(void)shutdown(prn_sv[1], SHUT_WR);
		while (read(prn_sv[1], &b, 1) == 1)
			got += 1;
		if (write(rep[1], &got, sizeof(got)) != (ssize_t)sizeof(got))
			_exit(4);
		_exit(0);
	}

	(void)close(net_sv[1]);
	(void)close(prn_sv[1]);
	(void)close(rep[1]);

	rc = copy_stream(net_sv[0], prn_sv[0]);

	(void)close(net_sv[0]);
	(void)close(prn_sv[0]);

	if (read(rep[0], printer_got, sizeof(*printer_got)) != (ssize_t)sizeof(*printer_got))
		*printer_got = -1;
	(void)close(rep[0]);
	(void)waitpid(client, NULL, 0);
	(void)waitpid(prn, NULL, 0);
	return rc;
}

int main(void)
{
	int i, bad_rc = 0, trunc = 0;
	double cpu = 0.0, wall = 0.0;

	for (i = 0; i < ITERS; i++)
	{
		long pg = -1;
		double c0 = cpu_now(), w0 = wall_now();
		int rc = run_once(&pg);
		cpu += cpu_now() - c0;
		wall += wall_now() - w0;
		if (rc != 0)
			bad_rc++;
		if (pg != (long)JOB_BYTES)
			trunc++;
	}
	fprintf(stderr,
	        "printer-early-EOF race: %d iters, copy_stream!=0: %d, truncated: %d, cpu=%.3fs wall=%.3fs\n",
	        ITERS, bad_rc, trunc, cpu, wall);
	if (trunc != 0)
	{
		fprintf(stderr, "FAIL: job truncated in %d/%d iterations (data lost)\n", trunc, ITERS);
		return 1;
	}
	if (bad_rc != 0)
	{
		fprintf(stderr, "FAIL: copy_stream returned -1 in %d/%d iterations (completed job misreported)\n",
		        bad_rc, ITERS);
		return 1;
	}
	/*
	 * A genuine busy loop spends CPU comparable to the wall-clock time of the
	 * (slow) printer, i.e. cpu ~= wall.  We assert the *ratio* cpu < wall *
	 * BUSY_FACTOR rather than an absolute budget so the proof is robust to
	 * environments where every instruction is expensive (valgrind binary
	 * translation, sanitizers, loaded hosts, slower architectures) -- there the
	 * legitimate, non-spinning select-driven CPU is amplified but the bytes
	 * are still delivered intact, so an absolute threshold would false-positive.
	 * A real spin fails this with an enormous margin (cpu ~= wall).
	 */
	if (cpu > 1.0 && cpu > wall * 0.25)
	{
		fprintf(stderr, "FAIL: copier burned %.3fs CPU vs %.3fs wall (busy-loop suspected)\n",
		        cpu, wall);
		return 1;
	}
	fprintf(stderr, "PASS: %d/%d jobs delivered intact with copy_stream()==0, no busy loop\n",
	        ITERS, ITERS);
	return 0;
}
