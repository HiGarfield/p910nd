/*
 * LD_PRELOAD shim that makes poll() report POLLNVAL for every descriptor.
 *
 * P910ND_POLLNVAL=1 simulates listening descriptors that have become
 * invalid (closed behind the daemon's back).  The accept loop must give up
 * after a short bounded retry instead of spinning forever on a set of
 * sockets that can never accept another client.
 *
 * The shim is inert without the environment variable: it forwards to the
 * real poll() so it is safe to load in helper processes too.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <poll.h>
#include <stdlib.h>

static int (*real_poll)(struct pollfd *, nfds_t, int);

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	nfds_t i;
	const char *flag;

	if (real_poll == NULL)
		real_poll = dlsym(RTLD_NEXT, "poll");
	flag = getenv("P910ND_POLLNVAL");
	if (flag == NULL || *flag == '\0')
		return real_poll(fds, nfds, timeout);
	(void)timeout;
	for (i = 0; i < nfds; i++)
		fds[i].revents = POLLNVAL;
	return (int)nfds;
}
