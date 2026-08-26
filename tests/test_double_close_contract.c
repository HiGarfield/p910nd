/*
 * Regression test for Bug #2: copy_stream() closed the caller's descriptors and
 * the caller (server() / one_job()) then closed them AGAIN.
 *
 * Root cause
 * ----------
 * dup_fd_below_fdsetsize() replaces a descriptor that lies outside
 * [0, FD_SETSIZE) with an in-range duplicate and CLOSES the original as part of
 * that substitution (it must, otherwise the original would leak).  copy_stream()
 * then closes the duplicate on its way out.  The net effect is that the
 * descriptor the caller handed in no longer exists when copy_stream() returns.
 *
 * server()'s job loop nevertheless did, unconditionally:
 *
 *      copy_stream(fd, lp);
 *      close(fd);              <- already closed inside copy_stream()
 *      close(lp);              <- same
 *
 * A close() of an already-closed number normally just fails with EBADF, which
 * looks harmless.  It is not: descriptor numbers are recycled immediately by the
 * kernel (verified below), so between copy_stream() returning and server()'s
 * close() the number can already have been handed to something else - the next
 * accept()ed connection, the listening socket, the lock file.  The stale close()
 * then destroys that unrelated live descriptor.  In a long-running daemon this
 * surfaces as the listener or a foreign print job dying for no visible reason.
 *
 * Fix
 * ---
 * copy_stream_ex() reports, via fd_closed/lp_closed, whether it already released
 * the caller's descriptor number, and server()/one_job() only close what is
 * still theirs.
 *
 * What this test proves
 * ---------------------
 * 1. Precondition: descriptor numbers really are recycled at once, so a stale
 *    close() is genuinely dangerous rather than merely untidy.
 * 2. The substitution really does close the caller's original descriptor
 *    (dup_fd_below_fdsetsize() returns a different, in-range number and the
 *    original becomes EBADF).
 * 3. The contract holds end to end: after a full copy_stream_ex() over a
 *    descriptor above FD_SETSIZE, fd_closed is set, and the descriptor the
 *    caller passed in is already gone - so a caller that honours the flag
 *    performs no stale close.  A caller that ignores it (the old behaviour)
 *    would get EBADF, which the test demonstrates explicitly.
 * 4. The normal case is unaffected: for in-range descriptors the flags stay
 *    clear and the caller remains the owner.
 *
 * The test is skipped (exit 77) when RLIMIT_NOFILE cannot be raised above
 * FD_SETSIZE, since the out-of-range path is then unreachable.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <sys/resource.h>

#define SKIP 77

static int fd_is_closed(int fd)
{
	errno = 0;
	return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

/* (1) Descriptor numbers are recycled immediately: a stale close() can hit a
 * live, unrelated descriptor. */
static int test_numbers_are_recycled(void)
{
	int a, b;

	a = open("/dev/null", O_RDWR);
	if (a < 0)
	{
		perror("open");
		return 1;
	}
	(void)close(a);
	b = open("/dev/null", O_RDWR);
	if (b < 0)
	{
		perror("open");
		return 1;
	}
	if (a != b)
	{
		fprintf(stderr,
		        "  note: number not recycled here (%d vs %d); the hazard still\n"
		        "  exists in general, continuing\n", a, b);
		(void)close(b);
		return 0;
	}
	/* A stale close() of `a` would now destroy the live descriptor `b`. */
	(void)close(a);
	if (!fd_is_closed(b))
	{
		fprintf(stderr, "FAIL: expected the stale close to destroy fd %d\n", b);
		return 1;
	}
	fprintf(stderr,
	        "  [1] OK: number %d was recycled and a stale close() destroyed the\n"
	        "      live descriptor that inherited it\n", b);
	return 0;
}

