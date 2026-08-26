/*
 * Regression test for Bug #1: the unidirectional copy_stream() loop stalled
 * delivery to a ready printer because it did a BLOCKING read() on the network
 * descriptor.
 *
 * Root cause
 * ----------
 * p910nd never put the network descriptor into non-blocking mode (only the
 * printer got O_NONBLOCK, in open_printer()).  The old unidirectional loop was:
 *
 *      if (bytes > 0) select(printer writable);   <- only the PRINTER is armed
 *      readBuffer();                              <- BLOCKING read on network
 *      writeBuffer();
 *
 * readBuffer() skips read() entirely when the buffer is FULL (avail == 0), so a
 * full buffer hid the defect.  But after a PARTIAL write to a slow printer the
 * buffer is only partially full, so avail > 0 and readBuffer() issued a
 * blocking read().  If the peer went quiet at that moment (a perfectly legal
 * client that pauses mid-job, and the requirement explicitly allows arbitrary
 * timing with no upper bound), the daemon parked inside read() while:
 *
 *   - the printer was writable, and
 *   - bytes were still buffered and waiting for that printer.
 *
 * So the already-received part of the job was NOT handed to a printer that was
 * ready to take it, for as long as the client stayed silent (unbounded).  That
 * violates "must correctly support printers of any speed / handle speed
 * differences" and "any ready-ordering with no upper bound on the interval".
 * A pre-fix measurement of this exact scenario showed read() blocking 6.89 s
 * with 4096 bytes pending and the printer writable.
 *
 * Fix
 * ---
 * 1. Put both descriptors into non-blocking mode (set_nonblocking()), so a
 *    read()/write() can never park the daemon; short reads/writes become
 *    EAGAIN, which readBuffer()/writeBuffer() already treat as "no progress".
 * 2. select() on BOTH directions (printer-writable AND network-readable) and
 *    only touch a descriptor that select() reported ready.  select() blocks
 *    with no timeout, so an idle job still burns no CPU.
 *
 * What this test proves
 * ---------------------
 * It reproduces the precise triggering state (0 < bytes < BUFFER_SIZE, socket
 * empty, printer writable) by using a slow printer that forces partial writes
 * and a client that sends one burst and then stays silent for SILENCE_S
 * seconds before sending the tail and closing.
 *
 * Formal assertions:
 *   A) Bounded completion time.  This is the discriminating assertion.  The
 *      job needs a fixed minimum time set purely by the printer's speed:
 *      (BURST + TAIL)/CHUNK * CHUNK_US, and the client is silent for
 *      SILENCE_S in the middle of it.  Because the printer keeps draining
 *      throughout the silence, a correct daemon overlaps the two and finishes
 *      in about max(printer_time, SILENCE_S + tail_time).  The pre-fix daemon
 *      instead SERIALISED them: it sat in a blocking read() for the remainder
 *      of the silence while holding a partially-full buffer, so the elapsed
 *      time grew by that stall.  Measured on this exact scenario: pre-fix
 *      9.31 s vs post-fix 6.53 s.  DEADLINE_S sits between the two.
 *   B) Integrity: every byte sent must reach the printer (no truncation) and
 *      copy_stream() must report success.
 *   C) No busy-loop: CPU time must stay far below the wall-clock time, proving
 *      the fix blocks in select() instead of spinning.
 *
 * A regression that restores the blocking read (or that stops arming the
 * network side in select()) re-serialises the silence and the printer drain
 * and therefore breaches DEADLINE_S in assertion (A).
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <sys/resource.h>

#define PIPE_CAP 4096
/* One burst large enough to fill the 8192-byte buffer AND back up the printer,
 * guaranteeing partial writes and therefore 0 < bytes < BUFFER_SIZE. */
#define BURST (BUFFER_SIZE)
#define TAIL 1024
#define SILENCE_S 6
/* Slow printer: CHUNK bytes every CHUNK_US microseconds. */
#define CHUNK 128
#define CHUNK_US 100000
/*
 * Completion deadline.  Printer-bound time for the whole job is
 * (BURST+TAIL)/CHUNK * CHUNK_US = 9216/128 * 0.1 s = 7.2 s, but the printer
 * drains concurrently with the client's silence, so a correct daemon finishes
 * in ~6.5 s (measured).  The pre-fix daemon serialised the stall and needed
 * ~9.3 s (measured).  8.0 s separates the two with ~1.5 s of margin on either
 * side, keeping the test stable on loaded machines while still failing hard on
 * a regression.
 */
#define DEADLINE_S 8.0

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
	gettimeofday(&t, NULL);
	return (double)t.tv_sec + (double)t.tv_usec / 1e6;
}

