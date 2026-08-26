/*
 * Regression test for Bug #2: open_printer() opened the printer device with
 * O_WRONLY (blocking) in unidirectional mode.  With several device drivers --
 * and unambiguously with a FIFO standing in for the printer -- open() blocks
 * until a reader appears.  That hung the entire daemon: it could not accept
 * new connections and could not be shut down, a process-level deadlock that
 * violates the requirement that a temporarily-missing printer must not stall
 * the daemon (it may block a job, but must never deadlock the process).
 *
 * The fix adds O_NONBLOCK to the unidirectional open() as well, so open()
 * returns immediately (-1, ENXIO/ENOENT) and the caller's retry loop simply
 * sleeps and tries again.
 *
 * Determinism: we create a FIFO and open it for writing with no reader
 * attached.  A blocking O_WRONLY open() would hang forever; the fixed
 * O_WRONLY|O_NONBLOCK open() returns -1 at once.  An alarm() bounds the test
 * so a regression (blocking open) is reported as a timeout rather than a hang.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/stat.h>

int main(void)
{
	char fifo[] = "/tmp/p910nd_open_nb_XXXXXX";
	int lp;
	const char *saved_device;

	/* mkstemp-style unique name; use mktemp then mkfifo. */
	{
		int fd = mkstemp(fifo);
		assert(fd >= 0);
		(void)close(fd);
		(void)unlink(fifo);
	}
	assert(mkfifo(fifo, 0600) == 0);

	/* Bound the test: a blocking open() would otherwise hang forever. */
	(void)alarm(10);

	/* Drive open_printer() directly, unidirectional (bidir = 0). */
	bidir = 0;
	log_to_stdout = 0;
	saved_device = device;
	device = fifo; /* open_printer opens this path, O_WRONLY|O_NONBLOCK */

	lp = open_printer('0');

	(void)alarm(0);

	if (lp != -1)
	{
		fprintf(stderr, "FAIL: open_printer returned %d for an unwritten FIFO (expected -1, non-blocking)\n", lp);
		(void)close(lp);
		(void)unlink(fifo);
		return 1;
	}
	/* Must be a clean immediate failure, not a hang that we escaped via alarm. */
	assert(errno == ENXIO || errno == ENOENT || errno == ENOTCONN);

	device = (char *)saved_device;
	(void)unlink(fifo);
	fprintf(stderr, "PASS: open_printer unidirectional open is non-blocking (returned -1, errno=%d)\n", errno);
	return 0;
}
