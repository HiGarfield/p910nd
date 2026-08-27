/*
 *	Port 9100+n daemon
 *	Accepts a connection from port 9100+n and copy stream to
 *	/dev/lpn, where n = 0..9.
 *
 *	GPLv2 license, read COPYING
 *
 *	Run standalone as: p910nd [0|...|9]
 *
 *	Run under inetd as:
 *	p910n stream tcp nowait root /usr/sbin/tcpd p910nd [0|...|9]
 *	 where p910n is an /etc/services entry for
 *	 port 9100 through 9109 as the case may be.
 *	 root can be replaced by any uid with rw permission on /dev/lpn
 *
 *	Port 9100+n will then be passively opened
 *	n defaults to 0
 *
 *	Version 0.97
 *	Patches by Stefan Sichler.
 *	Stream to printer is only closed after EOF from network if
 *	it is no more busy, otherwise printer driver may discard data of last
 *	write() at close().
 *
 *	Version 0.96
 *	Patches by Stefan Sichler.
 *	Fixed bi-directional mode to stay alive until connection is closed.
 *	Fixed timeout value in select() (controlling in/out balancing).
 *	Fixed network-to-printer buffer reporting EOF although there was data
 *	left in the buffer.
 *	Added log-to-stdout option (-d).
 *
 *	Version 0.95
 *	Patch by Mario Izquierdo
 *	Fix incomplete conversion to manipulate new ip_addr structure
 *	when LIBWRAP is selected
 *
 *	Version 0.94
 *	Patch by Guenther Niess:
 *	Support IPv6
 *	Patch by Philip Prindeville:
 *	Increase socket buffer size
 *	Use %hu for printing port
 *	Makefile fixes for LIBWRAP
 *
 *	Version 0.93
 *	Fix open call to include mode, required for O_CREAT
 *
 *	Version 0.92
 *	Patches by Dave Brown.  Use raw I/O syscalls instead of
 *	stdio buffering.  Buffer system to handle talkative bidi
 *	devices better on low-powered hosts.
 *
 *	Version 0.91
 *	Patch by Hans Harder.  Close printer device after each use to
 *	avoid crashing when hotpluggable devices going away.
 *	Don't wait 10 seconds after successful open.
 *
 *	Version 0.9
 *	Patch by Kostas Liakakis to keep retrying every 10 seconds
 *	if EBUSY is returned by open_printer, apparently NetBSD
 *	does this if the printer is not on.
 *	Patch by Albert Bartoszko (al_bin@vp_pl), August 2006
 *	Work with hotpluggable devices
 *	Improve Makefile
 *
 *	(The last two patches conflict somewhat, Liakakis's patch
 *	retries opening the device every 10 seconds until successful,
 *	whereas Bartoszko's patch exits if the printer device cannot be opened.
 *	The problem is with a hotpluggable device, that device node may
 *	not appear again.
 *
 *	I have opted for Liakakis's behaviour. Let me know if this can
 *	be improved. Perhaps we need another option that chooses the
 *	behaviour. - Ken)
 *
 *	Version 0.8
 *	Allow specifying address to bind to
 *
 *	Version 0.7
 *	Bidirectional data transfer
 *
 *	Version 0.6
 *	Arne Bernin fixed some cast warnings, corrected the version number
 *	and added a -v option to print the version.
 *
 *	Version 0.5
 *	-DUSE_LIBWRAP and -lwrap enables hosts_access (tcpwrappers) checking.
 *
 *	Version 0.4
 *	Ken Yap (greenpossum@users.sourceforge.net), April 2001
 *	Placed under GPL.
 *
 *	Added -f switch to specify device which overrides /dev/lpn.
 *	But number is still required get distinct ports and locks.
 *
 *	Added locking so that two invocations of the daemon under inetd
 *	don't try to open the printer at the same time. This can happen
 *	even if there is one host running clients because the previous
 *	client can exit after it has sent all data but the printer has not
 *	finished printing and inetd starts up a new daemon when the next
 *	request comes in too soon.
 *
 *	Various things could be Linux specific. I don't
 *	think there is much demand for this program outside of PCs,
 *	but if you port it to other distributions or platforms,
 *	I'd be happy to receive your patches.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <netdb.h>
#include <syslog.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
/*
 * get_port() uses uint16_t, and POSIX declares the exact-width integer types in
 * <stdint.h>.  Relying on <netinet/in.h> to expose them is not portable: that
 * header is only required to define in_port_t/in_addr_t plus the types it needs
 * for its own structures, and it may satisfy that through private
 * implementation headers rather than by making <stdint.h>'s namespace visible.
 * glibc, for instance, pulls in <bits/stdint-uintn.h>, so the name happens to
 * be available here but is not guaranteed to be on another C library.  Include
 * the standard header that actually owns the type so the translation unit does
 * not depend on another header's internals.
 */
#include <stdint.h>

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define FD_VALID(fd) ((fd) >= 0 && (fd) < FD_SETSIZE)

#ifdef USE_LIBWRAP
#include "tcpd.h"
int allow_severity, deny_severity;
extern int hosts_ctl(char *daemon, char *client_name, char *client_addr, char *client_user);
#endif

#define BASEPORT 9100
#define PIDFILE "/var/run/p910%cd.pid"
#ifdef LOCKFILE_DIR
#define LOCKFILE LOCKFILE_DIR "/p910%cd"
#else
#define LOCKFILE "/var/lock/subsys/p910%cd"
#endif
#ifndef PRINTERFILE
#define PRINTERFILE "/dev/lp%c"
#endif
#define LOGOPTS LOG_ERR

#ifndef BUFFER_SIZE
#define BUFFER_SIZE 8192
#endif

/* Idle timeout after which a bidirectional job with no pending data in
 * either direction is considered finished.  Configurable so tests can
 * exercise the drain logic quickly.
 *
 * This grace window also bounds how long the daemon keeps a job open AFTER
 * the network has sent EOF, to forward any printer response that arrives only
 * once the host has closed its send side (a common request/reply timing).
 * A printer that never responds simply adds at most this much latency to job
 * completion; a printer that does respond gets the full window to finish its
 * reply.  5 seconds is long enough for essentially any printer to answer a
 * just-completed job while keeping the cost for silent printers small. */
#ifndef IDLE_TIMEOUT_SEC
#define IDLE_TIMEOUT_SEC 5
#endif

/* Circular buffer used for each direction. */
typedef struct
{
	int detectEof; /* If nonzero, EOF is marked when read returns 0 bytes. */
	int infd;	   /* Input file descriptor. */
	int outfd;	   /* Output file descriptor. */
	int startidx;  /* Index of the start of valid data. */
	int endidx;	   /* Index of the end of valid data. */
	int bytes;	   /* The number of bytes currently buffered. */
	unsigned long totalin;   /* Total bytes that have been read. */
	unsigned long totalout;  /* Total bytes that have been written. */
	int eof_read;  /* Nonzero indicates the input file has reached EOF. */
	int eof_sent;  /* Nonzero indicates the output file has fully received all data. */
	int err;	   /* Nonzero indicates an error detected on the output file. */
#define READ_ERR 0x01
#define WRITE_ERR 0x02
	char buffer[BUFFER_SIZE]; /* Buffered data goes here. */
} Buffer_t;

static char *progname;
/* Writable fallback name; a string literal must not be used because
 * main() later overwrites the "p910n" prefix in place. */
static char default_progname[] = "p910nd";
static char version[] = "Version 0.97";
static char copyright[] = "Copyright (c) 2008-2014 Ken Yap and others, GPLv2";
static int lockfd = -1;
static char *device = NULL;
static int bidir = 0;
static char *bindaddr = NULL;
static int log_to_stdout = 0;

