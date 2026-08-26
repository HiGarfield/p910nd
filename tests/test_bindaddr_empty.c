/*
 * Regression test for Bug #3: an empty -i bind address was passed straight to
 * getaddrinfo(), which silently treats "" as "bind to all interfaces" and
 * overrides the operator's intent.  The fix rejects an empty -i with usage()
 * and a non-zero exit.
 *
 * This test invokes main() with argv = {"p910nd","-i",""} and asserts the
 * process exits with a non-zero status (a regression to the silent pass would
 * let main() proceed and eventually return 0 from server()/one_job()).
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>

static int run_test(void)
{
	pid_t child;
	int status;
	char *argv[] = {"p910nd", "-i", "", NULL};

	child = fork();
	assert(child >= 0);
	if (child == 0)
	{
		/*
		 * main() reads globals (bidir, device, bindaddr, ...) that are
		 * process-wide; reset the relevant ones so a prior run cannot leak
		 * state.  main() will re-parse argv and reject the empty -i.
		 */
		bindaddr = NULL;
		device = NULL;
		bidir = 0;
		log_to_stdout = 0;
		(void)signal(SIGPIPE, SIG_IGN);
		(void)p910nd_original_main(3, argv);
		/* Should not reach here; if it does, that is the bug. */
		_exit(0);
	}
	assert(waitpid(child, &status, 0) == child);

	if (!WIFEXITED(status))
	{
		fprintf(stderr, "FAIL: main() did not exit normally (status=0x%x)\n", status);
		return 1;
	}
	if (WEXITSTATUS(status) == 0)
	{
		fprintf(stderr, "FAIL: main() accepted an empty -i bind address (exit 0)\n");
		return 1;
	}
	fprintf(stderr, "PASS: empty -i bind address rejected (main exited %d)\n",
	        WEXITSTATUS(status));
	return 0;
}

int main(void)
{
	return run_test();
}
