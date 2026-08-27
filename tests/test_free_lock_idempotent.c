/*
 * Regression test: free_lock() must be idempotent -- after closing lockfd it
 * must reset the global to -1 so the descriptor number cannot be closed twice
 * (which would otherwise hit an unrelated, recycled descriptor).
 *
 * Bug
 * ---
 * free_lock() closed lockfd but did not reset it to -1.  Because lockfd is a
 * global, a second free_lock() call -- or any future code that reuses the
 * variable after a release -- would call close() on a stale descriptor number.
 * If that number had been recycled by the kernel for an unrelated open file,
 * the second close() would silently tear down that unrelated file.  The fix
 * sets lockfd = -1 after closing, matching the failure branch of get_lock().
 *
 * Proof strategy
 * --------------
 * 1. Take the lock (get_lock), record lockfd.
 * 2. Call free_lock() once, then call free_lock() a SECOND time.
 * 3. Before the fix: lockfd stays >= 0 after the first free_lock(), so the
 *    second free_lock() closes the same number.  If we open /dev/null right
 *    after the first free_lock(), the kernel is likely to reuse that exact fd;
 *    the second free_lock() then closes our /dev/null descriptor, and an fcntl
 *    probe on it returns EBADF -> test FAILS.
 * 4. After the fix: free_lock() resets lockfd = -1, so the second call is a
 *    no-op and our /dev/null descriptor stays valid -> test PASSES.
 *
 * To make the test deterministic regardless of fd-allocation luck, we perform
 * the double free_lock() and then check: if a probe fd opened after the first
 * free_lock() landed on the old lockfd number, it must still be valid after
 * the second free_lock().  We also directly assert lockfd == -1 after a single
 * free_lock().
 */
#define _GNU_SOURCE
#define LOCKFILE_DIR "/tmp/p910nd_lk6"
#define main p910nd_free_lock_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int main(void)
{
	char lockname[sizeof(LOCKFILE)];
	int saved, probe, r;
	int rc = 0;

	assert(mkdir(LOCKFILE_DIR, 0755) == 0 || errno == EEXIST);
	(void)snprintf(lockname, sizeof(lockname), LOCKFILE, '0');
	(void)unlink(lockname);

	assert(get_lock('0') == 1);
	saved = lockfd;
	fprintf(stderr, "lockfd after get_lock = %d\n", saved);
	assert(saved >= 0);

	/* First release. */
	free_lock();
	fprintf(stderr, "lockfd after free_lock = %d (must be -1 after fix)\n", lockfd);
	if (lockfd != -1)
	{
		fprintf(stderr,
		        "FAIL: free_lock() did not reset lockfd to -1; stale descriptor %d\n",
		        lockfd);
		rc = 1;
	}

	/* Open something that is likely to reuse the just-freed fd number. */
	probe = open("/dev/null", O_RDWR);
	fprintf(stderr, "probe fd = %d (old lockfd = %d)\n", probe, saved);

	/* Second release -- must be a harmless no-op. */
	free_lock();

	if (probe == saved)
	{
		r = fcntl(probe, F_GETFD);
		if (r == -1)
		{
			fprintf(stderr,
			        "FAIL: second free_lock() closed recycled descriptor %d "
			        "(unrelated /dev/null)\n",
			        probe);
			rc = 1;
		}
		else
		{
			fprintf(stderr,
			        "OK: probe fd %d still valid after second free_lock()\n",
			        probe);
		}
	}
	else
	{
		fprintf(stderr,
		        "OK: probe fd %d != old lockfd %d; cannot collide, double-free harmless\n",
		        probe, saved);
	}

	(void)close(probe);
	(void)unlink(lockname);
	(void)rmdir(LOCKFILE_DIR);

	if (rc == 0)
		fprintf(stderr, "PASS: free_lock() is idempotent, lockfd reset to -1\n");
	return rc;
}
