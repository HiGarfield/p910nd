/*
 * Regression test for Bug #1 / Bug #2: every exit(1) path in server() that
 * runs after a successful get_lock() must call free_lock() first, otherwise
 * the file lock (and its on-disk lockfile) is left dangling and a restarted
 * daemon blocks forever in F_SETLKW.
 *
 * The unit under test is the lock acquire/release pair (get_lock/free_lock).
 * fcntl(F_SETLKW) write locks do NOT conflict within the same process, so a
 * single-process test cannot observe the leak; instead we verify the *effect*
 * of free_lock(): before it, an independent process cannot take the lock;
 * after it, an independent process can.  That is exactly what the fixed
 * exit paths now guarantee (they call free_lock() before exit(1)).
 *
 * Compiled with -DLOCKFILE_DIR pointing at a temp dir so we don't touch
 * system lock directories and so the test is hermetic.
 */
#define _GNU_SOURCE
#define LOCKFILE_DIR "/tmp/p910nd_lock_test"
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

/* Try to take the write lock in a fresh process via non-blocking fcntl.
 * Returns 0 if the lock was obtained (free), non-zero if blocked (held). */
static int try_lock_held(void)
{
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0)
	{
		char lockname[sizeof(LOCKFILE)];
		int fd;
		struct flock lk;
		(void)snprintf(lockname, sizeof(lockname), LOCKFILE, '0');
		fd = open(lockname, O_CREAT | O_RDWR, 0644);
		if (fd < 0)
			_exit(2);
		memset(&lk, 0, sizeof(lk));
		lk.l_type = F_WRLCK;
		lk.l_pid = getpid();
		/* Non-blocking: succeeds only if no other process holds it. */
		if (fcntl(fd, F_SETLK, &lk) == 0)
			_exit(0); /* lock free -> we got it */
		_exit(1);     /* lock held by someone else */
	}
	else
	{
		int status;
		assert(waitpid(pid, &status, 0) == pid);
		if (!WIFEXITED(status))
			return 2;
		return WEXITSTATUS(status);
	}
}

int main(void)
{
	char dir[] = "/tmp/p910nd_lock_test";
	char lockname[sizeof(LOCKFILE)];
	int st;

	assert(mkdir(dir, 0755) == 0 || errno == EEXIST);
	(void)snprintf(lockname, sizeof(lockname), LOCKFILE, '0');
	/* Start clean so a stale lock from a prior failed run can't mask the bug. */
	(void)unlink(lockname);

	/* Acquire the lock the same way server() does after daemonizing. */
	assert(get_lock('0') == 1);

	/* An independent process must NOT be able to take it now. */
	st = try_lock_held();
	assert(st == 1); /* held */

	/* This is what the fixed exit(1) paths now do before giving up. */
	free_lock();

	/* After release, an independent process must be able to take it. */
	st = try_lock_held();
	assert(st == 0); /* free */

	(void)unlink(lockname);
	(void)rmdir(dir);
	fprintf(stderr, "PASS: get_lock/free_lock pair releases lock for other processes\n");
	return 0;
}