/* (2) The substitution closes the caller's original descriptor. */
static int test_substitution_closes_original(void)
{
	int high, low;

	high = open("/dev/null", O_RDWR);
	if (high < 0)
	{
		perror("open");
		return 1;
	}
	if (dup2(high, FD_SETSIZE + 3) < 0)
	{
		(void)close(high);
		fprintf(stderr, "  skip: cannot create a descriptor above FD_SETSIZE\n");
		return SKIP;
	}
	(void)close(high);
	high = FD_SETSIZE + 3;

	low = dup_fd_below_fdsetsize(high, "test");
	if (low < 0)
	{
		fprintf(stderr, "FAIL: dup_fd_below_fdsetsize failed\n");
		return 1;
	}
	if (!FD_VALID(low))
	{
		fprintf(stderr, "FAIL: returned fd %d is not selectable\n", low);
		(void)close(low);
		return 1;
	}
	if (low == high)
	{
		fprintf(stderr, "FAIL: expected a different descriptor number\n");
		(void)close(low);
		return 1;
	}
	if (!fd_is_closed(high))
	{
		fprintf(stderr,
		        "FAIL: original fd %d is still open; it would leak\n", high);
		(void)close(low);
		(void)close(high);
		return 1;
	}
	fprintf(stderr,
	        "  [2] OK: fd %d was replaced by in-range fd %d and the original was\n"
	        "      closed by the substitution\n", high, low);
	(void)close(low);
	return 0;
}

/* (3) End-to-end: the fd_closed contract is reported and is accurate. */
static int test_contract_reported(void)
{
	int net[2], prn[2];
	int hi_net;
	int net_closed = -1, lp_closed = -1;
	pid_t client, printer;
	int rc;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, net) != 0 ||
	    socketpair(AF_UNIX, SOCK_STREAM, 0, prn) != 0)
	{
		perror("socketpair");
		return 1;
	}

	hi_net = FD_SETSIZE + 7;
	if (dup2(net[0], hi_net) < 0)
	{
		fprintf(stderr, "  skip: cannot place a socket above FD_SETSIZE\n");
		return SKIP;
	}
	(void)close(net[0]);

	bidir = 1;
	log_to_stdout = 0;
	(void)alarm(60);

	client = fork();
	if (client < 0)
	{
		perror("fork");
		return 1;
	}
	if (client == 0)
	{
		/* Drop every descriptor the daemon side owns, otherwise the peer never
		 * observes EOF and copy_stream_ex() would block forever. */
		(void)close(hi_net);
		(void)close(prn[0]);
		(void)close(prn[1]);
		if (write(net[1], "job", 3) != 3)
			_exit(2);
		(void)usleep(100000);
		(void)close(net[1]); /* FIN ends the job */
		_exit(0);
	}

	printer = fork();
	if (printer < 0)
	{
		perror("fork");
		return 1;
	}
	if (printer == 0)
	{
		/*
		 * Play the printer: push a status line back to the daemon, then close
		 * the write half so the daemon observes EOF on its printer-readable
		 * descriptor.  Exit on our own - we must not block waiting for the
		 * daemon to write to us, because in bidirectional mode the daemon only
		 * READS from the printer, it never writes to prn[1].
		 */
		(void)close(hi_net);
		(void)close(net[1]);
		(void)close(prn[0]);
		if (write(prn[1], "ready", 5) != 5)
			_exit(2);
		(void)shutdown(prn[1], SHUT_WR);
		(void)usleep(100000);
		_exit(0);
	}

	(void)close(net[1]);
	(void)close(prn[1]);

	rc = copy_stream_ex(hi_net, prn[0], &net_closed, &lp_closed);

	(void)waitpid(client, NULL, 0);
	(void)waitpid(printer, NULL, 0);

	if (rc != 0)
	{
		fprintf(stderr, "FAIL: copy_stream_ex returned %d\n", rc);
		return 1;
	}
	if (net_closed != 1)
	{
		fprintf(stderr,
		        "FAIL: net_closed=%d, expected 1 (fd %d was above FD_SETSIZE and\n"
		        "      must have been reported as already closed)\n",
		        net_closed, hi_net);
		return 1;
	}
	if (!fd_is_closed(hi_net))
	{
		fprintf(stderr, "FAIL: fd %d should already be closed\n", hi_net);
		return 1;
	}
	/* The old code closed it again here; show that this is the EBADF that
	 * could instead have hit a recycled, unrelated descriptor. */
	errno = 0;
	if (close(hi_net) == 0)
	{
		fprintf(stderr, "FAIL: stale close(%d) unexpectedly succeeded\n", hi_net);
		return 1;
	}
	if (errno != EBADF)
	{
		fprintf(stderr, "FAIL: stale close(%d) gave %s, expected EBADF\n",
		        hi_net, strerror(errno));
		return 1;
	}
	/* prn[0] was in range, so the caller still owns it. */
	if (lp_closed != 0)
	{
		fprintf(stderr, "FAIL: lp_closed=%d, expected 0 for an in-range fd\n",
		        lp_closed);
		return 1;
	}
	if (close(prn[0]) != 0)
	{
		fprintf(stderr, "FAIL: caller should still own the in-range printer fd\n");
		return 1;
	}
	fprintf(stderr,
	        "  [3] OK: fd_closed=1 reported for the out-of-range network fd (a\n"
	        "      caller honouring it performs no stale close; ignoring it gives\n"
	        "      EBADF), lp_closed=0 for the in-range printer fd\n");
	return 0;
}

