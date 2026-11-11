/*
 * LD_PRELOAD shim that makes getsockname() fail on descriptor 0.
 *
 * P910ND_BREAK_GETSOCKNAME makes getsockname(0, ...) return -1/EINVAL while
 * fd 0 is still a real socket, reproducing an (x)inetd handover whose socket
 * name cannot be read.  Other descriptors and other processes are untouched.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>

static int (*real_getsockname)(int, struct sockaddr *, socklen_t *);
static int enabled = -1;

int getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
	if (real_getsockname == NULL)
		real_getsockname = dlsym(RTLD_NEXT, "getsockname");
	if (enabled < 0)
		enabled = (getenv("P910ND_BREAK_GETSOCKNAME") != NULL);
	if (enabled && fd == 0) {
		errno = EINVAL;
		return -1;
	}
	return real_getsockname(fd, addr, len);
}