/* Helper function: convert a struct sockaddr address (IPv4 and IPv6) to a string */
static char *get_ip_str(const struct sockaddr *sa, char *s, socklen_t maxlen)
{
	if (maxlen > 0)
		s[0] = '\0';
	switch (sa->sa_family)
	{
	case AF_INET:
		if (!inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr), s, maxlen))
			snprintf(s, maxlen, "<inet_ntop error>");
		break;
	case AF_INET6:
		if (!inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr), s, maxlen))
			snprintf(s, maxlen, "<inet_ntop error>");
		break;
	default:
		snprintf(s, maxlen, "Unknown AF");
	}
	return s;
}

static uint16_t get_port(const struct sockaddr *sa)
{
	uint16_t port;
	switch (sa->sa_family)
	{
	case AF_INET:
		port = ntohs(((struct sockaddr_in *)sa)->sin_port);
		break;
	case AF_INET6:
		port = ntohs(((struct sockaddr_in6 *)sa)->sin6_port);
		break;
	default:
		return 0;
	}
	return port;
}

static void usage(void)
{
	fprintf(stderr, "%s %s %s\n", progname, version, copyright);
	fprintf(stderr, "Usage: %s [-f device] [-i bindaddr] [-bvd] [0|...|9]\n", progname);
	exit(1);
}

static void show_version(void)
{
	fprintf(stdout, "%s %s\n", progname, version);
}

static void dolog(int level, const char *msg, ...)
{
	va_list argp;
	va_start(argp, msg);
	if (log_to_stdout)
	{
		vfprintf(stdout, msg, argp);
		fflush(stdout);
	}
	else if (level != LOG_DEBUG)
		vsyslog(level, msg, argp);
	va_end(argp);
}

static int open_printer(int lpnumber)
{
	int lp;
	static char lpname[sizeof(PRINTERFILE)];

	/*
	 * Under the TESTING build the device name is hard-coded to /dev/tty and
	 * lpnumber is not consumed by snprintf(), so mark it explicitly used to
	 * keep every build configuration (including -DTESTING) warning-free under
	 * -Wall -Wextra.  In the production build lpnumber is the %c argument.
	 */
	(void)lpnumber;

#ifdef TESTING
	(void)snprintf(lpname, sizeof(lpname), "/dev/tty");
#else
	(void)snprintf(lpname, sizeof(lpname), PRINTERFILE, lpnumber);
#endif
	if (device == NULL)
		device = lpname;
	/*
	 * Always open the printer with O_NONBLOCK.  For a unidirectional job the
	 * previous code opened O_WRONLY (blocking); with some device drivers (and
	 * notably with a FIFO used in place of a printer) open() blocks until a
	 * reader appears, which would hang the whole daemon -- it could neither
	 * accept new connections nor be shut down -- violating the requirement
	 * that a missing/again-unavailable printer must not deadlock the process.
	 * O_NONBLOCK makes open() return immediately (-1 with ENXIO/ENOENT) so the
	 * caller's retry loop can simply sleep and try again.
	 */
	if ((lp = open(device, bidir ? (O_RDWR | O_NONBLOCK) : (O_WRONLY | O_NONBLOCK))) == -1)
	{
		if (errno == EBUSY)
			dolog(LOGOPTS, "%s: %m, will try opening later\n", device);
		else
			dolog(LOGOPTS, "%s: %m\n", device);
	}
	return (lp);
}

/*
 * Put fd into non-blocking mode.
 *
 * The network descriptor arrives either from accept() or from (x)inetd on
 * stdin, and in both cases it is in blocking mode.  copy_stream() drives it
 * through select() and then calls read()/write() on it, but select() only
 * promises that *at least one* byte can be moved -- never that the whole
 * contiguous chunk computed from the circular buffer fits.  A blocking
 * descriptor therefore lets read() park the daemon even though it still has
 * buffered bytes to hand to a printer that is ready to accept them, which
 * stalls delivery for as long as the peer stays quiet (unbounded).  Making the
 * descriptor non-blocking turns those cases into EAGAIN, which readBuffer()
 * and writeBuffer() already treat as "no progress this round", so the loop
 * falls through to the other direction instead of blocking.
 */
static int set_nonblocking(int fd, const char *name)
{
	int flags = fcntl(fd, F_GETFL, 0);

	if (flags == -1)
	{
		dolog(LOGOPTS, "fcntl(F_GETFL, %s fd=%d): %m\n", name, fd);
		return -1;
	}
	if ((flags & O_NONBLOCK) == O_NONBLOCK)
		return 0;
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
	{
		dolog(LOGOPTS, "fcntl(F_SETFL O_NONBLOCK, %s fd=%d): %m\n", name, fd);
		return -1;
	}
	return 0;
}

/*
 * Block while the listening socket has no descriptor available to accept a new
 * connection on.  Called after accept() fails with a transient resource error
 * (EMFILE/ENFILE/ENOBUFS/ENOMEM).  A bare sleep(1) busy-spins at 100% CPU for
 * as long as the condition lasts, violating the requirement that the daemon
 * must never deadlock or spin; instead we wait in select() on the listening
 * socket with a bounded 1s timeout so the process consumes no CPU while a
 * descriptor frees up and still re-checks promptly once one becomes available.
 * Falls back to sleep(1) only when netfd is outside the select()-safe range.
 */
static void accept_backoff(int netfd)
{
	if (FD_VALID(netfd))
	{
		fd_set afds;
		struct timeval atv;
		FD_ZERO(&afds);
		FD_SET(netfd, &afds);
		atv.tv_sec = 1;
		atv.tv_usec = 0;
		(void)select(netfd + 1, &afds, NULL, NULL, &atv);
	}
	else
	{
		sleep(1);
	}
}

/* Duplicate fd into the select()-safe range [0, FD_SETSIZE). */
static int dup_fd_below_fdsetsize(int fd, const char *name)
{
	int target;

	if (FD_VALID(fd))
		return fd;

	for (target = 0; target < FD_SETSIZE; ++target)
	{
		if (fcntl(target, F_GETFD) == -1 && errno == EBADF)
		{
			if (dup2(fd, target) < 0)
			{
				dolog(LOGOPTS, "dup2(%s fd=%d -> fd=%d): %m\n", name, fd, target);
				return -1;
			}
			/*
			 * Ownership of the descriptor transfers to the duplicate: the
			 * original fd is no longer watched by select() (it lies beyond
			 * FD_SETSIZE) and would otherwise leak.  Close it here so the
			 * caller only has to manage the returned (in-range) descriptor.
			 */
			(void)close(fd);
			dolog(LOG_DEBUG, "using duplicate %s fd=%d for select() (original=%d closed)\n",
			      name, target, fd);
			return target;
		}
	}

	errno = EMFILE;
	dolog(LOGOPTS,
	      "%s fd=%d is not selectable and no free descriptor exists below %d\n",
	      name, fd, FD_SETSIZE);
	return -1;
}

static int get_lock(int lpnumber)
{
	char lockname[sizeof(LOCKFILE)];
	struct flock lplock;

	(void)snprintf(lockname, sizeof(lockname), LOCKFILE, lpnumber);
	if ((lockfd = open(lockname, O_CREAT | O_RDWR, 0644)) < 0)
	{
		dolog(LOGOPTS, "%s: %m\n", lockname);
		return (0);
	}
	memset(&lplock, 0, sizeof(lplock));
	lplock.l_type = F_WRLCK;
	lplock.l_pid = getpid();
	if (fcntl(lockfd, F_SETLKW, &lplock) < 0)
	{
		dolog(LOGOPTS, "%s: %m\n", lockname);
		(void)close(lockfd);
		lockfd = -1;
		return (0);
	}
	return (1);
}

static void free_lock(void)
{
	if (lockfd >= 0)
	{
		(void)close(lockfd);
		/*
		 * Reset to -1 so free_lock() is idempotent and the descriptor number
		 * cannot be mistakenly closed again.  lockfd is a global; leaving a
		 * stale (already-closed) value would let a later free_lock() -- or any
		 * logic that reuses the variable -- close a recycled, unrelated
		 * descriptor.  This mirrors the failure branch of get_lock(), which
		 * already sets lockfd = -1 after closing.
		 */
		lockfd = -1;
	}
}

