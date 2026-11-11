/*
 * Behaviour probe for the MAX_JOB_CHILDREN accounting under OOM.
 *
 * p910nd.c is included so record_child() and sigchld_handler() are reachable.
 * calloc() is forced to fail, so the child cannot be stored in the pid table,
 * and waitpid() is stubbed to reap exactly that untracked child.  The count
 * must still rise at fork time and fall again when the child exits, or the
 * cap would stop seeing the very processes it exists to bound.
 *
 * Exit 0 means both halves of the accounting are correct.
 */
#define main p910nd_main_unused
#include "p910nd.c"
#undef main

static int waitpid_calls;

void *calloc(size_t n, size_t s)
{
	(void)n;
	(void)s;
	return NULL;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
	(void)pid;
	(void)status;
	(void)options;
	if (waitpid_calls++ == 0)
		return (pid_t)4242;
	return (pid_t)0;
}

int main(void)
{
	int before = (int)inflight_children;
	int rc = record_child(4242);
	int after_record = (int)inflight_children;

	sigchld_handler(SIGCHLD);
	printf("rc=%d before=%d after_record=%d after_reap=%d\n",
	       rc, before, after_record, (int)inflight_children);
	return (rc == 0 && after_record == before + 1 &&
	        (int)inflight_children == before) ? 0 : 1;
}
