/*
 * Regression test: in bidirectional mode the daemon must NOT burn the whole
 * IDLE_TIMEOUT_SEC grace window on a job whose printer has already closed its
 * send side (reported EOF).
 *
 * Bug
 * ---
 * printerToNetworkBuffer is created with detectEof == 0, so readBuffer() never
 * recorded a zero-length read in eof_read.  The bidirectional loop therefore
 * had exactly two ways to set printer_stream_done:
 *   (a) a hard network write error, or
 *   (b) the absolute grace deadline (network EOF + IDLE_TIMEOUT_SEC) expiring.
 * There was no "the printer can no longer say anything" path.  A genuine
 * printer EOF was handled by the `result == 0` branch, which merely armed the
 * 100 ms re-read throttle -- treating a PERMANENT end-of-stream as if it were
 * a device that happened to be quiescent.  Consequence: every bidirectional
 * job whose printer closed (the normal way a device finishes) paid a full
 * IDLE_TIMEOUT_SEC before copy_stream() returned.  Measured on the pre-fix
 * tree: a 4 KiB job took 5.23 s with IDLE_TIMEOUT_SEC == 5.  Under (x)inetd,
 * where one process serves exactly one job, that fixed penalty throttles
 * throughput directly.
 *
 * Fix
 * ---
 * readBufferEx() reports a genuine zero-length read through an out-parameter
 * (the plain return value of 0 is ambiguous: it also means "buffer full" and
 * EAGAIN/EWOULDBLOCK/EINTR).  The loop latches that as printer_eof and, once
 * network EOF has been seen AND the already-received response has been
 * forwarded (bytes == 0), completes immediately instead of waiting for the
 * deadline.
 *
 * Formal argument that the fix is both sound and necessary
 * -------------------------------------------------------
 * Soundness (no response can be lost).  The early-completion guard is
 *   eof_reached && outfd != -1 && printer_eof && printerToNetworkBuffer.bytes == 0.
 *   - printer_eof holds only after read(io_lp, ...) returned 0.  On a stream
 *     descriptor that is a permanent state: the peer has closed its send side,
 *     so no byte can ever arrive afterwards.  Hence there is no future
 *     response for the grace window to protect.
 *   - bytes == 0 means every byte already received from the printer has been
 *     handed to writeBuffer() and drained to the network, so nothing pending
 *     is discarded either.
 *   Therefore the set of bytes delivered to the network peer is unchanged by
 *   the early exit; only the idle waiting time is removed.
 * Necessity (the grace window is still kept where it matters).  When the
 *   printer has NOT reported EOF, printer_eof is 0 and the original absolute
 *   deadline governs, so a printer that answers only AFTER network EOF still
 *   gets the full window.  That path is covered by
 *   test_bidir_late_response_after_net_eof, which continues to pass.
 *
 * What this test asserts
 * ----------------------
 *   1. Latency: with a printer that consumes the job and then closes its send
 *      side, copy_stream() returns well before the grace deadline.  The bound
 *      is IDLE_TIMEOUT_SEC * 0.6, which the pre-fix code cannot satisfy (it
 *      always needed >= IDLE_TIMEOUT_SEC) while the fixed code finishes in
 *      ~0.2 s (the deliberate flush usleep), leaving a wide margin so the
 *      proof is not timing-flaky on a loaded host.
 *   2. Integrity: the job still reaches the printer byte-for-byte, and a
 *      response the printer emits before closing still reaches the network
 *      peer in full.  This guards against "fixing" the latency by dropping
 *      data or truncating the response.
 *   3. No busy-loop: CPU consumed must stay far below wall time, so the
 *      latency win is a genuine early exit and not a spin.
 *
 * Run over several iterations so a single lucky schedule cannot pass it.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <sys/resource.h>

#define JOB_BYTES (4 * BUFFER_SIZE)
#define RESP "PRINTER-DONE-STATUS-OK"
#define ITERS 5

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

/*
 * One job.  The network peer sends the whole job then closes (EOF).  The
 * printer peer consumes the whole job, emits RESP, then closes its send side
 * -- the EOF that must let the daemon finish without waiting for the grace
 * deadline.
 *
 * *elapsed      <- wall time spent inside copy_stream()
 * *printer_got  <- bytes the printer received (must equal JOB_BYTES)
 * *resp_ok      <- 1 if the network peer received exactly RESP
 * returns copy_stream()'s result (must be 0)
 */