/* Initializes the buffer, at the start. */
static void initBuffer(Buffer_t *b, int infd, int outfd, int detectEof)
{
	memset(b, 0, sizeof(*b));
	b->detectEof = detectEof;
	b->infd = infd;
	b->outfd = outfd;
}

/* Sets the readfds and writefds (used by select) based on current buffer state. */
static void prepBuffer(Buffer_t *b, fd_set *readfds, fd_set *writefds)
{
	if (b->outfd >= 0 && b->outfd < FD_SETSIZE &&
		(!(b->err & WRITE_ERR)) &&
		!b->eof_sent &&
		(b->bytes != 0 || b->eof_read))
	{
		FD_SET(b->outfd, writefds);
	}
	if (b->infd >= 0 && b->infd < FD_SETSIZE &&
		!b->eof_read &&
		b->bytes < BUFFER_SIZE &&
		!(b->err & READ_ERR))
	{
		FD_SET(b->infd, readfds);
	}
}

/*
 * Reads data into a buffer from its input file.
 *
 * saw_zero_read (may be NULL) is set to 1 when read() actually returned 0,
 * i.e. a genuine end-of-stream on the input descriptor, and is left untouched
 * otherwise.  A plain return value of 0 cannot express this: it is also used
 * for "buffer full" and for the transient EAGAIN/EWOULDBLOCK/EINTR conditions.
 * Callers that must distinguish a PERMANENT end-of-stream (select() will keep
 * reporting the descriptor readable forever) from a device that is merely
 * quiescent right now need this extra signal -- notably the bidirectional
 * printer->network buffer, which runs with detectEof == 0 and therefore never
 * records EOF in eof_read by itself.
 */
static ssize_t readBufferEx(Buffer_t *b, int *saw_zero_read)
{
	size_t avail;
	ssize_t result = 0;
	/* Do not read once any error flag is set. */
	if (b->err & (READ_ERR | WRITE_ERR))
		return -1;
	/* Nothing more to read after EOF has been seen. */
	if (b->eof_read)
		return 0;
	if (b->bytes == 0)
	{
		/* The buffer is empty. */
		b->startidx = b->endidx = 0;
		avail = BUFFER_SIZE;
	}
	else if (b->bytes == BUFFER_SIZE)
	{
		/* The buffer is full. */
		avail = 0;
	}
	else if (b->endidx > b->startidx)
	{
		/* The buffer is not wrapped: from endidx to end of buffer is free. */
		avail = BUFFER_SIZE - b->endidx;
	}
	else
	{
		/* The buffer is wrapped: gap between endidx and startidx is free. */
		avail = b->startidx - b->endidx;
	}
	if (avail)
	{
		result = read(b->infd, b->buffer + b->endidx, avail);
		if (result > 0)
		{
			/* Some data was read. Update accordingly. */
			b->endidx += result;
			b->totalin += result;
			b->bytes += result;
			if (b->endidx == BUFFER_SIZE)
			{
				/* Time to wrap the buffer. */
				b->endidx = 0;
			}
		}
		else if (result < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			{
				return 0;
			}
			dolog(LOGOPTS, "read error: %m\n");
			b->err |= READ_ERR;
			/*
			 * Treat hard read failures like an end-of-input condition so any
			 * already-buffered bytes can still be drained to the output.
			 */
			b->eof_read = 1;
		}
		else
		{
			/* read() returned 0: a genuine end-of-stream on the input. */
			if (saw_zero_read != NULL)
				*saw_zero_read = 1;
			if (b->detectEof)
			{
				dolog(LOG_DEBUG, "read: eof\n");
				b->eof_read = 1;
			}
		}
	}
	/* Returns 0 when no data could be read (buffer full, temporary I/O condition
	 * EAGAIN/EWOULDBLOCK/EINTR, or non-EOF zero-length read), -1 for hard errors,
	 * or the number of bytes read (>0). */
	return result;
}

/* Backwards-compatible wrapper for callers that do not need to distinguish a
 * genuine zero-length read from the other "no progress" outcomes. */
static ssize_t readBuffer(Buffer_t *b)
{
	return readBufferEx(b, NULL);
}

/* Writes data from a buffer to the output file or discard if no output file is set. */
static ssize_t writeBuffer(Buffer_t *b)
{
	size_t avail;
	ssize_t result = 0;
	if (b->bytes == 0 || (b->err & WRITE_ERR))
	{
		/* Buffer is empty. */
		avail = 0;
	}
	else
	{
		/*
		 * The circular buffer may wrap, so only write a contiguous chunk.
		 * The remaining bytes will be sent in a subsequent write.
		 */
		avail = (size_t)b->bytes;
		if (b->startidx + avail > BUFFER_SIZE)
		{
			avail = BUFFER_SIZE - (size_t)b->startidx;
		}
	}
	if (avail)
	{
		if (b->outfd >= 0)
			result = write(b->outfd, b->buffer + b->startidx, avail);
		else
			result = avail;
		if (result < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			{
				return 0;
			}
			/*
			 * Hard write failure.  Note: an "EPIPE/ECONNRESET after all data
			 * was delivered" situation is NOT handled here on purpose.  By the
			 * time write() is even called, avail > 0 holds (see the avails
			 * computation above), which in turn requires b->bytes > 0, so a
			 * genuine "buffer already empty" (b->bytes == 0) case can never
			 * reach this branch.  That completion case is instead detected
			 * correctly at the end of writeBuffer(): when eof_read is set and
			 * the last pending bytes have been drained (b->bytes == 0),
			 * eof_sent is raised there.  Keeping a dead `if (b->bytes == 0 &&
			 * EPIPE) ...' branch here would be unreachable code that only
			 * misleads future readers, so it has been removed.
			 */
			dolog(LOGOPTS, "write error: %m\n");
			b->err |= WRITE_ERR;
		}
		else if (result == 0)
		{
			/* write() returning 0 with a non-zero count is undefined by POSIX
			 * and cannot make forward progress; treat it as a hard write error. */
			dolog(LOGOPTS, "write error: returned 0\n");
			b->err |= WRITE_ERR;
		}
		else
		{
			/* Some bytes were written. */
			b->startidx += result;
			if (b->outfd >= 0)
				b->totalout += result;
			b->bytes -= result;
			if (b->startidx == BUFFER_SIZE)
			{
				/* Unwrap the buffer. */
				b->startidx = 0;
			}
		}
	}
	/*
	 * Single, authoritative completion point for marking the stream fully
	 * sent: when EOF was observed on the input AND every buffered byte has
	 * now been delivered (bytes == 0), raise eof_sent and log it exactly
	 * once.
	 *
	 * This block runs after every writeBuffer() call, so it covers both
	 * ways a stream is finished:
	 *  - A write that *does* drain the last pending bytes in this very call
	 *    (the "drained on write" case); and
	 *  - A call made when the buffer is already empty (avail == 0, e.g. the
	 *    next select()/loop iteration after EOF arrived and the printer just
	 *    became writable), which still must raise eof_sent.
	 *
	 * Keeping this as the ONLY place that sets eof_sent (and logging it here
	 * alone) avoids the previous bug where the "drained on write" case set
	 * eof_sent -- and logged "write: eof" -- inside the write branch AND
	 * again here, producing a duplicated debug log line on every normal job
	 * completion.
	 */
	if (b->eof_read && b->bytes == 0)
	{
		b->eof_sent = 1;
		dolog(LOG_DEBUG, "write: eof\n");
	}

	/* Return the write() result, -1 (error) or #bytes written. */
	return result;
}

/* Returns 1 when at least timeout_sec seconds (microsecond-accurate) have
 * elapsed since `last`, otherwise 0.  A tv_sec-only comparison can fire up
 * to one second early whenever last.tv_usec exceeds now.tv_usec. */