int main(void)
{
	int net[2], prn[2], rep[2];
	pid_t client, printer;
	struct
	{
		long during_silence;
		long total;
	} report;
	int rc;
	double cpu0, wall0, cpu_used, wall_used;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net) == 0);
	assert(pipe(prn) == 0);
	assert(pipe(rep) == 0);
	/* Shrink the printer pipe so it backs up quickly and forces partial writes. */
	assert(fcntl(prn[1], F_SETPIPE_SZ, PIPE_CAP) >= 0);
	/*
	 * Reproduce what open_printer() does in the real daemon: the printer is
	 * O_NONBLOCK.  This is essential for the test to be discriminating -- with
	 * a BLOCKING printer the pre-fix code would simply block inside write()
	 * until the slow printer drained the whole buffer, which accidentally
	 * emptied the buffer and hid the network-side stall.  With the printer
	 * non-blocking (as in production) writeBuffer() does a PARTIAL write and
	 * leaves 0 < bytes < BUFFER_SIZE, which is exactly the state in which the
	 * pre-fix blocking read() on the network parked the daemon.
	 */
	assert(fcntl(prn[1], F_SETFL, O_NONBLOCK) == 0);

	bidir = 0;
	log_to_stdout = 0;
	(void)alarm(180);

	/* ---- client: one burst, then a long SILENCE, then the tail + FIN ---- */
	client = fork();
	assert(client >= 0);
	if (client == 0)
	{
		char chunk[BURST];
		size_t off = 0;
		memset(chunk, 'A', sizeof(chunk));
		(void)close(net[0]);
		(void)close(prn[0]);
		(void)close(prn[1]);
		(void)close(rep[0]);
		(void)close(rep[1]);
		while (off < sizeof(chunk))
		{
			ssize_t n = write(net[1], chunk + off, sizeof(chunk) - off);
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				_exit(2);
			}
			off += (size_t)n;
		}
		/* Stay completely silent, connection OPEN.  This is the window in
		 * which the pre-fix daemon was blocked inside read(). */
		(void)sleep(SILENCE_S);
		off = 0;
		while (off < TAIL)
		{
			ssize_t n = write(net[1], chunk + off, TAIL - off);
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				_exit(3);
			}
			off += (size_t)n;
		}
		(void)close(net[1]);
		_exit(0);
	}

	/* ---- printer: slow consumer that reports progress during the silence ---- */
	printer = fork();
	assert(printer >= 0);
	if (printer == 0)
	{
		char b[CHUNK];
		ssize_t n;
		long got = 0;
		long at_silence_start = -1;
		long at_silence_end = -1;
		double t0;
		(void)close(net[0]);
		(void)close(net[1]);
		(void)close(prn[1]);
		(void)close(rep[0]);

		t0 = wall_now();
		for (;;)
		{
			double el;
			n = read(prn[0], b, sizeof(b));
			if (n <= 0)
				break;
			got += n;
			(void)usleep(CHUNK_US);
			el = wall_now() - t0;
			/* The client's silence covers roughly t=0..SILENCE_S.  Sample the
			 * backlog drain strictly inside that window. */
			if (at_silence_start < 0 && el >= 1.0)
				at_silence_start = got;
			if (at_silence_end < 0 && el >= (double)SILENCE_S - 1.0)
				at_silence_end = got;
		}
		if (at_silence_start < 0)
			at_silence_start = 0;
		if (at_silence_end < 0)
			at_silence_end = got;
		report.during_silence = at_silence_end - at_silence_start;
		report.total = got;
		if (write(rep[1], &report, sizeof(report)) != (ssize_t)sizeof(report))
			_exit(4);
		_exit(0);
	}

	(void)close(net[1]);
	(void)close(prn[0]);
	(void)close(rep[1]);

	cpu0 = cpu_now();
	wall0 = wall_now();
	rc = copy_stream(net[0], prn[1]);
	cpu_used = cpu_now() - cpu0;
	wall_used = wall_now() - wall0;

	(void)close(net[0]);
	(void)close(prn[1]); /* EOF for the printer child */

	report.during_silence = -1;
	report.total = -1;
	if (read(rep[0], &report, sizeof(report)) != (ssize_t)sizeof(report))
	{
		fprintf(stderr, "FAIL: could not read printer report\n");
		(void)close(rep[0]);
		(void)waitpid(client, NULL, 0);
		(void)waitpid(printer, NULL, 0);
		return 1;
	}
	(void)close(rep[0]);
	(void)waitpid(client, NULL, 0);
	(void)waitpid(printer, NULL, 0);

	fprintf(stderr,
	        "copy_stream rc=%d; printer got %ld/%d bytes total; %ld bytes drained during the\n"
	        "%ds client silence; cpu=%.3fs wall=%.2fs\n",
	        rc, report.total, BURST + TAIL, report.during_silence, SILENCE_S,
	        cpu_used, wall_used);

	/* (B) Integrity: nothing may be lost and the job must be reported OK. */
	if (report.total != (long)(BURST + TAIL))
	{
		fprintf(stderr, "FAIL: printer received %ld of %d bytes (data lost)\n",
		        report.total, BURST + TAIL);
		return 1;
	}
	if (rc != 0)
	{
		fprintf(stderr, "FAIL: copy_stream returned %d for a fully delivered job\n", rc);
		return 1;
	}

	/*
	 * (A) The discriminating assertion: the printer drain and the client's
	 * silence must OVERLAP.  If the daemon parks in read() while holding a
	 * partially-full buffer, the two are serialised and the job overruns the
	 * deadline.
	 */
	if (wall_used > DEADLINE_S)
	{
		fprintf(stderr,
		        "FAIL: job took %.2fs, deadline %.2fs.  The daemon stalled in a blocking\n"
		        "read() on the empty network socket while %d-byte-capacity printer was\n"
		        "writable and bytes were still buffered, instead of overlapping the\n"
		        "printer drain with the peer's silence.\n",
		        wall_used, DEADLINE_S, PIPE_CAP);
		return 1;
	}

	/* (C) No busy-loop: blocking in select() must dominate over spinning. */
	if (cpu_used > wall_used / 4.0 && cpu_used > 1.0)
	{
		fprintf(stderr, "FAIL: burned %.3fs CPU over %.2fs wall (busy-loop suspected)\n",
		        cpu_used, wall_used);
		return 1;
	}

	fprintf(stderr,
	        "PASS: job finished in %.2fs (deadline %.2fs), %ld bytes drained during the\n"
	        "peer's silence, job intact (%ld bytes), no busy loop (cpu %.3fs)\n",
	        wall_used, DEADLINE_S, report.during_silence, report.total, cpu_used);
	return 0;
}
