/*
 * LD_PRELOAD shim that makes accept() fail with a transient errno.
 *
 * P910ND_ACCEPT_FAILURES=N makes the first N accept() calls in this process
 * return ENOSR ("out of streams resources"), an error that is neither a
 * connection abort nor a resource shortage and is not in the daemon's
 * explicit accept() list.  After N calls the real accept() is used, so the
 * daemon can be checked for serving clients again afterwards.
 *
 * The shim is inert without the environment variable.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>

static int (*real_accept)(int, struct sockaddr *, socklen_t *);
static int remaining = -1;

int accept(int fd, struct sockaddr *addr, socklen_t *len)
{
	const char *s;

	if (real_accept == NULL)
		real_accept = dlsym(RTLD_NEXT, "accept");
	if (remaining < 0) {
		s = getenv("P910ND_ACCEPT_FAILURES");
		remaining = (s != NULL) ? atoi(s) : 0;
	}
	if (remaining > 0) {
		remaining--;
		errno = ENOSR;
		return -1;
	}
	return real_accept(fd, addr, len);
}