static int idle_timeout_elapsed(const struct timeval *now,
                                const struct timeval *last,
                                int timeout_sec)
{
	time_t diff = now->tv_sec - last->tv_sec;
	if (diff > timeout_sec)
		return 1;
	if (diff == timeout_sec)
		return now->tv_usec >= last->tv_usec;
	return 0;
}

/*
 * Copy network data from file descriptor fd (network) to lp (printer) until EOS.
 * If bidir, also copy data from printer (lp) to network (fd).
 *
 * fd_closed/lp_closed (may be NULL) are set to 1 when the caller's descriptor
 * number has already been closed here and must NOT be closed again by the
 * caller.  This happens when a descriptor lies outside [0, FD_SETSIZE) and has
 * to be replaced by an in-range duplicate: dup_fd_below_fdsetsize() closes the
 * original as part of that substitution.  Without this contract the caller's
 * close() would operate on an already-closed number, which in a long-running
 * daemon can destroy an unrelated descriptor that meanwhile got the same number
 * recycled.
 */
static int copy_stream_ex(int fd, int lp, int *fd_closed, int *lp_closed)
{
	/*
	 * Initialised to the caller's descriptors so the `out:` cleanup path is
	 * well-defined even when dup_fd_below_fdsetsize() fails before assigning
	 * them (cppcheck reports the initialisation as redundant, but `goto out`
	 * can be reached before the assignments below).
	 */
	int io_fd = fd;
	int io_lp = lp;
	int close_io_fd = 0;
	int close_io_lp = 0;
	int rc = 0;
	ssize_t result;
	Buffer_t networkToPrinterBuffer;

	if (fd_closed != NULL)
		*fd_closed = 0;
	if (lp_closed != NULL)
		*lp_closed = 0;

	/*
	 * Both modes drive the descriptors through select()/fd_set, so both need
	 * descriptors inside [0, FD_SETSIZE).  Descriptors beyond FD_SETSIZE
	 * cannot be monitored safely and would make the stream stall.
	 */
	/* cppcheck-suppress redundantInitialization */
	io_fd = dup_fd_below_fdsetsize(fd, "network");
	if (io_fd < 0)
	{
		/*
		 * dup_fd_below_fdsetsize() only fails after it has given up on `fd`
		 * without closing it, so the caller still owns the original number.
		 */
		rc = -1;
		goto out;
	}
	close_io_fd = (io_fd != fd);
	/* The substitution closed the caller's original descriptor number. */
	if (close_io_fd && fd_closed != NULL)
		*fd_closed = 1;

	io_lp = dup_fd_below_fdsetsize(lp, "printer");
	if (io_lp < 0)
	{
		rc = -1;
		goto out;
	}
	close_io_lp = (io_lp != lp);
	if (close_io_lp && lp_closed != NULL)
		*lp_closed = 1;

	/*
	 * select() only guarantees that at least one byte can be moved, never the
	 * whole contiguous chunk that readBuffer()/writeBuffer() compute from the
	 * circular buffer.  With a blocking descriptor that difference lets read()
	 * or write() park the daemon while the other direction still has work to
	 * do (a slow printer waiting for buffered bytes while the peer is quiet).
	 * Non-blocking descriptors turn those cases into EAGAIN, which both
	 * helpers already handle as "no progress this round".
	 *
	 * The printer is already opened O_NONBLOCK by open_printer(); the network
	 * descriptor comes from accept() or from (x)inetd and is blocking, so it
	 * must be switched explicitly.  A failure here is not fatal: the loops
	 * stay correct (just possibly blocking), so only log it.
	 */
	(void)set_nonblocking(io_fd, "network");
	(void)set_nonblocking(io_lp, "printer");

	initBuffer(&networkToPrinterBuffer, io_fd, io_lp, 1);

	if (bidir)
	{
		struct timeval now;
		struct timeval then;
		struct timeval timeout;
		struct timeval last_activity;
		struct timeval grace_deadline;
		int timer = 0;
		/* Set once the printer-to-network direction has been fully handled:
		 * the network has finished sending the job (eof_read), every printer
		 * response already read has been forwarded to the network, and the
		 * printer has then had a grace period (IDLE_TIMEOUT_SEC) to emit any
		 * further response.  Without this the loop used to stop as soon as the
		 * last job byte reached the printer (eof_sent), so a response the
		 * printer emits *after* network EOF (a common request/reply timing)
		 * was silently dropped. */
		int printer_stream_done = 0;
		/* Becomes nonzero once network EOF (job data fully received) has been
		 * observed; at that moment a grace deadline is anchored and the daemon
		 * keeps the stream open until it elapses so any printer response that
		 * arrives (including one emitted only after network EOF) is forwarded.
		 * An absolute deadline (not a sliding timer) guarantees termination even
		 * for a chatty or non-responsive printer. */
		int eof_reached = 0;
		/* Set once the printer side has been observed at a genuine
		 * end-of-stream, i.e. read() returned 0 (not EAGAIN/EINTR) on a
		 * descriptor select() had just reported readable.  Because
		 * printerToNetworkBuffer is created with detectEof == 0, readBuffer()
		 * never records that condition itself, so it is tracked here.
		 *
		 * This matters for termination latency: a real EOF is a PERMANENT
		 * state (select() keeps reporting the descriptor readable forever),
		 * unlike a momentarily quiescent device.  Without distinguishing the
		 * two, the loop had no way to know the printer will never speak again
		 * and had to burn the whole grace window on every job. */
		int printer_eof = 0;
		Buffer_t printerToNetworkBuffer;
		fd_set readfds;
		fd_set writefds;
		initBuffer(&printerToNetworkBuffer, io_lp, io_fd, 0);
		gettimeofday(&last_activity, NULL);
		/*
		 * Keep copying until BOTH directions are finished:
		 *  - network -> printer: until eof_sent (all job bytes delivered); and
		 *  - printer -> network: until printer_stream_done (all responses
		 *    forwarded and the printer has gone quiet).
		 * Either direction hitting a hard write error on the printer still
		 * stops the whole job.
		 */
		while (!(networkToPrinterBuffer.eof_sent && printer_stream_done) &&
			   !(networkToPrinterBuffer.err & WRITE_ERR))
		{
			int maxfd = -1;
			/*
			 * Once the network has finished sending the job, anchor the grace
			 * deadline (now + IDLE_TIMEOUT_SEC).  The daemon then keeps the
			 * stream open until the deadline so any printer response that arrives
			 * -- including one emitted only AFTER network EOF -- is forwarded.
			 * An absolute deadline (not a sliding timer) guarantees the loop
			 * always terminates, even for a chatty printer.  A non-responsive
			 * printer simply adds the bounded grace latency to completion.
			 */
			if (networkToPrinterBuffer.eof_read && !eof_reached)
			{
				eof_reached = 1;
				gettimeofday(&grace_deadline, NULL);
				grace_deadline.tv_sec += IDLE_TIMEOUT_SEC;
			}
			FD_ZERO(&readfds);
			FD_ZERO(&writefds);
			prepBuffer(&networkToPrinterBuffer, &readfds, &writefds);
			prepBuffer(&printerToNetworkBuffer, &readfds, &writefds);
#define MAYBE_MAX(x)                      \
	do                                    \
	{                                     \
		if ((x) >= 0 && (x) < FD_SETSIZE) \
			maxfd = MAX(maxfd, (x));      \
	} while (0)
			MAYBE_MAX(io_fd);
			MAYBE_MAX(io_lp);
			MAYBE_MAX(networkToPrinterBuffer.infd);
			MAYBE_MAX(networkToPrinterBuffer.outfd);
			MAYBE_MAX(printerToNetworkBuffer.infd);
			MAYBE_MAX(printerToNetworkBuffer.outfd);
#undef MAYBE_MAX

			if (timer)
			{
				/* Delay after reading from the printer, so the */
				/* return stream cannot dominate. */
				/* Don't read from the printer until the timer expires. */
				gettimeofday(&now, NULL);
				if ((now.tv_sec > then.tv_sec) || (now.tv_sec == then.tv_sec && now.tv_usec > then.tv_usec))
					timer = 0;
				else
				{
					/*
					 * Guard FD_CLR just like FD_SET/FD_ISSET use FD_VALID.
					 * If lp >= FD_SETSIZE, touching fd_set is undefined and
					 * can corrupt memory.
					 */
					if (FD_VALID(io_lp))
						FD_CLR(io_lp, &readfds);
				}
			}
			timeout.tv_sec = 0;
			timeout.tv_usec = 100000;
			result = select(maxfd + 1, &readfds, &writefds, NULL, &timeout);
			gettimeofday(&now, NULL);
			if (result < 0)
			{
				if (errno == EINTR)
					continue;
				rc = (int)result;
				goto out;
			}
			if (FD_VALID(io_fd) && FD_ISSET(io_fd, &readfds))
			{
				/* Read network data. */
				result = readBuffer(&networkToPrinterBuffer);
				if (result > 0)
				{
					dolog(LOG_DEBUG, "%.2f: read %zd bytes from network\n",
						  now.tv_sec + now.tv_usec / 1e6, result);
					gettimeofday(&last_activity, NULL);
				}
			}
			/*
			 * Do not time out while data is still pending: network bytes still
			 * queued in the socket (select() reports io_fd readable), bytes
			 * buffered but not yet written to the printer, or printer responses
			 * not yet sent to the network would silently be dropped by an early
			 * exit.  Only stop once both directions are fully drained.
			 *
			 * Crucially, once the network has already sent EOF (eof_read set)
			 * we must NOT stop here on a quiet spell: a printer commonly
			 * answers only after the host closed its send side, so a response
			 * may still be in flight.  In that case the job-completion decision
			 * is deferred to the printer_stream_done logic below, which keeps
			 * the connection open for a full IDLE_TIMEOUT_SEC grace period
			 * after network EOF and is refreshed by any printer response
			 * activity.  Breaking here would drop that late response.
			 */
			if (!networkToPrinterBuffer.eof_read &&
				networkToPrinterBuffer.bytes == 0 &&
				printerToNetworkBuffer.bytes == 0 &&
				!(FD_VALID(io_fd) && FD_ISSET(io_fd, &readfds)))
			{
				/*
				 * Both directions are idle (no buffered bytes, network not
				 * readable).  Compare with microsecond precision: a naive
				 * tv_sec-only comparison can fire up to one second early when
				 * last_activity.tv_usec exceeds now.tv_usec, prematurely
				 * tearing down a job on a slow printer.  last_activity is
				 * bumped on either a network read OR a printer response read,
				 * so a printer that keeps emitting (while the network peer is
				 * quiet) will NOT be timed out mid-job.
				 */
				if (idle_timeout_elapsed(&now, &last_activity, IDLE_TIMEOUT_SEC))
				{
					dolog(LOG_NOTICE, "no activity from network or printer for %ds, stop copy stream\n",
						  IDLE_TIMEOUT_SEC);
					break;
				}
			}
			if (FD_VALID(io_lp) && FD_ISSET(io_lp, &readfds))
			{
				/* Read printer data, but pace it more slowly.  Capture a
				 * genuine zero-length read separately: it means the printer
				 * will never speak again, which lets the job finish as soon
				 * as the already-received response has been forwarded. */
				int zero_read = 0;
				result = readBufferEx(&printerToNetworkBuffer, &zero_read);
				if (zero_read)
					printer_eof = 1;
				if (result > 0)
				{
					dolog(LOG_DEBUG, "%.2f: read %zd bytes from printer\n",
						  now.tv_sec + now.tv_usec / 1e6, result);
					/*
					 * A printer response is activity too: the idle timeout
					 * must not fire while the printer keeps sending (even if
					 * the network peer is momentarily quiet).  Reset the
					 * activity clock so a chatty-but-slow printer is not
					 * torn down mid-job.  See idle_timeout_elapsed below.
					 */
					gettimeofday(&last_activity, NULL);
					gettimeofday(&then, NULL);
					/* wait 100 msec before reading again. */
					then.tv_usec += 100000;
					if (then.tv_usec >= 1000000)
					{
						then.tv_usec -= 1000000;
						then.tv_sec++;
					}
					/* Pace the return stream so the printer cannot dominate
					 * the network direction.  Previously this was gated on a
					 * now-removed "need_clear_lp" flag; the throttle must apply
					 * unconditionally to every printer read. */
					timer = 1;
				}
				else if (result == 0)
				{
					/*
					 * Empty read: the printer side returned no data.
					 * Because detectEof is off for this buffer, EOF (or a
					 * zero-length read from a quiescent device) leaves
					 * eof_read clear, so select() would report the fd
					 * readable again immediately and the loop would spin
					 * at 100% CPU.  Throttle the next read attempt so at
					 * most one empty read happens every 100 ms.
					 */
					gettimeofday(&then, NULL);
					then.tv_usec += 100000;
					if (then.tv_usec >= 1000000)
					{
						then.tv_usec -= 1000000;
						then.tv_sec++;
					}
					timer = 1;
				}
			}
			if (FD_VALID(io_lp) && FD_ISSET(io_lp, &writefds))
			{
				/* Write data to printer. */
				result = writeBuffer(&networkToPrinterBuffer);
				if (result > 0)
				{
					dolog(LOG_DEBUG, "%.2f: wrote %zd bytes to printer\n",
						  now.tv_sec + now.tv_usec / 1e6, result);
				}
			}
			if ((FD_VALID(io_fd) && FD_ISSET(io_fd, &writefds)) || printerToNetworkBuffer.outfd == -1)
			{
				/* Write data to network.  Printer responses are always
				 * forwarded (they are buffered in printerToNetworkBuffer and
				 * drained here); we no longer discard responses that arrive
				 * before the first byte has been written to the printer, so
				 * devices that handshake/respond before receiving data keep
				 * working. */
				result = writeBuffer(&printerToNetworkBuffer);
				/* If socket write error, discard further data from printer */
				if (result < 0)
				{
					printerToNetworkBuffer.outfd = -1;
					/* Only clear the write-error bit; preserve any
					 * printer-side READ_ERR so its state is not lost. */
					printerToNetworkBuffer.err &= ~WRITE_ERR;
					/*
					 * The network peer is gone, so any remaining printer
					 * responses can never be delivered.  Treat the printer
					 * stream as done so the job (network->printer) can still
					 * finish instead of hanging on a network that will never
					 * accept the response.
					 */
					printer_stream_done = 1;
					dolog(LOG_DEBUG, "network write error, discarding further printer data\n");
					continue;
				}
				else if (result > 0)
				{
					if (printerToNetworkBuffer.outfd == -1)
						dolog(LOG_DEBUG, "discarded %zd bytes from printer\n", result);
					else
					{
						dolog(LOG_DEBUG, "%.2f: wrote %zd bytes to network\n",
							  now.tv_sec + now.tv_usec / 1e6, result);
						/*
						 * Forwarding a response to the network keeps the
						 * loop alive until the grace deadline (set when
						 * network EOF was observed), so a late but
						 * still-arriving response is delivered.
						 */
					}
				}
			}
			if ((networkToPrinterBuffer.err & READ_ERR) && networkToPrinterBuffer.bytes == 0)
			{
				/*
				 * A hard network read error stops further reads (eof_read is
				 * set), but any bytes already buffered must still reach the
				 * printer.  Leave only once the buffer is fully drained: a
				 * fixed wall-clock deadline could truncate a job on a slow
				 * printer (e.g. a 300 bps serial device draining 8 KiB takes
				 * minutes).  select() keeps blocking meanwhile, so this never
				 * spins the CPU; if the printer never becomes writable the
				 * idle-timeout above still bounds the wait.
				 */
				dolog(LOG_NOTICE, "network read error, buffered data drained, stop copy stream\n");
				break;
			}
			/*
			 * Decide when the printer->network direction is finished.  The
			 * printer never signals EOF (detectEof is off for this buffer), so
			 * completion is decided once network EOF has been seen: the daemon
			 * keeps the stream open until the grace deadline (anchored at network
			 * EOF) elapses, then completes.  ANY printer response arriving
			 * within that grace window is still forwarded, because the loop keeps
			 * running until the deadline.  An absolute deadline (not a sliding
			 * timer) guarantees termination even for a chatty printer, so the
			 * loop can never hang.  A non-responsive printer simply adds the
			 * (bounded) grace-period latency to job completion; this is the
			 * deliberate cost of guaranteeing a late response is not dropped.
			 *
			 * There is deliberately no *unconditional* "finish early once the
			 * buffer has drained" shortcut: a printer that answers only AFTER
			 * network EOF has its response still in flight when the last job
			 * byte is delivered, and declaring done the moment the buffer
			 * empties would drop that late response.  The grace window is what
			 * lets such a response through.
			 *
			 * However, waiting for the deadline is only necessary while the
			 * printer *could still* say something.  Once read() has returned 0
			 * on the printer descriptor (printer_eof), the device has closed
			 * its send side: no further response can ever arrive, so there is
			 * nothing left for the grace window to protect.  Finishing as soon
			 * as the already-received response has been forwarded is then both
			 * safe and necessary -- otherwise every job with a printer that
			 * closes (or any device that reports EOF, which is the common case
			 * for a finished job) paid the full IDLE_TIMEOUT_SEC before
			 * completing.  That fixed per-job penalty throttles throughput
			 * badly under (x)inetd, where a process handles exactly one job.
			 *
			 * The network write-error case is handled separately above
			 * (printer_stream_done = 1) because no response can ever be
			 * delivered once the peer is gone.
			 */
			if (eof_reached && printerToNetworkBuffer.outfd != -1 &&
				printer_eof && printerToNetworkBuffer.bytes == 0)
			{
				dolog(LOG_NOTICE, "printer closed its response stream, stop copy stream\n");
				printer_stream_done = 1;
			}
			else if (eof_reached && printerToNetworkBuffer.outfd != -1 &&
				(now.tv_sec > grace_deadline.tv_sec ||
				 (now.tv_sec == grace_deadline.tv_sec &&
				  now.tv_usec >= grace_deadline.tv_usec)))
			{
				dolog(LOG_NOTICE, "printer response stream finished (grace elapsed), stop copy stream\n");
				printer_stream_done = 1;
			}
		}
		dolog(LOG_NOTICE,
			  "Finished job: %lu/%lu bytes sent to printer, %lu/%lu bytes sent to network\n",
			  networkToPrinterBuffer.totalout, networkToPrinterBuffer.totalin, printerToNetworkBuffer.totalout, printerToNetworkBuffer.totalin);
	}
	else
	{
		/*
		 * Unidirectional: read from network, write to printer.
		 *
		 * The printer is opened O_NONBLOCK (see open_printer), so a write()
		 * may return EAGAIN when the device is momentarily busy (e.g. a slow
		 * parallel port or a printer that throttles).  The old code called
		 * readBuffer()/writeBuffer() back to back; when the printer was not
		 * writable writeBuffer() made no progress and the loop spun at 100%
		 * CPU while the buffer was full, violating the requirement that
		 * speed differences between the network and the printer must be
		 * handled without busy-waiting.
		 *
		 * Fix: before draining the buffer to the printer, block in select()
		 * until the printer is writable (when there is pending data).  This
		 * replaces the old busy-spin with a kernel-level block, so the daemon
		 * copes with any printer speed and with temporary device
		 * unavailability without consuming CPU.  The loop otherwise keeps the
		 * original read-then-write structure, which is known to make forward
		 * progress whenever the printer becomes writable again.
		 */
		fd_set readfds;
		fd_set writefds;
		while (!networkToPrinterBuffer.eof_sent &&
			   !(networkToPrinterBuffer.err & WRITE_ERR))
		{
			/*
			 * Block until at least one direction can make progress.
			 *
			 * Both descriptors are non-blocking, so the loop must never call
			 * read()/write() speculatively -- that would busy-spin.  Arm the
			 * printer for writing whenever bytes are pending (back-pressure
			 * for a slow printer) and the network for reading whenever there
			 * is free buffer space.  Waiting on BOTH is what avoids a stall:
			 * arming only the printer and then doing a blocking read on a
			 * possibly-empty socket leaves pending bytes undelivered to a
			 * printer that is ready for them, for as long as the peer stays
			 * quiet.  select() blocks with no timeout, so an idle job
			 * consumes no CPU while a missing/again-unavailable printer or a
			 * quiet peer is waited on.
			 */
			int maxfd = -1;
			int want_write = (networkToPrinterBuffer.bytes > 0 ||
			                  networkToPrinterBuffer.eof_read) &&
			                 !(networkToPrinterBuffer.err & WRITE_ERR) &&
			                 !networkToPrinterBuffer.eof_sent &&
			                 FD_VALID(io_lp);
			int want_read = !networkToPrinterBuffer.eof_read &&
			                networkToPrinterBuffer.bytes < BUFFER_SIZE &&
			                !(networkToPrinterBuffer.err & READ_ERR) &&
			                FD_VALID(io_fd);

			FD_ZERO(&readfds);
			FD_ZERO(&writefds);
			if (want_write)
			{
				FD_SET(io_lp, &writefds);
				maxfd = MAX(maxfd, io_lp);
			}
			if (want_read)
			{
				FD_SET(io_fd, &readfds);
				maxfd = MAX(maxfd, io_fd);
			}
			if (maxfd >= 0 &&
				select(maxfd + 1, &readfds, &writefds, NULL, NULL) < 0)
			{
				if (errno == EINTR)
					continue;
				rc = -1;
				goto out;
			}
			/*
			 * Only touch a descriptor select() reported ready.  Reading when
			 * the socket is not readable is exactly the stall described
			 * above, and reading when the buffer is full is pointless work.
			 */
			if (want_read && FD_ISSET(io_fd, &readfds))
				(void)readBuffer(&networkToPrinterBuffer);
			if (!(want_write && FD_ISSET(io_lp, &writefds)))
				continue;
			/*
			 * A hard read error (e.g. a network peer that resets the
			 * connection with data still in flight) sets eof_read inside
			 * readBuffer() but must NOT discard the bytes already pulled
			 * into the buffer.  readBuffer() returns -1 on that hard error,
			 * but the loop condition only checks eof_sent and WRITE_ERR, so
			 * a READ_ERR does not stop the loop; writeBuffer() below keeps
			 * draining the already-buffered bytes to the printer until the
			 * buffer empties, at which point eof_sent is raised and the
			 * lingering error flag is cleared on the exit path when every
			 * byte was delivered (see the eof_sent / totalin==totalout
			 * checks below), so a completed job is reported as success rather
			 * than a spurious failure.  This mirrors the bidirectional drain
			 * behaviour and prevents silent data loss on a fast network /
			 * slow printer.  Bailing out here would both drop buffered data
			 * and misreport a finished job as failed.  Note: readBuffer()'s
			 * return value is intentionally not captured above -- its only
			 * effect we rely on is the side effect (filling the buffer and
			 * setting eof_read on EOF or READ_ERR on a hard error).
			 * writeBuffer() below determines forward progress and owns the
			 * `result` used for the error check.
			 */
			result = writeBuffer(&networkToPrinterBuffer);
			if (result < 0)
			{
				rc = (int)result;
				goto out;
			}
		}
		dolog(LOG_NOTICE, "Finished job: %lu/%lu bytes sent to printer\n", networkToPrinterBuffer.totalout, networkToPrinterBuffer.totalin);
	}
	/* Add a short delay to allow flushing */
	if (networkToPrinterBuffer.eof_sent)
		usleep(200000); /* 200ms */
	/*
	 * If every network byte already reached the printer (eof_sent set), any
	 * lingering error flag is teardown after a successful job, not a failure:
	 *  - WRITE_ERR: the printer peer closed its receive side once the whole
	 *    job had been delivered (EPIPE after eof_sent).
	 *  - READ_ERR: the network peer closed/reset its send side after the last
	 *    byte was already read and forwarded; the data is intact.
	 * A genuine failure always leaves bytes undelivered (eof_sent clear), so
	 * it is still reported.  Reporting a completed job as failed would make
	 * the caller log "copy_stream: <errno>" and treat a success as an error.
	 */
	if (networkToPrinterBuffer.eof_sent)
		networkToPrinterBuffer.err = 0;
	/*
	 * Defensive: if eof_sent was not set (e.g. a hard read error broke the
	 * loop before the EOF-sent marker could be raised) but every byte that
	 * was read in has already been written out (totalin == totalout, no
	 * bytes pending) and EOF was observed on the input, the job is complete.
	 * A lingering READ_ERR/WRITE_ERR is then just the peer closing its side
	 * after the last byte was delivered, not data loss, so do not report it.
	 */
	if (!networkToPrinterBuffer.eof_sent &&
	    networkToPrinterBuffer.eof_read &&
	    networkToPrinterBuffer.bytes == 0 &&
	    networkToPrinterBuffer.totalin == networkToPrinterBuffer.totalout)
		networkToPrinterBuffer.err = 0;
	rc = networkToPrinterBuffer.err ? -1 : rc;
out:
	if (close_io_lp)
		(void)close(io_lp);
	if (close_io_fd)
		(void)close(io_fd);
	return rc;
}

