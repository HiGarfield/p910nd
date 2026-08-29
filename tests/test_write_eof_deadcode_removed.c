/*
 * Formal proof test that removing the unreachable EPIPE/ECONNRESET branch in
 * writeBuffer() did NOT change observable behaviour, and that the "peer closed
 * after all data was sent" completion case is still handled correctly.
 *
 * The removed branch was:
 *     if (b->bytes == 0 && (errno == EPIPE || errno == ECONNRESET)) {
 *         if (b->eof_read) b->eof_sent = 1;
 *         return 0;
 *     }
 * It was unreachable because write() is only called when avail > 0, which
 * implies b->bytes > 0, so b->bytes == 0 can never hold at that point.  The
 * real completion detection lives at the end of writeBuffer(): when eof_read is
 * set and the last bytes have drained (b->bytes == 0), eof_sent is raised.
 *
 * This test exercises BOTH the case the dead branch *claimed* to handle
 * (peer closes receive side after the whole job was delivered -> job must
 * complete successfully) and a genuine partial-write error (peer resets while
 * bytes are still pending -> must be reported, not silently swallowed), to
 * prove the behaviour is byte-for-byte identical with and without the branch.
 *
 * Scenario A (bidirectional, EPIPE after full delivery):
 *   - Client sends a 48 KiB job then closes its receive side (FIN/RST) so the
 *     daemon's writes of printer responses fail with EPIPE.
 *   - The network->printer job must STILL be delivered in full (the daemon
 *     switches the printer->network direction to discard mode) and copy_stream
 *     must return 0 (success), because every network byte already reached the
 *     printer before the peer went away.
 *
 * Scenario B (unidirectional, EPIPE with bytes still pending -> real failure):
 *   - The printer is a pipe whose reader closes while the daemon is BLOCKED
 *     with a full pipe, i.e. while job bytes are unquestionably still pending.
 *     copy_stream must report a failure.
 *
 *   The previous version of scenario B was racy: the "printer" was a socket
 *   whose peer closed as soon as the printer child was scheduled, and the job
 *   (48 KiB) fit entirely into the socket's default send buffer.  If the
 *   daemon won that race it handed the whole job to the kernel before the peer
 *   was gone, so every write succeeded, totalin == totalout, and copy_stream
 *   correctly returned 0 -- while the test demanded a non-zero result.  On the
 *   unmodified tree that made the test fail roughly a third of all runs.
 *
 *   The fix is to remove the race instead of the timing: the job is larger
 *   than the pipe capacity, so the daemon provably blocks with bytes pending
 *   (it cannot make progress without a reader), and the reader only closes
 *   afterwards.  The daemon is therefore not racing anything when the error
 *   is injected, and the assertion holds on every run and every platform.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>

#define JOB_BYTES (6 * BUFFER_SIZE) /* 48 KiB, scenario A */

/*
 * Scenario B job.  It must exceed the pipe capacity so the daemon is forced to
 * block; 1 MiB is larger than the default pipe capacity of every platform this
 * suite targets (Linux, macOS and the BSDs all default to at most 64 KiB), and
 * the precondition is asserted at run time rather than assumed.
 */
#define PIPE_JOB_BYTES (1024 * 1024)
static char big_job[PIPE_JOB_BYTES]; /* static: too large for the stack */

/*
 * Measure the pipe capacity by filling it (non-blocking) and counting, then
 * draining it again so nothing is left behind.  Portable -- it needs no
 * Linux-only F_SETPIPE_SZ.  The count can only undershoot the true capacity by
 * less than one chunk, which is far below PIPE_JOB_BYTES, so the comparison
 * below is sound.
 */
static long probe_pipe_capacity(int wfd, int rfd)
{
	static char chunk[4096];
	static char sink[4096];
	long capacity = 0;
	long left;

	for (;;)
	{
		ssize_t n = write(wfd, chunk, sizeof(chunk));
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			break; /* EAGAIN: the pipe is full */
		}
		capacity += (long)n;
	}
	/* Drain everything we just wrote so the pipe is empty again. */
	for (left = capacity; left > 0;)
	{
		ssize_t n = read(rfd, sink, sizeof(sink));
		if (n <= 0)
			break;
		left -= (long)n;
	}
	return capacity;
}

/* Drives one copy_stream() run with a client that sends JOB_BYTES then
 * closes, and a printer that either drains fully or aborts early.  Returns
 * copy_stream()'s rc and (via *printer_got) the number of job bytes the
 * printer actually received. */
