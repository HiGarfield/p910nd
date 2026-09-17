/*
 * Regression test for Bug #1: in (x)inetd mode one_job() used to simply
 * `return` when get_lock() failed (e.g. the lock file could not be opened),
 * leaving an ambiguous, half-handled network connection (descriptor 0)
 * instead of terminating like server() does.
 *
 * Note on semantics: get_lock() uses the blocking F_SETLKW variant, so a lock
 * HELD by another instance makes the caller wait (serialising jobs) rather
 * than fail -- that "blocking is allowed" path is fine.  The failure branch
 * (get_lock() == 0) is only reached when the lock file itself cannot be
 * opened/locked, which is the error condition the old `return` mishandled.
 *
 * Fix: one_job() now calls exit(1) when get_lock() fails, so inetd sees a
 * definite exit status and the kernel releases descriptor 0 on exit.
 *
 * This test makes the lock file impossible to open so get_lock() returns 0.
 * It then runs one_job() and asserts the process exits with status 1
 * (terminate) rather than returning and letting main() return 0 (the old
 * silent-return bug).
 *
 * The lock is made unobtainable by putting an ordinary FILE in the middle of
 * the path, so no directory can ever be created there.  A merely MISSING
 * directory used to be enough, but that stopped being true once get_lock()
 * began creating its directory (BUG-001): the missing directory then became
 * creatable, the lock succeeded, and one_job() went on to block forever in
 * its open_printer() retry loop.  A blocker file keeps this test aimed at the
 * lock-failure branch instead of depending on that behaviour change.
 */
#define _GNU_SOURCE
#define LOCKFILE_BLOCKER "/tmp/p910nd_onejob_lock_blocker_xyz"
#define LOCKFILE_DIR LOCKFILE_BLOCKER "/sub"
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <fcntl.h>

int main(void)
{
	pid_t child;
	int status;

	/* Replace stdin (descriptor 0) with /dev/null so one_job()'s
	 * getpeername(0,...) does not blow up; we only care about the lock path. */
	{
		int dn = open("/dev/null", O_RDWR);
		int blocker;
		assert(dn >= 0);
		assert(dup2(dn, 0) == 0);
		(void)close(dn);
		/* A plain file where the lock directory has to go: creating it is
		 * impossible, so open() of the lock file can never succeed. */
		blocker = open(LOCKFILE_BLOCKER, O_CREAT | O_WRONLY, 0644);
		assert(blocker >= 0);
		(void)close(blocker);
	}

	log_to_stdout = 0;
	child = fork();
	assert(child >= 0);
	if (child == 0)
	{
		/* one_job() must call exit(1) because get_lock() fails to open the
		 * lock file (directory absent). */
		one_job('0');
		/* If it ever returns here, that is the bug. */
		_exit(0);
	}
	assert(waitpid(child, &status, 0) == child);

	if (!WIFEXITED(status))
	{
		fprintf(stderr, "FAIL: one_job() did not exit normally (status=0x%x)\n", status);
		return 1;
	}
	if (WEXITSTATUS(status) != 1)
	{
		fprintf(stderr,
		        "FAIL: one_job() exited with %d, expected 1 (lock failure must exit(1), not return)\n",
		        WEXITSTATUS(status));
		return 1;
	}
	fprintf(stderr, "PASS: one_job() exits with status 1 on lock failure (no silent return)\n");
	return 0;
}