#ifdef TESTING
/*
 * Convenience wrapper used by the test suite, which passes descriptors that are
 * always inside [0, FD_SETSIZE) and keeps ownership of them itself, so the
 * "already closed" feedback cannot be triggered.  Production code must use
 * copy_stream_ex() and honour the fd_closed/lp_closed flags instead.
 *
 * Compiled only for -DTESTING, and marked "possibly unused" because p910nd.c is
 * also compiled standalone with -DTESTING -Werror (see
 * tests/test_server_testing_warnings.c), where nothing references it.
 */
#if defined(__GNUC__)
__attribute__((unused))
#endif
/* cppcheck-suppress unusedFunction */
static int copy_stream(int fd, int lp)
{
	return copy_stream_ex(fd, lp, NULL, NULL);
}
#endif

static void one_job(int lpnumber)
{
	int lp;
	int net_closed = 0;
	int lp_closed = 0;
	struct sockaddr_storage client;
	socklen_t clientlen = sizeof(client);

	if (getpeername(0, (struct sockaddr *)&client, &clientlen) >= 0)
	{
		char host[INET6_ADDRSTRLEN];
		dolog(LOG_NOTICE, "Connection from %s port %hu\n", get_ip_str((struct sockaddr *)&client, host, sizeof(host)), get_port((struct sockaddr *)&client));
	}
	if (get_lock(lpnumber) == 0)
	{
		/*
		 * Under (x)inetd the network connection is descriptor 0.  A failed
		 * lock means another instance already owns the printer.  Mirror the
		 * server() failure path: terminate the process so inetd sees a
		 * definite exit status and the connection (descriptor 0) is released
		 * by the kernel on exit, instead of silently returning and leaving
		 * an ambiguous, half-handled connection behind.
		 */
		exit(1);
	}
	/* Make sure lp device is open... */
	while ((lp = open_printer(lpnumber)) == -1)
		sleep(10);
	if (copy_stream_ex(0, lp, &net_closed, &lp_closed) < 0)
		dolog(LOGOPTS, "copy_stream: %m\n");
	/*
	 * Only close what copy_stream_ex() did not already release: closing an
	 * already-closed number could hit an unrelated recycled descriptor.
	 */
	if (!lp_closed)
		(void)close(lp);
	(void)net_closed; /* stdin is released when the process exits */
	free_lock();
}