/* (4) The ordinary in-range case must not change ownership. */
static int test_inrange_unchanged(void)
{
	int net[2], prn[2];
	int net_closed = -1, lp_closed = -1;
	pid_t client, printer;
	int rc;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, net) != 0 ||
	    socketpair(AF_UNIX, SOCK_STREAM, 0, prn) != 0)
	{
		perror("socketpair");
		return 1;
	}

	bidir = 1;
	log_to_stdout = 0;
	(void)alarm(60);

	client = fork();
	if (client < 0)
		return 1;
	if (client == 0)
	{
		(void)close(net[0]);
		(void)close(prn[0]);
		(void)close(prn[1]);
		if (write(net[1], "job", 3) != 3)
			_exit(2);
		(void)usleep(100000);
		(void)close(net[1]);
		_exit(0);
	}
	printer = fork();
	if (printer < 0)
		return 1;
	if (printer == 0)
	{
		(void)close(net[0]);
		(void)close(net[1]);
		(void)close(prn[0]);
		if (write(prn[1], "ready", 5) != 5)
			_exit(2);
		(void)shutdown(prn[1], SHUT_WR);
		(void)usleep(100000);
		_exit(0);
	}
	(void)close(net[1]);
	(void)close(prn[1]);

	rc = copy_stream_ex(net[0], prn[0], &net_closed, &lp_closed);
	(void)waitpid(client, NULL, 0);
	(void)waitpid(printer, NULL, 0);

	if (rc != 0)
	{
		fprintf(stderr, "FAIL: copy_stream_ex returned %d for in-range fds\n", rc);
		return 1;
	}
	if (net_closed != 0 || lp_closed != 0)
	{
		fprintf(stderr, "FAIL: in-range fds reported as closed (%d/%d)\n",
		        net_closed, lp_closed);
		return 1;
	}
	if (close(net[0]) != 0 || close(prn[0]) != 0)
	{
		fprintf(stderr, "FAIL: caller lost ownership of in-range descriptors\n");
		return 1;
	}
	fprintf(stderr,
	        "  [4] OK: in-range descriptors stay owned by the caller (flags clear,\n"
	        "      both closable exactly once)\n");
	return 0;
}

int main(void)
{
	struct rlimit rl;
	int r;

	if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
	{
		rl.rlim_cur = rl.rlim_max;
		(void)setrlimit(RLIMIT_NOFILE, &rl);
	}
	if (getrlimit(RLIMIT_NOFILE, &rl) != 0 ||
	    rl.rlim_cur <= (rlim_t)FD_SETSIZE + 16)
	{
		fprintf(stderr, "SKIP: RLIMIT_NOFILE too low to exceed FD_SETSIZE\n");
		return SKIP;
	}

	if ((r = test_numbers_are_recycled()) != 0)
		return r;
	if ((r = test_substitution_closes_original()) != 0)
		return r == SKIP ? SKIP : r;
	if ((r = test_contract_reported()) != 0)
		return r == SKIP ? SKIP : r;
	if ((r = test_inrange_unchanged()) != 0)
		return r;

	fprintf(stderr,
	        "PASS: copy_stream_ex() reports descriptor ownership correctly, so the\n"
	        "caller never performs a stale close that could hit a recycled fd\n");
	return 0;
}