static int run_once(double *elapsed, long *printer_got, int *resp_ok)
{
	int net_sv[2], prn_sv[2], rep[2];
	pid_t client, prn;
	char *job;
	size_t i;
	int rc, status;
	double t0;

	job = malloc(JOB_BYTES);
	assert(job != NULL);
	for (i = 0; i < JOB_BYTES; i++)
		job[i] = (char)(i * 13 + 7);

	assert(pipe(rep) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
	assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);

	/* Generous backstop: a regression that hangs must fail, not wedge. */
	(void)alarm(60);
	bidir = 1;
	log_to_stdout = 0;

	client = fork();
	assert(client >= 0);
	if (client == 0)
	{
		char resp[256];
		size_t off = 0, got = 0;
		(void)close(net_sv[0]);
		(void)close(prn_sv[0]);
		(void)close(prn_sv[1]);
		(void)close(rep[0]);
		(void)close(rep[1]);
		while (off < JOB_BYTES)
		{
			ssize_t n = write(net_sv[1], job + off, JOB_BYTES - off);
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				_exit(2);
			}
			off += (size_t)n;
		}
		/* Network EOF: the daemon has the whole job. */
		(void)shutdown(net_sv[1], SHUT_WR);
		/* Collect the printer's response until the daemon closes. */
		while (got < sizeof(resp))
		{
			ssize_t n = read(net_sv[1], resp + got, sizeof(resp) - got);
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				if (errno == ECONNRESET)
					break;
				_exit(3);
			}
			if (n == 0)
				break;
			got += (size_t)n;
		}
		(void)close(net_sv[1]);
		_exit(got == strlen(RESP) && memcmp(resp, RESP, got) == 0 ? 0 : 5);
	}

	prn = fork();
	assert(prn >= 0);
	if (prn == 0)
	{
		char buf[4096];
		long got = 0;
		(void)close(net_sv[0]);
		(void)close(net_sv[1]);
		(void)close(prn_sv[0]);
		(void)close(rep[0]);
		while (got < (long)JOB_BYTES)
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
			got += (long)r;
		}
		/* Answer, then close the send side: this is the printer EOF. */
		{
			size_t off = 0, len = strlen(RESP);
			while (off < len)
			{
				ssize_t n = write(prn_sv[1], RESP + off, len - off);
				if (n < 0)
				{
					if (errno == EINTR)
						continue;
					break;
				}
				off += (size_t)n;
			}
		}
		(void)shutdown(prn_sv[1], SHUT_WR);
		if (write(rep[1], &got, sizeof(got)) != (ssize_t)sizeof(got))
			_exit(4);
		(void)close(rep[1]);
		/* Stay alive so the descriptor is not closed by process exit; the
		 * shutdown() above is the only EOF source, which is exactly the
		 * condition under test. */
		{
			char sink[256];
			while (read(prn_sv[1], sink, sizeof(sink)) > 0)
				;
		}
		_exit(0);
	}

	(void)close(net_sv[1]);
	(void)close(prn_sv[1]);
	(void)close(rep[1]);

	t0 = wall_now();
	rc = copy_stream(net_sv[0], prn_sv[0]);
	*elapsed = wall_now() - t0;

	(void)close(net_sv[0]);
	(void)close(prn_sv[0]);

	if (read(rep[0], printer_got, sizeof(*printer_got)) != (ssize_t)sizeof(*printer_got))
		*printer_got = -1;
	(void)close(rep[0]);

	*resp_ok = 0;
	assert(waitpid(client, &status, 0) == client);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		*resp_ok = 1;
	(void)waitpid(prn, NULL, 0);

	free(job);
	(void)alarm(0);
	return rc;
}

int main(void)
{
	int i, bad_rc = 0, trunc = 0, lost_resp = 0, too_slow = 0;
	double worst = 0.0, total = 0.0;
	double cpu0, cpu_used, wall0, wall_used;
	/* Pre-fix the loop always needed >= IDLE_TIMEOUT_SEC; the fixed code
	 * finishes in about the 0.2 s flush delay.  0.6 * IDLE_TIMEOUT_SEC sits
	 * far from both, so the verdict is unambiguous yet not timing-fragile. */
	const double budget = (double)IDLE_TIMEOUT_SEC * 0.6;

	(void)signal(SIGPIPE, SIG_IGN);

	cpu0 = cpu_now();
	wall0 = wall_now();
	for (i = 0; i < ITERS; i++)
	{
		double el = 0.0;
		long pg = -1;
		int ok = 0;
		int rc = run_once(&el, &pg, &ok);

		total += el;
		if (el > worst)
			worst = el;
		if (rc != 0)
			bad_rc++;
		if (pg != (long)JOB_BYTES)
			trunc++;
		if (!ok)
			lost_resp++;
		if (el > budget)
			too_slow++;
	}
	cpu_used = cpu_now() - cpu0;
	wall_used = wall_now() - wall0;

	fprintf(stderr,
	        "printer-EOF grace: %d iters, worst=%.3fs avg=%.3fs budget=%.3fs "
	        "(IDLE_TIMEOUT_SEC=%d), rc!=0:%d truncated:%d lost-resp:%d over-budget:%d "
	        "cpu=%.3fs wall=%.3fs\n",
	        ITERS, worst, total / ITERS, budget, IDLE_TIMEOUT_SEC,
	        bad_rc, trunc, lost_resp, too_slow, cpu_used, wall_used);

	/* Integrity first: a latency win that loses data is not a fix. */
	if (trunc != 0)
	{
		fprintf(stderr, "FAIL: job truncated in %d/%d iterations (data lost)\n",
		        trunc, ITERS);
		return 1;
	}
	if (lost_resp != 0)
	{
		fprintf(stderr, "FAIL: printer response lost in %d/%d iterations\n",
		        lost_resp, ITERS);
		return 1;
	}
	if (bad_rc != 0)
	{
		fprintf(stderr, "FAIL: copy_stream returned non-zero in %d/%d iterations\n",
		        bad_rc, ITERS);
		return 1;
	}
	/* The actual bug: waiting out the grace window despite a printer EOF. */
	if (too_slow != 0)
	{
		fprintf(stderr,
		        "FAIL: %d/%d iterations exceeded %.3fs (worst %.3fs) -- the daemon "
		        "still waits out the grace window after a printer EOF\n",
		        too_slow, ITERS, budget, worst);
		return 1;
	}
	/* An early exit achieved by spinning would be a different bug. */
	if (cpu_used > 1.0 && cpu_used > wall_used * 0.25)
	{
		fprintf(stderr, "FAIL: copier burned %.3fs CPU vs %.3fs wall (busy-loop suspected)\n",
		        cpu_used, wall_used);
		return 1;
	}
	fprintf(stderr,
	        "PASS: printer EOF completes the job promptly (worst %.3fs < %.3fs) "
	        "with full job and response integrity, no busy loop\n",
	        worst, budget);
	return 0;
}