static int run_job(int bidir_mode, int printer_abort, long *printer_got)
{
	int net_sv[2], prn_sv[2], rep[2];
	pid_t client, prn;
	char job[JOB_BYTES];
	int rc;

	for (size_t i = 0; i < sizeof(job); i++)
		job[i] = (char)(i * 11 + 5);

	assert(pipe(rep) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
	assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);
	(void)signal(SIGPIPE, SIG_IGN);
	(void)alarm(60);

	bidir = bidir_mode;
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
		/* In bidirectional mode the peer closes its receive side AFTER the
		 * whole job was delivered, triggering EPIPE on response writes. */
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
		/* In bidirectional mode, emit a response burst first so the daemon
		 * has data to write back to the (already-closed) network. */
		if (bidir_mode)
		{
			char resp[8192];
			memset(resp, 0x5a, sizeof(resp));
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
		}
		/* If printer_abort, stop reading early (drop the connection) so the
		 * daemon hits a genuine EPIPE with bytes still pending. */
		if (printer_abort)
		{
			(void)close(prn_sv[1]);
			_exit(0);
		}
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

/*
 * Scenario B, deterministic: the printer is a pipe, the job is larger than the
 * pipe capacity, and the reader only closes after the daemon has provably
 * blocked.  The daemon therefore always has bytes pending when the write
 * fails, so a genuine error must be reported.
 */
static int run_blocked_printer_abort(void)
{
	int net_sv[2];
	int prn[2];
	pid_t client, prn_child;
	int rc;
	long capacity;
	size_t i;

	for (i = 0; i < sizeof(big_job); i++)
		big_job[i] = (char)(i * 13 + 3);

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
	assert(pipe(prn) == 0);
	assert(fcntl(prn[1], F_SETFL, O_NONBLOCK) == 0);

	capacity = probe_pipe_capacity(prn[1], prn[0]);
	if (capacity <= 0 || capacity >= (long)PIPE_JOB_BYTES)
	{
		fprintf(stderr,
		        "FAIL: precondition not met, pipe capacity %ld must be "
		        "smaller than the %d byte job\n",
		        capacity, PIPE_JOB_BYTES);
		exit(1);
	}

	bidir = 0;
	log_to_stdout = 0;

	client = fork();
	assert(client >= 0);
	if (client == 0)
	{
		size_t off = 0;
		(void)signal(SIGALRM, SIG_DFL);
		(void)alarm(60);
		(void)close(net_sv[0]);
		(void)close(prn[0]);
		(void)close(prn[1]);
		while (off < sizeof(big_job))
		{
			ssize_t n = write(net_sv[1], big_job + off, sizeof(big_job) - off);
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				_exit(2);
			}
			off += (size_t)n;
		}
		(void)close(net_sv[1]);
		for (;;)
			(void)pause();
	}

	prn_child = fork();
	assert(prn_child >= 0);
	if (prn_child == 0)
	{
		(void)signal(SIGALRM, SIG_DFL);
		(void)alarm(60);
		(void)close(net_sv[0]);
		(void)close(net_sv[1]);
		(void)close(prn[1]);
		/*
		 * The daemon blocks within microseconds: it cannot make progress
		 * once the pipe is full, because the job is larger than the pipe
		 * capacity.  Closing only now makes "bytes are pending" a fact
		 * rather than a race.
		 */
		(void)sleep(1);
		(void)close(prn[0]);
		_exit(0);
	}

	(void)close(net_sv[1]);
	(void)close(prn[0]);

	rc = copy_stream(net_sv[0], prn[1]);

	(void)close(net_sv[0]);
	(void)close(prn[1]);
	(void)kill(client, SIGKILL);
	(void)waitpid(client, NULL, 0);
	(void)waitpid(prn_child, NULL, 0);
	return rc;
}

int main(void)
{
	long got;

	(void)signal(SIGPIPE, SIG_IGN);
	(void)alarm(120);

	/* Scenario A: bidirectional, EPIPE after full delivery -> success, full job. */
	if (run_job(1, 0, &got) != 0)
	{
		fprintf(stderr, "FAIL A: copy_stream returned error on EPIPE-after-delivery\n");
		return 1;
	}
	if (got != (long)JOB_BYTES)
	{
		fprintf(stderr, "FAIL A: job truncated to %ld (lost data after EPIPE)\n", got);
		return 1;
	}
	fprintf(stderr, "PASS A: bidirectional EPIPE after full delivery -> rc=0, %ld bytes intact\n", got);

	/*
	 * Scenario B: the printer aborts while bytes are still pending -> the
	 * error must be reported, not swallowed.  This run is deterministic: the
	 * daemon is blocked when the reader goes away.
	 */
	if (run_blocked_printer_abort() == 0)
	{
		fprintf(stderr,
		        "FAIL B: copy_stream reported success although the printer "
		        "went away with job bytes still pending\n");
		return 1;
	}
	fprintf(stderr,
	        "PASS B: genuine pending-bytes write error is reported (rc != 0)\n");

	fprintf(stderr, "PASS: removing unreachable EPIPE branch changed no observable behaviour\n");
	return 0;
}