static void server(int lpnumber)
{
#ifdef USE_GETPROTOBYNAME
	struct protoent *proto;
#endif
	int netfd = -1, fd, lp, one = 1;
	socklen_t clientlen;
	struct sockaddr_storage client;
	struct addrinfo hints, *res, *ressave;
	char service[8];
	const int bufsiz = 65536;
	int gai_err;

#ifndef TESTING
	/*
	 * These four locals are only referenced inside the daemonization block
	 * below, which is compiled out under -DTESTING.  Declaring them here (as
	 * the original code did) makes every -DTESTING build emit
	 * -Wunused-variable warnings under the project's mandatory -Wall -Wextra,
	 * and would break a -Werror build of the test suite.  Move them into the
	 * block so all build configurations stay warning-free.
	 */
	struct rlimit resourcelimit;
	rlim_t max_close_fd;
	char pidfilename[sizeof(PIDFILE)];
	FILE *f;

	if (!log_to_stdout)
	{
		switch (fork())
		{
		case -1:
			dolog(LOGOPTS, "fork: %m\n");
			exit(1);
		case 0: /* child */
			break;
		default: /* parent */
			exit(0);
		}
		/* Now in child process */
		if (getrlimit(RLIMIT_NOFILE, &resourcelimit) < 0)
		{
			dolog(LOGOPTS, "getrlimit: %m\n");
			exit(1);
		}
		max_close_fd = resourcelimit.rlim_cur;
		if (max_close_fd == RLIM_INFINITY)
		{
			long open_max = sysconf(_SC_OPEN_MAX);
			if (open_max < 0)
				open_max = 1024;
			max_close_fd = (rlim_t)open_max;
		}
		/*
		 * Iterate with an rlim_t counter.  Closing through an int counter
		 * would invoke signed overflow (undefined behaviour) once the
		 * descriptor count exceeds INT_MAX, with an arbitrary outcome.
		 */
		{
			rlim_t i;
			for (i = 0; i < max_close_fd; ++i)
				(void)close((int)i);
		}
		if (setsid() < 0)
		{
			dolog(LOGOPTS, "setsid: %m\n");
			exit(1);
		}
		if (chdir("/") < 0)
		{
			dolog(LOGOPTS, "chdir: %m\n");
			exit(1);
		}
		(void)umask(022);
		fd = open("/dev/null", O_RDWR); /* stdin */
		if (fd < 0)
		{
			dolog(LOGOPTS, "/dev/null: %m\n");
			exit(1);
		}
		if (dup2(fd, STDOUT_FILENO) < 0) /* stdout */
		{
			dolog(LOGOPTS, "dup2 stdout: %m\n");
			exit(1);
		}
		if (dup2(fd, STDERR_FILENO) < 0) /* stderr */
		{
			dolog(LOGOPTS, "dup2 stderr: %m\n");
			exit(1);
		}
	}
	if (get_lock(lpnumber) == 0)
		exit(1);
	if (!log_to_stdout)
	{
		(void)snprintf(pidfilename, sizeof(pidfilename), PIDFILE, lpnumber);
		if ((f = fopen(pidfilename, "w")) == NULL)
		{
			dolog(LOGOPTS, "%s: %m\n", pidfilename);
			free_lock();
			exit(1);
		}
		if (fprintf(f, "%d\n", getpid()) < 0)
		{
			dolog(LOGOPTS, "%s: fprintf: %m\n", pidfilename);
			(void)fclose(f);
			free_lock();
			exit(1);
		}
		if (fclose(f) != 0)
		{
			dolog(LOGOPTS, "%s: fclose: %m\n", pidfilename);
			free_lock();
			exit(1);
		}
	}
#endif
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_flags = AI_PASSIVE;
	hints.ai_socktype = SOCK_STREAM;
	if (lpnumber < '0' || lpnumber > '9')
	{
		dolog(LOGOPTS, "invalid lpnumber '%c' (must be 0-9)\n", lpnumber);
		exit(1);
	}
	(void)snprintf(service, sizeof(service), "%d", (BASEPORT + lpnumber - '0'));
	gai_err = getaddrinfo(bindaddr, service, &hints, &res);
	if (gai_err != 0)
	{
		dolog(LOGOPTS, "getaddrinfo: %s\n", gai_strerror(gai_err));
		free_lock();
		exit(1);
	}
	ressave = res;
	while (res)
	{
#ifdef USE_GETPROTOBYNAME
		if ((proto = getprotobyname("tcp6")) == NULL)
		{
			if ((proto = getprotobyname("tcp")) == NULL)
			{
				dolog(LOGOPTS, "Cannot find protocol for TCP!\n");
				free_lock();
				exit(1);
			}
		}
		if ((netfd = socket(res->ai_family, res->ai_socktype, proto->p_proto)) < 0)
#else
		if ((netfd = socket(res->ai_family, res->ai_socktype, 0)) < 0)
#endif
		{
			dolog(LOGOPTS, "socket: %m\n");
			res = res->ai_next;
			continue;
		}
		if (setsockopt(netfd, SOL_SOCKET, SO_RCVBUF, &bufsiz, sizeof(bufsiz)) < 0)
		{
			dolog(LOGOPTS, "setsockopt: SO_RCVBUF: %m\n");
			/* not fatal if it fails */
		}
		if (setsockopt(netfd, SOL_SOCKET, SO_SNDBUF, &bufsiz, sizeof(bufsiz)) < 0)
		{
			dolog(LOGOPTS, "setsockopt: SO_SNDBUF: %m\n");
			/* not fatal if it fails */
		}
		if (setsockopt(netfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
		{
			dolog(LOGOPTS, "setsockopt: SO_REUSEADDR: %m\n");
			(void)close(netfd);
			netfd = -1;
			res = res->ai_next;
			continue;
		}
		if (bind(netfd, res->ai_addr, res->ai_addrlen) < 0)
		{
			dolog(LOGOPTS, "bind: %m\n");
			(void)close(netfd);
			netfd = -1;
			res = res->ai_next;
			continue;
		}
		if (listen(netfd, 30) < 0)
		{
			dolog(LOGOPTS, "listen: %m\n");
			(void)close(netfd);
			netfd = -1;
			res = res->ai_next;
			continue;
		}
		break;
	}
	freeaddrinfo(ressave);
	if (netfd < 0)
	{
		dolog(LOGOPTS, "failed to create and bind a listening socket on %s:%s\n",
		      bindaddr ? bindaddr : "*", service);
		free_lock();
		exit(1);
	}
	memset(&client, 0, sizeof(client));
	while (1)
	{
		char host[INET6_ADDRSTRLEN];
		clientlen = sizeof(client);
		fd = accept(netfd, (struct sockaddr *)&client, &clientlen);
		if (fd < 0)
		{
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			if (errno == EMFILE || errno == ENFILE ||
			    errno == ENOBUFS || errno == ENOMEM)
			{
				/*
				 * Transient resource exhaustion.  Wait without burning CPU
				 * (see accept_backoff) instead of a busy-spin sleep(1).
				 */
				dolog(LOGOPTS, "accept: %m, waiting for a free descriptor\n");
				accept_backoff(netfd);
				continue;
			}
			break;
		}
#ifdef USE_LIBWRAP
		if (hosts_ctl("p910nd", STRING_UNKNOWN, get_ip_str((struct sockaddr *)&client, host, sizeof(host)), STRING_UNKNOWN) == 0)
		{
			dolog(LOGOPTS,
				  "Connection from %s port %hu rejected\n", get_ip_str((struct sockaddr *)&client, host, sizeof(host)), get_port((struct sockaddr *)&client));
			(void)close(fd);
			continue;
		}
#endif
		dolog(LOG_NOTICE, "Connection from %s port %hu accepted\n", get_ip_str((struct sockaddr *)&client, host, sizeof(host)), get_port((struct sockaddr *)&client));
		/*write(fd, "Printing", 8); */

		/* Make sure lp device is open... */
		while ((lp = open_printer(lpnumber)) == -1)
			sleep(10);

		{
			int net_closed = 0;
			int lp_closed = 0;

			if (copy_stream_ex(fd, lp, &net_closed, &lp_closed) < 0)
				dolog(LOGOPTS, "copy_stream: %m\n");
			/*
			 * copy_stream_ex() may have replaced an out-of-range descriptor
			 * with an in-range duplicate, closing the original in the
			 * process.  Closing such a number again would, once it has been
			 * recycled (e.g. by the next accept() or by the listening
			 * socket), destroy an unrelated live descriptor.
			 */
			if (!net_closed)
				(void)close(fd);
			if (!lp_closed)
				(void)close(lp);
		}
	}
	dolog(LOGOPTS, "accept: %m\n");
	free_lock();
	exit(1);
}

static int is_standalone(void)
{
	struct sockaddr_storage bind_addr;
	socklen_t ba_len;

	/*
	 * Check to see if a socket was passed to us from (x)inetd.
	 *
	 * Use getsockname() to determine if descriptor 0 is indeed a socket
	 * (and thus we are probably a child of (x)inetd) or if it is instead
	 * something else and we are running standalone.
	 */
	ba_len = sizeof(bind_addr);
	if (getsockname(0, (struct sockaddr *)&bind_addr, &ba_len) == 0)
		return (0);		   /* under (x)inetd */
	if (errno != ENOTSOCK) /* strange... */
		dolog(LOGOPTS, "getsockname: %m\n");
	return (1);
}

int main(int argc, char *argv[])
{
	int c, lpnumber;
	char *p;
	char *log_ident;

	/*
	 * Broken peer connections can happen while writing in bidirectional mode.
	 * Ignore SIGPIPE so write() reports EPIPE and the daemon can continue.
	 */
	(void)signal(SIGPIPE, SIG_IGN);

	if (argc <= 0) /* in case not provided in (x)inetd config */
		progname = default_progname;
	else
	{
		progname = argv[0];
		if ((p = strrchr(progname, '/')) != NULL)
			progname = p + 1;
	}
	lpnumber = '0';
	while ((c = getopt(argc, argv, "bdi:f:v")) != EOF)
	{
		switch (c)
		{
		case 'b':
			bidir = 1;
			break;
		case 'd':
			log_to_stdout = 1;
			break;
		case 'f':
			device = optarg;
			break;
		case 'i':
			bindaddr = optarg;
			if (bindaddr != NULL && strlen(bindaddr) == 0)
			{
				/*
				 * An empty -i would be passed straight to getaddrinfo(),
				 * which treats "" as "any address" and silently overrides
				 * the user's intent.  Reject it explicitly so a typo does
				 * not quietly rebind to all interfaces.  The standard hints
				 * still bound the length, but an explicit check is clearer.
				 */
				dolog(LOGOPTS, "invalid bind address (empty)\n");
				usage();
			}
			break;
		case 'v':
			show_version();
			exit(0);
			break;
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 1)
		usage();
	if (argc == 1)
	{
		if (argv[0][0] >= '0' && argv[0][0] <= '9' && argv[0][1] == '\0')
			lpnumber = argv[0][0];
		else
		{
			dolog(LOGOPTS, "invalid printer number '%s' (must be a single digit 0-9)\n", argv[0]);
			usage();
		}
	}
	if (lpnumber < '0' || lpnumber > '9')
	{
		dolog(LOGOPTS, "invalid printer number '%c' (must be 0-9)\n", lpnumber);
		usage();
	}
	/* change the n in argv[0] to match the port so ps will show that */
	if ((p = strstr(progname, "p910n")) != NULL)
		p[4] = (char)lpnumber;

	log_ident = p ? p : progname;

	/* We used to pass (LOG_PERROR|LOG_PID|LOG_LPR|LOG_ERR) to syslog, but
	 * syslog ignored the LOG_PID and LOG_PERROR option.  I.e. the intention
	 * was to add both options but the effect was to have neither.
	 * I disagree with the intention to add PERROR.  --Stef  */
	if (!log_to_stdout)
		openlog(log_ident, LOG_PID, LOG_LPR);

	if (log_to_stdout || is_standalone())
		server(lpnumber);
	else
		one_job(lpnumber);
	return (0);
}
