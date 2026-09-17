/*
 * get_lock() must survive a signal that interrupts the blocking flock wait.
 *
 * F_SETLKW blocks until the printer lock can be granted.  While it waits it
 * can return -1/EINTR if a signal arrives.  The original code treated any
 * fcntl() failure as fatal, so a single signal delivered at the wrong moment
 * made the daemon give up on a lock it could have taken on the next attempt:
 * under (x)inetd, where several instances can be waiting for their turn, that
 * either killed a daemon or let one proceed believing it was alone.
 *
 * This test reproduces the situation deterministically:
 *   - a child process takes the printer lock and holds it for five seconds;
 *   - the parent installs a SIGALRM handler WITHOUT SA_RESTART, arms a one
 *     second alarm and then calls get_lock(), so the alarm fires while it is
 *     blocked inside F_SETLKW;
 *   - get_lock() must retry the interrupted wait and succeed once the child
 *     releases the lock.
 *
 * Without the fix get_lock() returns failure immediately, so this fails fast
 * rather than hanging.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t alarm_seen = 0;

static void on_alarm(int sig)
{
	(void)sig;
	alarm_seen = 1;
}

int main(void)
{
	pid_t holder;
	int status = 0;
	struct sigaction sa;

	log_to_stdout = 0;

	holder = fork();
	assert(holder >= 0);
	if (holder == 0)
	{
		/* Take the lock, hold it, then release it and report success. */
		if (get_lock(0) == 0)
			_exit(2);
		sleep(5);
		free_lock();
		_exit(0);
	}

	/* Give the child time to actually acquire the lock. */
	sleep(1);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_alarm;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0; /* deliberately NOT SA_RESTART */
	assert(sigaction(SIGALRM, &sa, NULL) == 0);

	/* Fire once while get_lock() is blocked waiting for the holder. */
	alarm(1);

	/*
	 * The alarm lands inside F_SETLKW.  The interrupted wait must be
	 * retried, so this blocks until the child releases the lock and then
	 * succeeds.
	 */
	assert(get_lock(0) == 1);
	assert(alarm_seen == 1);
	free_lock();

	assert(waitpid(holder, &status, 0) == holder);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	return 0;
}
