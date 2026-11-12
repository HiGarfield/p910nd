/*
 *	Port 9100+n daemon
 *	Accepts a connection from port 9100+n and copy stream to
 *	/dev/lpn, where n = 0,1,2.
 *
 *	GPLv2 license, read COPYING
 *
 *	Run standalone as: p910nd [0|1|2]
 *
 *	Run under inetd as:
 *	p910n stream tcp nowait root /usr/sbin/tcpd p910nd [0|1|2]
 *	 where p910n is an /etc/services entry for
 *	 port 9100, 9101 or 9102 as the case may be.
 *	 root can be replaced by any uid with rw permission on /dev/lpn
 *
 *	Port 9100+n will then be passively opened
 *	n defaults to 0
 *
 *	Version 1.1
 *	Bug fix release: no change to the command line or to the data streams.
 *	See the "Fourth round fixes (1.1)" section of README.md for the list.
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
 *	Fixed the I/O wait timeout (controlling in/out balancing).
 *	Fixed network-to-printer buffer reporting EOF although there was data
 *	left in the buffer.
 *	Added log-to-stdout option (-d).
 *
 *	Version 0.95
 *	Patch by Mario Izquierdo
 *	Fix incomplete conversion to manipulate new ip_addr structure
 *	when LIBWRAP is enabled
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

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h> /* B2: TCP_NODELAY and keepalive tuning */
#include <poll.h>        /* T8: poll() has no descriptor-number limit */
#include <signal.h>      /* A1/D9: SIGPIPE and clean shutdown */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h> /* waitpid() for forked job children (R7) */
#include <syslog.h>
#include <time.h> /* nanosleep()/struct timespec (R6 polling pause) */
#include <unistd.h>

#ifdef __linux__
#include <linux/lp.h> /* U8a: LPGETSTATUS to poll the parallel port */
#endif
#include <sys/ioctl.h> /* U8a: ioctl() for LPGETSTATUS */

#ifdef USE_LIBWRAP
#include "tcpd.h"
int allow_severity, deny_severity;
extern int hosts_ctl(char *daemon, char *client_name, char *client_addr,
                     char *client_user);
#endif

#define BASEPORT 9100
#define PIDFILE "/var/run/p910%cd.pid"
/* V-lockdir: the directory and the file are now separate so that get_lock() can
 * create the directory when it is missing (PD-02).  LOCKFILE_DIR overrides the
 * whole directory, e.g. -DLOCKFILE_DIR=\"/var/log\". */
#ifdef LOCKFILE_DIR
#define LOCKDIR LOCKFILE_DIR
#else
#define LOCKDIR "/var/lock/subsys"
#endif
#define LOCKFILE LOCKDIR "/p910%cd"
#ifndef PRINTERFILE
#define PRINTERFILE "/dev/lp%c"
#endif
#define LOGOPTS LOG_ERR

#define BUFFER_SIZE 8192

/* getaddrinfo(AI_PASSIVE, PF_UNSPEC) may return an IPv4 and an IPv6 wildcard
 * address.  Every one that binds has to be listened on, so the accept loop
 * needs a descriptor for each and a poll() set to wait on them. */
#define MAX_LISTEN_FDS 8

/* Idle timeout in seconds.  A job is abandoned when neither direction has
 * moved data for this long, so a client that connects and never sends
 * anything cannot wedge the single threaded daemon (B1) and a slow printer
 * or a client waiting for status is not killed (B4/B5).  Only counts while
 * both buffers are empty.  Override at build time: -DIDLE_TIMEOUT=60 */
#ifndef IDLE_TIMEOUT
#define IDLE_TIMEOUT 30
#endif
/* Opening the printer is retried because the printer may be switched off when
 * the daemon starts, or switched off between two jobs, so the device node may
 * not exist yet; waiting for the printer is better than losing the job.
 * The wait differs by the kind of failure (see printer_open_is_permanent()):
 * a temporary failure is retried forever, a permanent one only until
 * OPEN_PRINTER_PERMANENT_WAIT seconds have passed since the client connected,
 * so the user gets that long to switch the printer on but a job whose printer
 * is simply missing is not held (and does not hold the queue) indefinitely.
 * The budget is counted from the connection, not from each attempt, so a job
 * that already queued longer than the budget fails as soon as it sees a
 * permanent error instead of adding another full wait.
 * Interval between attempts in seconds; SIGTERM interrupts the wait.
 * Override with -DOPEN_PRINTER_RETRY_INTERVAL=n /
 * -DOPEN_PRINTER_PERMANENT_WAIT=n. */
#ifndef OPEN_PRINTER_RETRY_INTERVAL
#define OPEN_PRINTER_RETRY_INTERVAL 10
#endif
#ifndef OPEN_PRINTER_PERMANENT_WAIT
#define OPEN_PRINTER_PERMANENT_WAIT 30
#endif
/* V-openmax: an absolute ceiling on the whole wait for the printer, whatever
 * kind of failure it is.  A *temporary* failure is otherwise retried for as
 * long as it takes, and a device can report one for ever: ENXIO from a USB
 * backend that never comes back, EBUSY from a driver that keeps the node.
 * That job holds the byte-1 job lock the whole time, so every later job for
 * the same printer queues behind it and the printer stops being usable.
 * Counted from the moment the client connected.  0 removes the ceiling
 * (the pre-fix behaviour).  Seconds; -DOPEN_PRINTER_MAX_WAIT=n. */
#ifndef OPEN_PRINTER_MAX_WAIT
#define OPEN_PRINTER_MAX_WAIT 300
#endif
/* V-openlog: a printer that stays unavailable must not flood syslog, one
 * LOG_ERR line per job per retry interval quickly buries everything else.
 * The first failure is reported at LOG_ERR and then at most one line every
 * this many seconds; giving up is always reported at LOG_ERR with the real
 * errno.  Seconds; -DOPEN_PRINTER_RETRY_LOG_EVERY=n. */
#ifndef OPEN_PRINTER_RETRY_LOG_EVERY
#define OPEN_PRINTER_RETRY_LOG_EVERY 60
#endif
/* V-lockwait: how long a job waits for the printer lock.  0 (the default) keeps
 * the original behaviour: queue for as long as it takes, because losing a print
 * job is worse than making the next one wait.  A non-zero value bounds that
 * wait so one wedged job cannot block every later job for that printer
 * forever; the timeout is reported to the client as a failure (RST) so it
 * retries instead of hanging.  Seconds; override with -DLOCK_WAIT_TIMEOUT=n. */
#ifndef LOCK_WAIT_TIMEOUT
#define LOCK_WAIT_TIMEOUT 0
#endif
/* V-instancelock: how long a *second daemon instance* waits for the byte-0
 * lock.  Byte 0 answers "am I the only p910nd for this printer?", which is a
 * different question from byte 1's "may I use the printer now?" and needs the
 * opposite answer: a second instance must say so and exit, because queueing
 * would leave behind a process that never listens (the daemonizing parent has
 * already exited successfully) and that silently takes the port over the moment
 * the first instance stops - a restart would appear to succeed and then leave
 * the old, or a new but unexpected, daemon behind.
 * 0 (the default) is one non-blocking attempt and then "already running";
 * a positive value is the number of seconds to wait for a restart takeover.
 * Seconds; -DINSTANCE_LOCK_WAIT=n. */
#ifndef INSTANCE_LOCK_WAIT
#define INSTANCE_LOCK_WAIT 0
#endif

/* R1/T4/U1/U2: a printer that accepts no data for this long (while we still
 * have bytes to send) is considered stalled.  This is now a *recoverable*
 * budget: an out-of-paper / off-line / full-buffer condition reports EAGAIN and
 * we keep retrying, so a job survives a transient lapse of up to this long and
 * resumes automatically when the printer recovers.  A printer that reports a
 * permanent fault (ENODEV/EIO) fails at once via writeBuffer().  Default raised
 * 30s -> 120s so a slow or briefly stalled printer is no longer mis-judged as
 * dead (U1/U2).  The final flush (PRINTER_FLUSH_TIMEOUT) runs afterwards, so
 * the worst case is 120 + 10 = 130s.  Override with -DPRINTER_STALL_TIMEOUT=n.
 */
#ifndef PRINTER_STALL_TIMEOUT
#define PRINTER_STALL_TIMEOUT 120
#endif
/* R1: total budget for the final flush of a job (seconds). */
#ifndef PRINTER_FLUSH_TIMEOUT
#define PRINTER_FLUSH_TIMEOUT 10
#endif
/* R5: how long to wait for the device to go idle before closing it. */
#ifndef PRINTER_DRAIN_TIMEOUT
#define PRINTER_DRAIN_TIMEOUT 5
#endif
/* T1: once the client has half-closed and both buffers are drained, keep the
 * connection only long enough for the printer's reply (status query answer)
 * to drain.  Do NOT wait on the printer direction EOF: character devices such
 * as /dev/lpX and usblp never signal EOF, so the old fixed 120s wait locked
 * the connection and the byte-1 job lock for two minutes on every bi-di job.
 * The window is measured from the last real printer->network movement, so a
 * still-talking device is never cut off.  Override with -DPRINTER_REPLY_WINDOW.
 */
#ifndef PRINTER_REPLY_WINDOW
#define PRINTER_REPLY_WINDOW 60
#endif
/* U4: after the client half-closed, if the printer has already sent a reply,
 * release the connection PRINTER_REPLY_IDLE seconds after the last activity
 * (keeps a normal bi-di job quick, preserving T1).  If the printer has sent
 * nothing yet we wait the longer PRINTER_REPLY_WINDOW, because a printer that
 * is out of paper / off line / busy may only answer a status query after the
 * user clears the condition (tens of seconds to minutes).  Both overridable. */
#ifndef PRINTER_REPLY_IDLE
#define PRINTER_REPLY_IDLE 5
#endif
/* V8: on SIGTERM the daemon stops accepting and gives in-flight jobs this many
 * seconds to finish on their own before they are forcibly terminated.  A normal
 * job draining the printer exits well within this window; only jobs stuck on a
 * dead printer outlive it.  Override with -DSHUTDOWN_GRACE=n. */
#ifndef SHUTDOWN_GRACE
#define SHUTDOWN_GRACE 30
#endif
/* V-childcap: how many job children may be forked at the same time.  Every
 * client has to be served, but "served" does not mean "forked at once": when
 * the cap is reached the connection is simply left in the listen backlog
 * instead of being accepted, so the client waits a little and is still served,
 * while the number of processes is bounded.  Without a cap a printer that
 * never becomes available forks one process per connection until the machine
 * runs out of processes, descriptors or memory.
 * 0 means no cap.  -DMAX_JOB_CHILDREN=n. */
#ifndef MAX_JOB_CHILDREN
#define MAX_JOB_CHILDREN 64
#endif
/* R9/T2/U3: a client that connected but never sent a byte is released after
 * IDLE_TIMEOUT (a probe).  A client that has actually streamed data is a real
 * job that may legitimately pause (CUPS building pages, a slow link, a client
 * that sends a PJL header then waits, a large job sent in pieces), so it is
 * only abandoned after SILENT_TIMEOUT of no progress - which must be long
 * enough not to truncate such jobs.  Default raised from 10s to 120s (U3) so a
 * 35s client pause is not mistaken for a dead client (override
 * -DSILENT_TIMEOUT=n). */
#ifndef SILENT_TIMEOUT
#define SILENT_TIMEOUT 120
#endif
/* V-jobmax: an absolute ceiling on one job, measured from the moment
 * copy_stream() starts.  Every other deadline in the loop is a *no progress*
 * deadline and some of them can be switched off (-DPRINTER_STALL_TIMEOUT=0
 * means "never abandon on stall"), so without this there is no configuration
 * in which a job that is waiting for the printer is guaranteed to end.  Such a
 * job holds the byte-1 job lock, so it also stops every later job.
 * 0 (the default) keeps today's behaviour: no absolute ceiling.  Seconds;
 * -DJOB_MAX_DURATION=n. */
#ifndef JOB_MAX_DURATION
#define JOB_MAX_DURATION 0
#endif
/* R6: consecutive zero byte reads from the printer before its direction is
 * treated as finished, so an EOF device cannot spin the CPU. */
#ifndef PRINTER_EOF_ZERO_READS
#define PRINTER_EOF_ZERO_READS 20
#endif
/* R6: pause after an iteration that moved nothing (microseconds). */
#ifndef NO_PROGRESS_USLEEP
#define NO_PROGRESS_USLEEP 20000
#endif
/* R4/U8b: bounds for draining a socket so close() sends FIN instead of RST.
 * Both a time cap and a byte cap are applied.  Both defaults were enlarged
 * (3s->10s, 256KiB->1MiB) because a large in-flight job can have far more than
 * 256KiB still buffered in the kernel, so the old limits cut the connection
 * with RST and lost that data (U8b).  Override with -DDRAIN_TIMEOUT=n /
 * -DDRAIN_MAX_BYTES=n. */
#ifndef DRAIN_TIMEOUT
#define DRAIN_TIMEOUT 10
#endif
#ifndef DRAIN_MAX_BYTES
#define DRAIN_MAX_BYTES ((size_t)1024 * 1024)
#endif
/* V5: socket read/write timeouts, decoupled from the business IDLE_TIMEOUT.
 * Kept short so a write() that the peer is not reading returns EAGAIN at once
 * instead of blocking for IDLE_TIMEOUT seconds; the central poll() in
 * copy_stream() retries the move.  Override with -DSOCKET_RCVTIMEO=n /
 * -DSOCKET_SNDTIMEO=n. */
#ifndef SOCKET_RCVTIMEO
#define SOCKET_RCVTIMEO 5
#endif
#ifndef SOCKET_SNDTIMEO
#define SOCKET_SNDTIMEO 5
#endif
/* V1: independent, bounded "no progress" limit for the printer->network
 * direction.  If data is pending for the client but neither reaches the client
 * nor is read from the printer for this long, the job ends (the peer is gone or
 * not draining the reply) instead of spinning forever.  Override with
 * -DNETWORK_STALL_TIMEOUT=n.  Must be > 0. */
#ifndef NETWORK_STALL_TIMEOUT
#define NETWORK_STALL_TIMEOUT 120
#endif
/* V1 test knob: delay (us) after reading from the printer in bi-directional
 * mode, so the return stream cannot dominate.  The default keeps the reply
 * polite; the test build lowers it so a stuck return buffer engages the
 * NETWORK_STALL_TIMEOUT guard quickly.  Override with -DPRINTER_READ_PACE_US=n.
 */
#ifndef PRINTER_READ_PACE_US
#define PRINTER_READ_PACE_US 100000
#endif
/* V3: on a FAILED job (printer busy / device missing / stall / discarded data)
 * close the client socket with SO_LINGER{onoff=1,linger=0} so the kernel sends
 * RST and the client (CUPS) gets ECONNRESET instead of a clean FIN.  AppSocket
 * (9100) has no application-layer acknowledgement, so a clean FIN makes CUPS
 * believe the job printed and never retry, silently losing the job.  Set
 * -DFAIL_WITH_RST=0 to keep the old clean-FIN behaviour. */
#ifndef FAIL_WITH_RST
#define FAIL_WITH_RST 1
#endif

/* Circular buffer used for each direction. */
typedef struct {
	int detectEof; /* If nonzero, EOF is marked when read returns 0 bytes.
	                */
	int infd;      /* Input file descriptor. */
	int outfd;     /* Output file descriptor. */
	int startidx;  /* Index of the start of valid data. */
	int endidx;    /* Index of the end of valid data. */
	int bytes;     /* The number of bytes currently buffered. */
	/* D8: byte counters are unsigned long, never int: an int overflowed at
	 * 2 GB and destroyed the print_ok verdict.  On LP64 this is a full 64
	 * bit counter; on ILP32 it wraps at 4 GB.
	 *
	 * WRAPPING IS SAFE ONLY IF comparisions keep using differences:
	 *   (totalin - totalout) is the number of bytes not yet delivered.
	 * Unsigned subtraction returns that difference modulo the type width,
	 * and the difference is bounded by BUFFER_SIZE, so the result is the
	 * exact value across a wrap.  NEVER write "totalout >= totalin" here:
	 * it flips when one counter has wrapped and the other has not. */
	unsigned long totalin;  /* Total bytes that have been read. */
	unsigned long totalout; /* Total bytes that have been written. */
	/* Whether at least one byte was ever read into this buffer.  Used to
	 * tell a probe (never sent anything) from a real job.  It must NOT be
	 * derived from totalin == 0: on ILP32 totalin can wrap around to 0
	 * again, which would mistake a multi-gigabyte job for a probe. */
	int saw_data;
	int eof_read;   /* Nonzero indicates the input file has reached EOF. */
	int eof_sent;   /* Nonzero indicates the output file has fully received
	                   all   data. */
	int zero_reads; /* Consecutive read()s that returned 0 bytes (R6). */
	int err; /* Nonzero indicates an error detected on the output file. */
#define READ_ERR 0x01
#define WRITE_ERR 0x02
	char buffer[BUFFER_SIZE]; /* Buffered data goes here. */
} Buffer_t;

/* D15: the fallback name is a writable array, not a string literal.  main()
 * rewrites the port digit inside progname so ps shows the port, and writing
 * into a literal would be undefined behaviour. */
static char default_progname[] = "p910nd";
static char *progname;
static const char version[] = "Version 1.1";
static const char copyright[] =
    "Copyright (c) 2008-2014 Ken Yap and others, GPLv2";
static int lockfd = -1;
static char *device = 0;
static int bidir = 0;
static char *bindaddr = 0;
static int log_to_stdout = 0;
/* D9: remembered so the pid file can be removed when the daemon goes away. */
static char pidfilename[sizeof(PIDFILE)];
static int have_pidfile = 0;
/* R11: the lock file name, so the path that was actually used can be logged. */
static char lockname[sizeof(LOCKFILE)];
/* R10: signal handlers only raise a flag, the main loop does the work. */
static volatile sig_atomic_t got_term = 0;
/* T5/T10a: number of forked job children still running.  The SIGCHLD handler
 * reaps and decrements it; shutdown waits for it to reach zero. */
static volatile sig_atomic_t inflight_children = 0;
/* T10b: pid of every running job child, so shutdown can terminate the ones
 * that outlive the grace period.  There is deliberately no limit on how many
 * clients may be served at once: refusing a connection because other jobs are
 * in flight would silently drop print jobs, so the table grows on demand. */
#define CHILD_PIDS_INITIAL 64
static pid_t *child_pids = 0;
static int child_pids_slots = 0;

/* Helper function: convert a struct sockaddr address (IPv4 and IPv6) to a
 * string */
static char *get_ip_str(const struct sockaddr *sa, char *s, size_t maxlen) {
	if (s == 0 || maxlen == 0)
		return s;
	switch (sa->sa_family) {
	case AF_INET:
		if (inet_ntop(AF_INET, &(((const struct sockaddr_in *)sa)->sin_addr), s,
		              maxlen) == NULL)
			(void)snprintf(s, maxlen, "Unknown AF");
		break;
	case AF_INET6:
		if (inet_ntop(AF_INET6, &(((const struct sockaddr_in6 *)sa)->sin6_addr),
		              s, maxlen) == NULL)
			(void)snprintf(s, maxlen, "Unknown AF");
		break;
	default:
		(void)snprintf(s, maxlen, "Unknown AF");
		break;
	}
	/* D3: callers feed the result straight to %s/hosts_ctl(), never return
	 * NULL and always guarantee the NUL termination. */
	s[maxlen - 1] = '\0';
	return s;
}

static unsigned short get_port(const struct sockaddr *sa) {
	unsigned short port;
	switch (sa->sa_family) {
	case AF_INET:
		/* D11: the cast keeps the const of the caller's address,
		 * casting to a non-const pointer here would silently drop it.
		 */
		port = ntohs(((const struct sockaddr_in *)sa)->sin_port);
		break;
	case AF_INET6:
		port = ntohs(((const struct sockaddr_in6 *)sa)->sin6_port);
		break;
	default:
		return 0;
	}
	return port;
}

static void usage(void) {
	fprintf(stderr, "%s %s %s\n", progname, version, copyright);
	fprintf(stderr, "Usage: %s [-f device] [-i bindaddr] [-bvd] [0|1|2]\n",
	        progname);
	exit(1);
}

static void show_version(void) {
	fprintf(stdout, "%s %s\n", progname, version);
}

/* V-logfd: under (x)inetd the accepted socket is on fd 0, 1 *and* 2, so
 * log_to_stdout would write the log into the client's own byte stream (PD-04).
 * Detect that case and fall back to syslog.  fstat() rather than a remembered
 * descriptor number, because it is the identity of the open file that matters,
 * not which number it happens to have. */
static int stdout_is_the_client_socket(void) {
	struct stat s1;
	struct stat s0;

	if (fstat(1, &s1) != 0 || fstat(0, &s0) != 0)
		return (0);
	return S_ISSOCK(s1.st_mode) && s1.st_dev == s0.st_dev &&
	       s1.st_ino == s0.st_ino;
}

/* The format string is never modified: taking it as const char * keeps every
 * call site from passing a (const) string literal as a char * (which the
 * compiler only accepts because C89 types literals as char[]). */
static void dolog(int level, const char *msg, ...) {
	va_list argp;
	va_start(argp, msg);
	if (log_to_stdout && !stdout_is_the_client_socket())
		vfprintf(stdout, msg, argp);
	else if (level != LOG_DEBUG)
		vsyslog(level, msg, argp);
	va_end(argp);
}

/* V-time: every deadline in this program goes through mono_now().  All of them
 * used to be derived from gettimeofday(), i.e. from CLOCK_REALTIME, so a single
 * backwards clock step (NTP correction, an administrator, a VM resuming from
 * suspend) made every "now - start" difference negative: IDLE_TIMEOUT,
 * SILENT_TIMEOUT, PRINTER_STALL_TIMEOUT, PRINTER_REPLY_WINDOW, SHUTDOWN_GRACE
 * and the permanent-open budget all stopped firing and the job -- and the
 * daemon -- hung.  A forward step did the opposite and aborted live jobs.
 *
 * clock_gettime() needs no extra library on musl, uClibc, BSD or glibc 2.17 and
 * later; older glibc keeps it in librt.  The switch is therefore decided at
 * compile time:
 *
 *   -DNO_CLOCK_GETTIME=1   force the gettimeofday() fallback (for a toolchain
 *                          where clock_gettime() is unusable, or to reproduce
 * the old behaviour) old glibc (< 2.17)      falls back automatically; add -lrt
 * to LIBS to enable the monotonic clock there
 *
 * When the monotonic clock is unavailable the fallback is the original
 * gettimeofday(), so behaviour is exactly what it was before.
 */
#if !defined(NO_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 17)
#define P910ND_HAVE_MONOTONIC 1
#endif
#elif !defined(__GLIBC__)
/* musl, uClibc, the BSDs, macOS: clock_gettime() is always in libc. */
#define P910ND_HAVE_MONOTONIC 1
#endif
#endif

/* The one place the program reads the clock.  Returns non-zero when the
 * monotonic time base is in use.
 *
 * PD-40: the clock source is latched on the first call.  A transient
 * clock_gettime() failure must not silently switch to gettimeofday(): the
 * former counts seconds since boot, the latter seconds since the epoch, so
 * every "now - start" difference would jump by decades and fire (or
 * suppress) every deadline at once.  When that happens we keep the
 * monotonic base by subtracting the wall-minus-monotonic offset captured
 * at the first successful call. */
static int mono_now(struct timeval *tv) {
#ifdef P910ND_HAVE_MONOTONIC
	struct timespec ts;
	static int use_monotonic = -1; /* -1 undecided, 1 monotonic, 0 wall */
	static long wall_offset = 0;   /* wall seconds minus monotonic seconds */
	static int offset_known = 0;
	static struct timeval last;
	int ok = (clock_gettime(CLOCK_MONOTONIC, &ts) == 0);

	if (use_monotonic < 0)
		use_monotonic = ok;
	if (use_monotonic == 1 && ok) {
		tv->tv_sec = ts.tv_sec;
		tv->tv_usec = (suseconds_t)(ts.tv_nsec / 1000L);
		if (!offset_known) {
			struct timeval wall;

			if (gettimeofday(&wall, 0) == 0) {
				wall_offset = (long)(wall.tv_sec - ts.tv_sec);
				offset_known = 1;
			}
		}
		last = *tv;
		return (1);
	}
	if (use_monotonic == 1 && offset_known) {
		struct timeval wall;

		if (gettimeofday(&wall, 0) == 0) {
			tv->tv_sec = wall.tv_sec - wall_offset;
			tv->tv_usec = wall.tv_usec;
			last = *tv;
			return (1);
		}
	}
	if (use_monotonic == 1) {
		*tv = last; /* both clocks failed: never invent a jump */
		return (1);
	}
#endif
	(void)gettimeofday(tv, 0);
	return (0);
}

/* Sleep for a fraction of a second, retrying when a signal interrupts it. */
static void sleep_us(long usec) {
	struct timespec ts;

	ts.tv_sec = usec / 1000000L;
	ts.tv_nsec = (usec % 1000000L) * 1000L;
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

/* PRINTERFILE is a build-time template and open_printer() passes a single
 * int (the printer number) to snprintf().  Any conversion other than one
 * %c would make snprintf() read that int as a pointer or as a missing
 * argument, which is undefined behaviour.  Reject such templates up front.
 */
static int printer_template_ok(const char *tmpl) {
	int conversions = 0;
	const char *p;

	for (p = tmpl; *p != '\0'; p++) {
		if (*p != '%')
			continue;
		if (p[1] == '%') { /* a literal percent sign */
			p++;
			continue;
		}
		if (p[1] != 'c')
			return (0);
		conversions++;
		p++;
	}
	return (conversions == 1);
}

static int open_printer(int lpnumber) {
	int lp;
	int n;
	/* C7: static storage, the global device used to point into this frame.
	 * A fixed buffer is independent of the template's literal length, so
	 * a long custom PRINTERFILE cannot be truncated into a wrong path. */
	static char lpname[256];

#ifdef TESTING
	(void)lpnumber; /* the test build always uses /dev/tty */
	n = snprintf(lpname, sizeof(lpname), "/dev/tty");
#else
	n = snprintf(lpname, sizeof(lpname), PRINTERFILE, lpnumber);
#endif
	if (n < 0 || (size_t)n >= sizeof(lpname)) {
		dolog(LOGOPTS, "printer device path is too long\n");
		errno = ENAMETOOLONG;
		return (-1);
	}
	if (device == 0)
		device = lpname;
	/* R1: never open the device blocking.  A blocking write() to a printer
	 * that is out of paper, off line or simply full hangs forever, and
	 * neither SO_SNDTIMEO nor the idle timeout can rescue the process
	 * because it never gets back to poll(). */
	lp = open(device, bidir ? (O_RDWR | O_NONBLOCK) : (O_WRONLY | O_NONBLOCK));
	return (lp);
}

/* Classify an open() failure.  A *temporary* failure clears by itself and is
 * worth waiting for without any limit: the device is busy (EBUSY, e.g. another
 * writer), the node exists but its backing device is not ready yet (ENXIO -
 * no reader on a FIFO, a USB backend in the middle of a power cycle), the call
 * was interrupted (EINTR), or the system is momentarily out of resources
 * (ENOMEM/ENFILE/EMFILE).  A *permanent* failure will not fix itself by
 * waiting: the device node is gone (ENOENT), the backend is detached
 * (ENODEV), access is denied (EACCES/EPERM) or the device reported an I/O
 * error (EIO).  Those are abandoned after OPEN_PRINTER_PERMANENT_WAIT. */
static int printer_open_is_permanent(int e) {
	return !(e == EBUSY || e == EAGAIN || e == EWOULDBLOCK || e == EINTR ||
	         e == ENOMEM || e == ENFILE || e == EMFILE || e == ENXIO);
}

/* Wait for the printer to become available.  A temporary failure is retried
 * for as long as it takes, a permanent one only until
 * OPEN_PRINTER_PERMANENT_WAIT seconds have passed, so the user gets a bounded
 * chance to switch the printer on and a printer that is simply missing does not
 * hold the job forever.
 *
 * Both waits are additionally capped by OPEN_PRINTER_MAX_WAIT, counted from the
 * moment the client connected: this job holds the byte-1 job lock the whole
 * time, so an endless wait would also stop every later job for that printer
 * (V-openmax).  A permanent failure is measured from the instant the job got
 * the lock (locked_at), not from the connection: measuring from the connection
 * gave a job that had queued behind another one no grace period at all, it
 * failed the moment it was finally its turn (V-openbudget).
 *
 * U6: SIGTERM still stops the wait promptly.  U10a: the real device name and
 * errno are logged so the cause is visible in syslog.
 * V-time: both budgets are measured with mono_now(), not time(), so a clock
 * step cannot extend or shorten how long a job is allowed to wait. */
static int open_printer_retry(int lpnumber, const struct timeval *connected_at,
                              const struct timeval *locked_at) {
	struct timeval last_logged;
	int logged = 0;

	/* V-openlog: last_logged is only meaningful once a line has actually
	 * been emitted.  A zero timestamp stood in for "never logged" when the
	 * clock was gettimeofday() (epoch seconds are large), but mono_now()
	 * uses CLOCK_MONOTONIC: during the first
	 * OPEN_PRINTER_RETRY_LOG_EVERY seconds after boot the very first
	 * failure was throttled to LOG_DEBUG and vanished from syslog. */
	last_logged.tv_sec = 0;
	last_logged.tv_usec = 0;
	for (;;) {
		int lp;
		long left = OPEN_PRINTER_RETRY_INTERVAL;
		struct timeval now;
		long waited_total;
		int e = 0;
		const char *what;

		if (got_term) /* U6: stop promptly on shutdown */
			return -1;
		(void)mono_now(&now);
		waited_total = (long)(now.tv_sec - connected_at->tv_sec);
		lp = open_printer(lpnumber);
		if (lp >= 0)
			return lp;
		/* D2: save errno before dolog() (vsyslog) can clobber it. */
		e = errno;
		what = device ? device : "(unknown)";
		if (OPEN_PRINTER_MAX_WAIT > 0 &&
		    waited_total >= (long)OPEN_PRINTER_MAX_WAIT) {
			dolog(LOGOPTS,
			      "cannot open printer %s: %s after %ld seconds, "
			      "job abandoned\n",
			      what, strerror(e), waited_total);
			return -1;
		}
		if (printer_open_is_permanent(e)) {
			long budget = OPEN_PRINTER_PERMANENT_WAIT;
			long waited;

			waited = (long)(now.tv_sec - locked_at->tv_sec);
			if (waited >= budget) {
				dolog(LOGOPTS,
				      "cannot open printer %s: %s after %ld "
				      "seconds, job abandoned\n",
				      what, strerror(e), waited);
				return -1;
			}
			/* Do not overshoot the budget: wake up in time for it.
			 */
			if (left > budget - waited)
				left = budget - waited;
		}
		/* V-openlog: report the first failure and then at most one
		 * LOG_ERR every OPEN_PRINTER_RETRY_LOG_EVERY seconds.  A
		 * printer that is off for hours used to bury every other
		 * message.  -d still shows every attempt, but under a
		 * separate wording: reusing the LOG_ERR text made a throttled
		 * attempt indistinguishable from a real warning, so the
		 * throttle could not be observed in the debug log at all. */
		if (!logged || now.tv_sec - last_logged.tv_sec >=
		                   (long)OPEN_PRINTER_RETRY_LOG_EVERY) {
			dolog(LOGOPTS, "cannot open printer %s: %s, will keep retrying\n",
			      what, strerror(e));
			(void)mono_now(&last_logged);
			logged = 1;
		} else {
			dolog(LOG_DEBUG,
			      "cannot open printer %s: %s, retrying "
			      "(log throttled)\n",
			      what, strerror(e));
		}
		/* Sleep in one-second slices so SIGTERM is noticed promptly. */
		while (left-- > 0 && !got_term)
			(void)sleep(1);
	}
}

/* Take a record lock on one byte of the lock file.
 *
 * The two bytes ask different questions and therefore need different waits:
 *
 *   byte 0, the instance lock taken by get_lock()
 *       "am I the only p910nd for this printer?"  A second instance has to be
 *       told so: queueing would leave a process that never listens behind,
 *       because the daemonizing parent has already exited successfully.
 *
 *   byte 1, the job lock taken by lock_printer_job()
 *       "may I use the printer now?"  A busy printer must make the job wait
 *       its turn rather than refuse it: dropping a print job is worse than
 *       letting it queue.
 *
 * wait_seconds < 0   queue in the kernel with F_SETLKW, no time limit
 * wait_seconds == 0  one non-blocking F_SETLK attempt, then report and give up
 * wait_seconds > 0   F_SETLK polling, give up after that many seconds
 *
 * F_SETLKW blocks in the kernel so waiting costs no CPU; EINTR is retried
 * because SIGCHLD is delivered whenever another job child exits, and got_term
 * keeps shutdown prompt.
 *
 * V-lockwait: a non-zero bound exists because an unbounded F_SETLKW lets one
 * wedged job block every later job for that printer forever (PD-08).  The bound
 * is spent with F_SETLK polling so a timeout can be reported to the client (V3)
 * instead of hanging, and so SIGTERM is still noticed while waiting. */
static int take_lock(int fd, off_t start, long wait_seconds) {
	struct flock lplock;
	long waited = 0;

	memset(&lplock, 0, sizeof(lplock));
	lplock.l_type = F_WRLCK;
	lplock.l_whence = SEEK_SET;
	lplock.l_start = start;
	lplock.l_len = 1;
	lplock.l_pid = getpid();
	for (;;) {
		if (wait_seconds < 0) {
			if (fcntl(fd, F_SETLKW, &lplock) == 0)
				return (1);
		} else {
			if (fcntl(fd, F_SETLK, &lplock) == 0)
				return (1);
		}
		if (errno == EINTR) {
			if (got_term)
				return (0);
			continue;
		}
		/* EACCES/EAGAIN means somebody else already holds the range. */
		if (errno == EACCES || errno == EAGAIN || errno == EWOULDBLOCK) {
			if (wait_seconds <= 0)
				return (0); /* the caller names its own reason */
			if (got_term)
				return (0);
			if (waited >= wait_seconds) {
				dolog(LOGOPTS, "lock: still busy after %ld seconds\n", waited);
				return (0);
			}
			/* One second per iteration keeps this cheap; the poll()
			 * in the caller would not run, so the granularity is
			 * the budget's own resolution. */
			sleep_us(1000000);
			waited++;
			continue;
		}
		dolog(LOGOPTS, "lock: %s\n", strerror(errno)); /* D1 */
		return (0);
	}
}

/* V-lockdir: create LOCKDIR component by component.  /var/lock/subsys is a SUSE
 * path; on distributions that only have /var/lock (-> /run/lock), and on the
 * small embedded targets this daemon is written for, the directory is simply
 * absent and the daemon used to refuse to start (PD-02).  EEXIST is success:
 * the point is only that the path exists and is a directory. */
static int make_lock_dir(void) {
	char dir[sizeof(LOCKDIR)];
	char *p;

	(void)snprintf(dir, sizeof(dir), "%s", LOCKDIR);
	for (p = dir + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(dir, 0755) != 0 && errno != EEXIST)
			return (0);
		*p = '/';
	}
	if (mkdir(dir, 0755) != 0 && errno != EEXIST)
		return (0);
	return (1);
}

/* Take the byte-0 instance lock.  queue != 0 asks for the (x)inetd semantics,
 * where this byte is what serialises concurrent one-shot processes and must
 * therefore queue; queue == 0 asks for the standalone semantics, where a
 * second daemon must be told that one is already running. */
static int get_lock(int lpnumber, int queue) {
	(void)snprintf(lockname, sizeof(lockname), LOCKFILE, lpnumber);
	/* V-lockmode: 0644, not 0666.  The daemon itself does not set a umask
	 * on the -d and (x)inetd paths, so the requested mode was the *only*
	 * thing standing between a world-writable lock file and any local user
	 * being able to block every print job (PD-09).  O_NOFOLLOW keeps a
	 * planted symlink from redirecting the lock somewhere else. */
	lockfd = open(lockname, O_CREAT | O_RDWR | O_NOFOLLOW, 0644);
	if (lockfd < 0 && errno == ENOENT && make_lock_dir()) {
		/* V-lockdir: the directory was missing; it exists now. */
		lockfd = open(lockname, O_CREAT | O_RDWR | O_NOFOLLOW, 0644);
	}
	if (lockfd < 0) {
		dolog(LOGOPTS, "%s: %s\n", lockname, strerror(errno)); /* D1 */
		return (0);
	}
	/* Byte 0 answers "am I the only p910nd for this printer?".
	 *
	 *   standalone (server())   another daemon for this printer is already
	 *                           running, so say so and let the caller exit
	 *                           instead of queueing forever
	 * (V-instancelock) (x)inetd (one_job())    every connection is its own
	 * process and this byte is the only thing that serialises them, so it
	 * must queue exactly like the job lock: refusing here would drop the
	 * job
	 */
	if (queue) {
		if (take_lock(lockfd, 0, -1L) == 0)
			return (0);
		return (1);
	}
	if (take_lock(lockfd, 0, (long)INSTANCE_LOCK_WAIT) == 0) {
		dolog(LOGOPTS, "another p910nd is already running for printer %c\n",
		      lpnumber);
		return (0);
	}
	return (1);
}

/* R7: one printer, one writer.  The daemon holds the instance lock on byte 0
 * for its whole lifetime, every job child contends for byte 1 of the same
 * file, so jobs stay serialized although accepting no longer is.  Contention
 * queues: the job waits until the printer (or the device node for it) is
 * available instead of being rejected.
 *
 * LOCK_WAIT_TIMEOUT == 0 (the default) keeps that unbounded wait; a non-zero
 * value bounds it (V-lockwait/PD-08) so one wedged job cannot block every
 * later job forever. */
static int lock_printer_job(void) {
	long wait;

	if (lockfd < 0)
		return (1);
	wait = (LOCK_WAIT_TIMEOUT == 0) ? -1L : (long)LOCK_WAIT_TIMEOUT;
	return (take_lock(lockfd, 1, wait));
}

/* V7: release the byte-1 job lock while this connection is only waiting for the
 * printer's reply.  A pure status query (or any job whose print direction has
 * been fully delivered) must not block a real print job for the whole reply
 * window.  The lock is on byte 1 of the same file lock_printer_job() took. */
static void unlock_printer_job(void) {
	struct flock lplock;
	if (lockfd < 0)
		return;
	memset(&lplock, 0, sizeof(lplock));
	lplock.l_type = F_UNLCK;
	lplock.l_whence = SEEK_SET;
	lplock.l_start = 1;
	lplock.l_len = 1;
	(void)fcntl(lockfd, F_SETLK, &lplock);
}

static void free_lock(void) {
	/* T6: the lock file is intentionally NOT unlinked.  Unlinking it while
	 * another instance holds a lock on the same inode would let a later
	 * instance open a brand new inode and run concurrently, breaking mutual
	 * exclusion (two processes writing one printer, interleaved output).
	 * Keeping the file stable means every instance locks the same inode and
	 * fcntl() serialises them correctly; a stale lock file is harmless
	 * because the lock is released when the owning process exits. */
	if (lockfd >= 0) {
		(void)close(lockfd);
		lockfd = -1;
	}
}

/* D9: a stale pid file makes start scripts believe the daemon is running. */
static void remove_pidfile(void) {
	if (have_pidfile) {
		(void)unlink(pidfilename);
		have_pidfile = 0;
	}
}

/* D9: shared by atexit() and the termination signal handler. */
static void cleanup_and_exit(void) {
	remove_pidfile();
	free_lock();
}

/* R10: a signal handler must only set a flag.  Calling exit() from the
 * handler could re-enter stdio or the allocator while they were interrupted,
 * so the main loop does the cleanup in the normal control flow. */
static void terminate_handler(int sig) {
	(void)sig;
	got_term = 1;
}

/* R7/T5/T10a: reap forked job children (otherwise they pile up as zombies)
 * and keep the in-flight count and pid table in sync so shutdown can wait for
 * the jobs that are still running. */
static void sigchld_handler(int sig) {
	int saved_errno = errno;
	pid_t pid;
	int i;

	(void)sig;
	while ((pid = waitpid(-1, 0, WNOHANG)) > 0) {
		for (i = 0; i < child_pids_slots; i++) {
			if (child_pids[i] == pid) {
				child_pids[i] = 0;
				break;
			}
		}
		/* Every reaped child was counted at fork time, including one
		 * that record_child() could not store (PD-38, OOM). */
		if (inflight_children > 0)
			inflight_children--;
	}
	errno = saved_errno;
}

/* T10b: remember a job child so shutdown can wait for it and, if it outlives
 * the grace period, signal it.  The table is grown on demand and never caps
 * the number of accepted clients; a client is always served.
 * The caller MUST block SIGCHLD around this call: growing the table replaces
 * the array the handler scans, and that must not happen while it is walking
 * it.  Returns 0 only when memory is exhausted: the child cannot be tracked
 * but still runs, so it must remain in the in-flight count or the
 * MAX_JOB_CHILDREN cap would stop seeing it (PD-38). */
static int record_child(pid_t pid) {
	int i;

	for (i = 0; i < child_pids_slots; i++) {
		if (child_pids[i] == 0) {
			child_pids[i] = pid;
			inflight_children++;
			return (1);
		}
	}
	if (child_pids_slots == 0) {
		child_pids = (pid_t *)calloc(CHILD_PIDS_INITIAL, sizeof(pid_t));
		if (child_pids == 0) {
			dolog(LOG_ERR, "out of memory, cannot track job child %d\n",
			      (int)pid);
			inflight_children++;
			return (0);
		}
		child_pids_slots = CHILD_PIDS_INITIAL;
		i = 0;
	} else {
		int olds = child_pids_slots;
		pid_t *grown =
		    (pid_t *)realloc(child_pids, (size_t)olds * 2 * sizeof(pid_t));
		if (grown == 0) {
			dolog(LOG_ERR, "out of memory, cannot track job child %d\n",
			      (int)pid);
			inflight_children++;
			return (0);
		}
		child_pids = grown;
		child_pids_slots = olds * 2;
		memset(child_pids + olds, 0, (size_t)olds * sizeof(pid_t));
		i = olds;
	}
	child_pids[i] = pid;
	inflight_children++;
	return (1);
}

/* A1/D9: ignoring SIGPIPE keeps a vanished client from killing the daemon,
 * the write simply fails with EPIPE which is handled as "peer is gone".
 * D12: sigaction() is used for every disposition, including SIG_IGN.
 * signal() is not used at all: POSIX allows SIG_IGN to be defined as a plain
 * integer constant (it is on some libcs), and the semantics of signal()
 * differ between implementations (System V resets the disposition to
 * SIG_DFL after delivery, BSD does not). */
static void setup_signals(void) {
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	(void)sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	(void)sigaction(SIGPIPE, &sa, 0);
	(void)sigaction(SIGHUP, &sa, 0); /* R10: don't die when the terminal goes */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = terminate_handler;
	(void)sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0; /* no SA_RESTART: blocking calls must return EINTR (A3) */
	(void)sigaction(SIGTERM, &sa, 0);
	(void)sigaction(SIGINT, &sa, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigchld_handler;
	(void)sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	(void)sigaction(SIGCHLD, &sa, 0);
}

/* B1: bound every blocking socket operation, a client that connects and
 * never sends (or never comes back) used to block the daemon forever.
 * B2: keepalive detects half open connections (power loss, unplugged cable). */
static void set_socket_options(int fd) {
	int one = 1;
	struct timeval tv;
#ifdef O_NONBLOCK
	int fl = 0;
#endif
#ifdef TCP_KEEPIDLE
	int keepidle = 60; /* probe after a minute of silence */
#ifdef TCP_KEEPINTVL
	int keepintvl = 10;
#endif
#ifdef TCP_KEEPCNT
	int keepcnt = 6;
#endif
#endif

	/* V5: socket timeouts are independent of the business IDLE_TIMEOUT.  A
	 * short send timeout alone would still let write() block for that long;
	 * the socket is made non-blocking below so an unwritable peer yields
	 * EAGAIN immediately and the loop retries via poll(). */
	tv.tv_sec = SOCKET_RCVTIMEO;
	tv.tv_usec = 0;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	tv.tv_sec = SOCKET_SNDTIMEO;
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	(void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef O_NONBLOCK
	/* V5: a non-blocking socket means a stalled peer cannot pin the loop in
	 * a 30s blocking write(); EAGAIN is handled as "try again later". */
	fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0)
		(void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
#ifdef TCP_NODELAY
	/* status queries want their answer immediately */
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
#ifdef TCP_KEEPIDLE
	(void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle,
	                 sizeof(keepidle));
#ifdef TCP_KEEPINTVL
	(void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl,
	                 sizeof(keepintvl));
#endif
#ifdef TCP_KEEPCNT
	(void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
#endif
#endif
}

/* A4: rlim_max may be RLIM_INFINITY, which made the close loop in server()
 * endless; rlim_cur is what the process can actually have open.
 * Only the daemonizing path needs it, so it is not built in TESTING mode
 * (an unused static function would be a warning there). */
#ifndef TESTING
static long fd_limit(void) {
	struct rlimit rl;
	long n;

	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
		n = (long)rl.rlim_cur;
	else {
		n = sysconf(_SC_OPEN_MAX);
		if (n < 0)
			n = 256; /* last resort */
	}
	/* A child right after fork() never has more than a handful of
	 * descriptors; closing a million of them only delays startup. */
	if (n > 4096)
		n = 4096;
	return n;
}
#endif

/* Initializes the buffer, at the start. */
static void initBuffer(Buffer_t *b, int infd, int outfd, int detectEof) {
	b->detectEof = detectEof;
	b->infd = infd;
	b->outfd = outfd;
	b->startidx = 0;
	b->endidx = 0;
	b->bytes = 0;
	b->totalin = 0;
	b->totalout = 0;
	b->saw_data = 0;
	b->eof_read = 0;
	b->eof_sent = 0;
	b->zero_reads = 0; /* T3: must be initialised, it is on the stack */
	b->err = 0;
}

/* Interests of a buffer for the next poll(): the events it wants to wait for,
 * recomputed before every poll() so that a full buffer stops asking for
 * POLLIN and an empty one stops asking for POLLOUT.  The ready states poll()
 * reports back map onto the loop as follows, and every call site treats them
 * alike:
 *
 *   POLLIN            readBuffer() is attempted
 *   POLLOUT           writeBuffer() is attempted
 *   POLLERR/POLLHUP   treated as ready in both directions and the real
 *                     read()/write() decides: it either drains the bytes that
 *                     did arrive or reports the error (C2)
 *   POLLNVAL          not expected - negative descriptors are rejected on
 *                     entry, and a descriptor is closed only after its
 *                     direction is done; should it appear, the round makes no
 *                     progress and the idle/stall deadlines end the job
 *
 * poll() has no descriptor-number ceiling: the highest descriptor is bounded
 * only by RLIMIT_NOFILE, not by the width of a fixed bit set, so a number that
 * grows during a long-running daemon (or after a big job) is served like any
 * other. */
static short buffer_read_events(const Buffer_t *b) {
	/* reading after a read error would spin on the same failing fd (C2) */
	if (b->infd >= 0 && !b->eof_read && !(b->err & READ_ERR) &&
	    (size_t)b->bytes < sizeof(b->buffer))
		return POLLIN;
	return 0;
}

static short buffer_write_events(const Buffer_t *b) {
	if (b->outfd >= 0 && (!(b->err & WRITE_ERR)) &&
	    (b->bytes != 0 || (b->eof_read && !b->eof_sent)))
		return POLLOUT;
	return 0;
}

/* Reads data into a buffer from its input file. */
static ssize_t readBuffer(Buffer_t *b) {
	size_t avail;
	ssize_t result = 0;
	/* C1: only reset the ring when it is really empty.  Resetting it while
	 * an error is pending left bytes inconsistent with the indices, so
	 * writeBuffer() would emit stale or duplicated data. */
	if (b->bytes == 0) {
		/* The buffer is empty. */
		b->startidx = b->endidx = 0;
		avail = sizeof(b->buffer);
	} else if ((size_t)b->bytes == sizeof(b->buffer)) {
		/* The buffer is full. */
		avail = 0;
	} else if (b->endidx > b->startidx) {
		/* The buffer is not wrapped: from endidx to end of buffer is
		 * free. */
		avail = sizeof(b->buffer) - (size_t)b->endidx;
	} else {
		/* The buffer is wrapped: gap between endidx and startidx is
		 * free. */
		avail = (size_t)(b->startidx - b->endidx);
	}
	if (avail) {
		result = read(b->infd, b->buffer + b->endidx, avail);
		if (result > 0) {
			/* Some data was read. Update accordingly. */
			b->endidx += (int)result;
			b->totalin += (unsigned long)result;
			b->bytes += (int)result;
			b->saw_data = 1;
			b->zero_reads = 0; /* T3: a real read clears the empty streak */
			if ((size_t)b->endidx == sizeof(b->buffer)) {
				/* Time to wrap the buffer. */
				b->endidx = 0;
			}
		} else if (result < 0) {
			int e = errno;
			/* A3/C4: interrupted by a signal or nothing available
			 * right now are not errors, the caller waits on poll()
			 * again. */
			if (e == EINTR || e == EAGAIN || e == EWOULDBLOCK)
				result = 0;
			else if (e == ECONNRESET || e == ENOTCONN) {
				/* U5/PD-25: the peer forcibly closed (RST)
				 * or vanished.  This is end-of-stream, not
				 * data corruption, so mark EOF instead of
				 * READ_ERR on BOTH directions: a fully
				 * printed job must not become "Job
				 * incomplete" because the client hung up,
				 * and the printer->network direction must
				 * stop polling a dead descriptor. */
				b->eof_read = 1;
				result = 0;
			} else {
				dolog(LOGOPTS, "read: %s\n",
				      strerror(e)); /* D1: %m is a GNU extension */
				b->err |= READ_ERR;
			}
		} else if (b->detectEof) {
			dolog(LOG_DEBUG, "read: eof\n");
			b->eof_read = 1;
		} else {
			/* R6: count the empty reads, a device that only ever
			 * returns 0 (regular file, /dev/null, a wedged USB
			 * device) is finished and must not be polled again. */
			b->zero_reads++;
			result = 0; /* still data buffered, ignore it for now */
		}
	}
	/* Return the value returned by read(), which is -1 (error), or #bytes
	 * read.
	 */
	return result;
}

/* Writes data from a buffer to the output file or discard if no output file is
 * set. */
static ssize_t writeBuffer(Buffer_t *b) {
	size_t avail;
	ssize_t result = 0;
	if (b->bytes == 0 || (b->err & WRITE_ERR)) {
		/* Buffer is empty. */
		avail = 0;
	} else if (b->endidx > b->startidx) {
		/* Not wrapped: one contiguous run. */
		avail = (size_t)(b->endidx - b->startidx);
	} else {
		/* Wrapped: only up to the end of the buffer is contiguous, the
		 * rest is written on the next pass (C1: b->bytes would run past
		 * the end of the ring). */
		avail = sizeof(b->buffer) - (size_t)b->startidx;
	}
	if (avail) {
		if (b->outfd >= 0)
			result = write(b->outfd, b->buffer + b->startidx, avail);
		else
			result = (ssize_t)avail;
		if (result < 0) {
			int e = errno;
			/* A3/C4: interrupted or "try again later" are not
			 * errors. */
			if (e == EINTR || e == EAGAIN || e == EWOULDBLOCK)
				result = 0;
			else if (e == EPIPE || e == ECONNRESET) {
				/* A1: the peer is gone, not a reason to die or
				 * to declare the whole job broken. */
				dolog(LOG_DEBUG, "write: %s, peer closed\n", strerror(e));
				b->err |= WRITE_ERR;
			} else {
				/* Mark the output file in an error condition.
				 */
				dolog(LOGOPTS, "write: %s\n", strerror(e));
				b->err |= WRITE_ERR;
			}
		} else {
			/* Zero or more bytes were written. */
			b->startidx += (int)result;
			if (b->outfd >= 0)
				b->totalout += (unsigned long)result;
			b->bytes -= (int)result;
			if ((size_t)b->startidx == sizeof(b->buffer)) {
				/* Unwrap the buffer. */
				b->startidx = 0;
			}
		}
	} else if (b->eof_read && !b->eof_sent) {
		b->eof_sent = 1;
		dolog(LOG_DEBUG, "write: eof\n");
	}

	/* Return the write() result, -1 (error) or #bytes written. */
	return result;
}

/* Best effort: write out whatever is still buffered before giving up, so an
 * error never discards data that was already received (C2/C5).
 * R1: bounded in time and safe on a non-blocking device - the old loop spun
 * on a blocking write() and hung the daemon forever.
 * Returns the number of bytes that could NOT be delivered (0 if all flushed),
 * including bytes consumed by a sink (outfd < 0) after the client vanished.
 * V6: on timeout the undelivered bytes are reported at LOG_ERR as discarded and
 * unrecoverable - they are never silently dropped without a trace. */
static int flush_buffer(Buffer_t *b, int timeout_secs) {
	struct timeval start;
	struct timeval now;
	int discarded = 0;

	mono_now(&start);
	while (b->bytes > 0 && !(b->err & WRITE_ERR)) {
		ssize_t n = writeBuffer(b);
		/* PD-43: count only the bytes this pass actually removed.
		 * writeBuffer() drains one contiguous run at a time, so a
		 * wrapped ring needs two passes; adding b->bytes before each
		 * one counted the tail twice and made the "reply: N bytes not
		 * delivered" log over-report the loss. */
		if (b->outfd < 0 && n > 0)
			discarded += (int)n;
		if (n > 0) {
			mono_now(&start); /* progress: keep going */
			continue;
		}
		if (b->err & WRITE_ERR)
			break;
		if (b->outfd >= 0) {
			struct pollfd pfd;

			pfd.fd = b->outfd;
			pfd.events = POLLOUT;
			pfd.revents = 0;
			/* The timeout is in milliseconds: wake up after 100 ms
			 * even if the device never becomes writable, so the
			 * deadline below is still checked. */
			if (poll(&pfd, 1, 100) < 0) {
				if (errno == EINTR) /* A3 */
					continue;
				break;
			}
			if (!(pfd.revents & (POLLOUT | POLLERR)))
				sleep_us(NO_PROGRESS_USLEEP);
		}
		mono_now(&now);
		if (now.tv_sec - start.tv_sec >= timeout_secs) {
			/* V6: discarding buffered data is only acceptable if it
			 * is made explicit and unrecoverable.  Report it at
			 * LOG_ERR so the lost byte count is never hidden, and
			 * let the caller's print_ok / Job-incomplete line and
			 * the RST close (V3) tell the client. */
			dolog(LOG_ERR,
			      "discarded %d undelivered bytes after %d seconds "
			      "flushing to "
			      "printer; job NOT recoverable\n",
			      b->bytes, timeout_secs);
			b->err |= WRITE_ERR; /* T4: the buffered data was not
			                        all written */
			break;
		}
	}
	/* V6: undelivered plus discarded bytes, for the caller to report. */
	return b->bytes + discarded;
}

/* R5/U8a: the 0.97 change log says the device stream may only be closed when
 * the printer is no longer busy, otherwise the driver may drop the last
 * write(). A character device (and usblp) reports POLLOUT almost always, so
 * waiting for writability says nothing about the driver having drained its own
 * buffer, and the old code closed immediately, leaving the tail of the job
 * unprotected.  When the kernel exposes the parallel-port status we query it
 * with LPGETSTATUS and wait while the printer is busy; otherwise we give the
 * driver a single short, interruptible slice to flush.  #ifdef-guarded so it
 * stays portable to musl/OpenWrt (LPGETSTATUS only exists on Linux). */
static void wait_printer_idle(int lp) {
#ifdef LPGETSTATUS
	struct timeval start;
	struct timeval now;

	/* Only the real parallel port supports this; for any other device
	 * ioctl() fails and we fall through to the bounded wait below. */
	mono_now(&start);
	for (;;) {
		int st = 0;

		if (ioctl(lp, LPGETSTATUS, &st) != 0)
			break; /* not queryable, cannot do better */
		if (!(st & LP_BUSY))
			break; /* the port reports idle */
		sleep_us(100000);
		mono_now(&now);
		if (now.tv_sec - start.tv_sec >= PRINTER_DRAIN_TIMEOUT)
			break;
		if (got_term) /* U6: terminate promptly */
			break;
	}
#else
	/* No LPGETSTATUS here (FreeBSD, musl, ...): the device cannot be
	 * queried, so only the bounded wait below is possible and lp has
	 * nothing to be used for. */
	(void)lp;
#endif
	/* Non-queryable device, unqueryable port or a port that reported idle:
	 * give the driver a short, bounded chance to push the last bytes out,
	 * then stop.  This replaces the old immediate close that left the tail
	 * of the job unprotected (U8a). */
	sleep_us(200000);
	dolog(LOG_DEBUG, "device drained (or timed out), closing\n");
}

/* R4/U8b: closing a socket that still has unread data makes the kernel send
 * RST, the client then sees ECONNRESET instead of a clean end of job and any
 * status data still in flight is lost.  Send FIN first (this is the only
 * "stop sending" signal AppSocket offers; there is no application-layer
 * pause), then read what is left, bounded by BOTH a time cap and a byte cap.
 * The caps were enlarged because a large in-flight job can have far more than
 * 256KiB still sitting in the kernel receive buffer, so the old limits cut
 * the connection with RST and lost that data.  The descriptor is
 * non-blocking while the copy loop owns it (V5), so SO_RCVTIMEO alone cannot
 * make read() wait: the flag is cleared here, before the final drain. */
static void close_connection(int fd, int failure) {
	struct timeval start;
	struct timeval now;
	struct timeval tv;
	char drain[65536];
	size_t total = 0;
	ssize_t n;
#ifdef O_NONBLOCK
	int fl;
#endif

	/* V3: a failed job must be visible to the client.  A clean FIN would
	 * let CUPS think the job printed and never retry, silently losing it.
	 * Send RST instead so the client sees ECONNRESET. */
	if (failure) {
#if FAIL_WITH_RST
		struct linger l;
		l.l_onoff = 1;
		l.l_linger = 0;
		(void)setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
#endif
		(void)close(fd);
		return;
	}

	tv.tv_sec = 1;
	tv.tv_usec = 0;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#ifdef O_NONBLOCK
	/* R4/V-drain: set_socket_options() made this socket non-blocking so a
	 * stalled peer cannot pin the copy loop in write().  SO_RCVTIMEO is
	 * ignored on a non-blocking descriptor, so the first read() below
	 * returned EAGAIN immediately and the peer's in-flight bytes were
	 * still unread when close() sent RST.  The descriptor belongs to this
	 * job alone and is about to be closed, so clear the flag and let the
	 * one second timeout give the client a chance to finish. */
	fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0 && (fl & O_NONBLOCK))
		(void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
#endif
	if (shutdown(fd, SHUT_WR) < 0 && errno != ENOTCONN)
		dolog(LOG_DEBUG, "shutdown: %s\n", strerror(errno));
	mono_now(&start);
	for (;;) {
		mono_now(&now);
		if (now.tv_sec - start.tv_sec >= DRAIN_TIMEOUT ||
		    total >= DRAIN_MAX_BYTES)
			break;
		n = read(fd, drain, sizeof(drain));
		if (n < 0 && errno == EINTR)
			continue; /* A3: a signal is not the end of the stream */
		if (n <= 0)
			break;
		total += (size_t)n;
	}
	if (total >= DRAIN_MAX_BYTES)
		dolog(LOG_NOTICE,
		      "close: %lu bytes still unread after draining, sending "
		      "RST\n",
		      (unsigned long)total);
	(void)close(fd);
}

/* R6: a regular file has a real end of data, character devices (real
 * printers, /dev/null) may only return 0 bytes temporarily. */
static int printer_is_regular(int lp) {
	struct stat st;

	return (fstat(lp, &st) == 0 && S_ISREG(st.st_mode));
}

/* C3: a client that vanished must not stop the print direction, so a failed
 * write of the printer's reply only switches that direction to discard mode.
 * PD-26: clear WRITE_ERR alone.  Clearing the whole err word also erased a
 * READ_ERR raised on the same round, which disabled the read-error exit and
 * left the job polling a dead printer until the reply window expired. */
static void discard_printer_reply(Buffer_t *b) {
	b->outfd = -1;
	b->err &= ~WRITE_ERR;
}

/* Copy network data from file descriptor fd (network) to lp (printer) until EOS
 */
/* If bidir, also copy data from printer (lp) to network (fd). */
static int copy_stream(int fd, int lp) {
	int result;
	Buffer_t networkToPrinterBuffer;

	if (fd < 0 || lp < 0) {
		dolog(LOG_ERR, "refusing job on invalid descriptor(s) %d/%d\n", fd, lp);
		return (-1);
	}
	initBuffer(&networkToPrinterBuffer, fd, lp, 1);

	if (bidir) {
		struct timeval now;
		struct timeval then;
		struct timeval last_activity;
		struct timeval last_print;
		int timer = 0;
		int printer_replied = 0; /* U4: printer has produced output this job */
		int stall_extended =
		    0; /* V4: give a slow-but-alive printer a 2nd window */
		int lock_released = 0; /* V7: byte-1 lock freed once print dir done */
		struct timeval last_pn_progress; /* V1: printer->network progress */
		struct timeval job_start; /* V-jobmax: absolute ceiling on the job */
		Buffer_t printerToNetworkBuffer;
		struct pollfd pfd[2];
		long w; /* U4: reply window, set once it is needed */
		unsigned long undelivered = 0;
		int undelivered_print = 0;
		int undelivered_reply = 0;
		int print_ok = 0;
		initBuffer(&printerToNetworkBuffer, lp, fd, printer_is_regular(lp));
		mono_now(&last_activity);
		mono_now(&last_print);
		mono_now(&last_pn_progress);
		mono_now(&job_start);
		/* Finish when network sent EOF. */
		/* The printer to network stream may however not be finished:
		 * the answer to a status query arrives after the client half
		 * closed, so the loop below waits for it. */
		while (!(networkToPrinterBuffer.err & WRITE_ERR) &&
		       !(printerToNetworkBuffer.err & WRITE_ERR)) {
			int moved = 0;
			/* C2: a read error only ends the job once the buffer is
			 * drained. */
			if ((networkToPrinterBuffer.err & READ_ERR) &&
			    networkToPrinterBuffer.bytes == 0)
				break;
			/* PD-05: the same for the printer->network direction.
			 * Without this it had no read-error exit at all, so a
			 * printer failing outright was handled *worse* than one
			 * that merely had nothing to say: the zero-read counter
			 * ended the soft case at once, while a hard error only
			 * stopped the job when the reply window ran out.
			 * Draining what already arrived first keeps whatever
			 * reply the printer did manage to send. */
			if ((printerToNetworkBuffer.err & READ_ERR) &&
			    printerToNetworkBuffer.bytes == 0)
				break;
			/* T8: the loop waits with poll(), which has no
			 * descriptor-number ceiling, so a high descriptor is
			 * handled like any other. */
			pfd[0].fd = fd;
			pfd[0].events =
			    (short)(buffer_read_events(&networkToPrinterBuffer) |
			            buffer_write_events(&printerToNetworkBuffer));
			pfd[0].revents = 0;
			pfd[1].fd = lp;
			pfd[1].events =
			    (short)(buffer_read_events(&printerToNetworkBuffer) |
			            buffer_write_events(&networkToPrinterBuffer));
			pfd[1].revents = 0;

			if (timer) {
				/* Delay after reading from the printer, so the
				 */
				/* return stream cannot dominate. */
				/* Don't read from the printer until the timer
				 * expires. */
				mono_now(&now);
				if ((now.tv_sec > then.tv_sec) ||
				    (now.tv_sec == then.tv_sec && now.tv_usec > then.tv_usec))
					timer = 0;
				else
					pfd[1].events &= ~POLLIN;
			}
			mono_now(&now);
			result = poll(pfd, 2, 100);
			if (result < 0) {
				if (errno == EINTR) { /* A3 */
					if (got_term)     /* U6: terminate promptly
					                   */
						break;
					continue;
				}
				dolog(LOGOPTS, "poll: %s\n", strerror(errno));
				break;
			}
			if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
				/* Read network data. */
				result = (int)readBuffer(&networkToPrinterBuffer);
				if (result > 0) {
					moved = 1;
					dolog(LOG_DEBUG,
					      "%d.%d: read %d bytes from "
					      "network\n",
					      (int)now.tv_sec, (int)now.tv_usec, result);
					mono_now(&last_activity);
				}
			}
			if (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) {
				/* Read printer data, but pace it more slowly.
				 */
				result = (int)readBuffer(&printerToNetworkBuffer);
				if (result > 0) {
					moved = 1;
					dolog(LOG_DEBUG,
					      "%d.%d: read %d bytes from "
					      "printer\n",
					      (int)now.tv_sec, (int)now.tv_usec, result);
					mono_now(&last_activity);
					printer_replied = 1; /* U4: printer has produced
					                        output */
					mono_now(&then);
					/* wait PRINTER_READ_PACE_US before
					 * reading again. */
					/* Split the pace into whole seconds and
					 * the remainder before adding: adding
					 * it whole leaves tv_usec at up to
					 * twice the legal range, and one
					 * subtraction then still leaves an
					 * illegal value behind (a legal timeval
					 * is 0..999999). */
					then.tv_sec += (long)(PRINTER_READ_PACE_US / 1000000);
					then.tv_usec += (long)(PRINTER_READ_PACE_US % 1000000);
					/* PD-11: `>=`, not `>`.  When the sum
					 * is exactly 1000000 the strict
					 * comparison left tv_usec out of range
					 * and never carried into tv_sec. */
					if (then.tv_usec >= 1000000) {
						then.tv_usec -= 1000000;
						then.tv_sec++;
					}
					/* C8: need_clear_lp was never set to 1,
					 * so the two buffer clearing branches
					 * were unreachable. */
					timer = 1;
				} else if (!printerToNetworkBuffer.eof_read &&
				           printerToNetworkBuffer.zero_reads >=
				               PRINTER_EOF_ZERO_READS &&
				           networkToPrinterBuffer.eof_sent) {
					/* R6: the printer keeps answering with
					 * 0 bytes, stop polling it or the loop
					 * burns all CPU. PD-06: only once the
					 * print direction is finished. A
					 * character device never signals EOF,
					 * so this counter is the only
					 * end-of-data signal the loop has for
					 * the reply; applying it while the
					 * client is still sending would cut off
					 * a printer that is merely slow between
					 * two answer blocks.  Before the print
					 * direction completes the reply window
					 * below remains the authority, and that
					 * window is bounded. */
					printerToNetworkBuffer.eof_read = 1;
					dolog(LOG_NOTICE,
					      "printer %s sent no data, stop "
					      "reading from printer\n",
					      device ? device : "(unknown)");
				} else if (!printerToNetworkBuffer.eof_read &&
				           printerToNetworkBuffer.zero_reads >=
				               PRINTER_EOF_ZERO_READS) {
					/* PD-06: count reached, but the job is
					 * still going in. Start over rather
					 * than end the conversation. */
					printerToNetworkBuffer.zero_reads = 0;
				}
			}
			if (pfd[1].revents & (POLLOUT | POLLERR)) {
				/* Write data to printer. */
				result = (int)writeBuffer(&networkToPrinterBuffer);
				if (result > 0) {
					moved = 1;
					dolog(LOG_DEBUG,
					      "%d.%d: wrote %d bytes to "
					      "printer\n",
					      (int)now.tv_sec, (int)now.tv_usec, result);
					mono_now(&last_activity);
					mono_now(&last_print);
					stall_extended = 0; /* V4: real progress, full budget
					                       again */
				}
			}
			if ((pfd[0].revents & (POLLOUT | POLLERR)) ||
			    printerToNetworkBuffer.outfd == -1) {
				/* Write data to network. */
				result = (int)writeBuffer(&printerToNetworkBuffer);
				/* If socket write error, discard further data
				 * from printer */
				if (result < 0) {
					/* C3: only the printer to network
					 * direction stops, the job keeps going
					 * to the printer. */
					discard_printer_reply(&printerToNetworkBuffer);
					dolog(LOG_INFO, "network write error, discarding "
					                "further printer data\n"); /* D10
					                                            */
				} else if (result > 0) {
					if (printerToNetworkBuffer.outfd == -1)
						dolog(LOG_DEBUG,
						      "discarded %d bytes from "
						      "printer\n",
						      result);
					else {
						moved = 1;
						dolog(LOG_DEBUG,
						      "%d.%d: wrote %d bytes "
						      "to network\n",
						      (int)now.tv_sec, (int)now.tv_usec, result);
						mono_now(&last_activity);
						mono_now(&last_pn_progress); /* V1 */
					}
				}
			}
			mono_now(&now);
			/* R1/U1/U2: data pending but the printer takes none of
			 * it for PRINTER_STALL_TIMEOUT - a stalled printer (off
			 * line, out of paper, full buffer).  Stop, but do NOT
			 * mark failure here: the final flush gets one last
			 * chance and we judge by bytes delivered, so a printer
			 * that recovers within the window is never reported
			 * dead. */
			if (networkToPrinterBuffer.bytes == 0) {
				mono_now(&last_print);
				/* V4: nothing is pending, so this is not a
				 * stall: give the next one the full two windows
				 * again. */
				stall_extended = 0;
			} else {
				if (PRINTER_STALL_TIMEOUT > 0) {
					/* V4: do not destroy a slow-but-alive
					 * printer.  A device that is merely
					 * slow (heating, large buffer, walking
					 * the paper) may not accept a byte for
					 * longer than PRINTER_STALL_TIMEOUT
					 * between consumptions.  On the first
					 * stall window give it a second, equal
					 * window before declaring it dead - any
					 * byte written in either window resets
					 * the clock, and so does the extension:
					 * a printer that recovers and then
					 * stalls again later gets the same two
					 * windows, not one. V2:
					 * PRINTER_STALL_TIMEOUT == 0 means
					 * "never abandon on stall" (paper added
					 * days later).  Nothing here bounds
					 * that wait, so it also holds the
					 * byte-1 job lock for as long as it
					 * lasts; use JOB_MAX_DURATION to put a
					 * ceiling on it. */
					long stalled = now.tv_sec - last_print.tv_sec;
					if (stalled >= PRINTER_STALL_TIMEOUT) {
						if (!stall_extended) {
							stall_extended = 1;
							mono_now(&last_print);
							dolog(LOG_NOTICE,
							      "printer accepted "
							      "no data for %ld "
							      "seconds, "
							      "extending wait\n",
							      (long)PRINTER_STALL_TIMEOUT);
						} else {
							/* Report what was
							 * actually measured,
							 * not a fixed multiple:
							 * the second window is
							 * one
							 * PRINTER_STALL_TIMEOUT
							 * long, and the job may
							 * have been stalled for
							 * far less (or more)
							 * than twice that. */
							dolog(LOG_ERR,
							      "printer stalled "
							      "for %ld seconds, "
							      "%d bytes "
							      "undelivered, "
							      "abandoning job "
							      "(unrecoverable)\n",
							      stalled, networkToPrinterBuffer.bytes);
							break;
						}
					}
				}
			}
			/* V7: once the print direction is fully delivered (the
			 * client has half-closed and the network->printer
			 * buffer is empty) release the byte-1 job lock so a
			 * real print job is not blocked behind this
			 * connection's reply wait.  The printer's reply is
			 * read-only for the rest of this job, so the serial
			 * printer mutex is preserved for the next writer.  Done
			 * once. */
			if (!lock_released && networkToPrinterBuffer.eof_sent &&
			    networkToPrinterBuffer.bytes == 0) {
				unlock_printer_job();
				lock_released = 1;
			}
			/* U3/U9: idle only counts when both buffers are empty
			 * AND the client has not yet half-closed - once it has,
			 * we are waiting for the printer's reply and the
			 * reply-window block below decides.  This keeps the two
			 * directions' semantics consistent.
			 *
			 * V-probe: the same probe/real-job distinction as the
			 * unidirectional branch.  A client that has actually
			 * streamed data may legitimately pause - CUPS building
			 * pages, a slow link, a client that sends a PJL header
			 * and then waits - so it gets SILENT_TIMEOUT; only a
			 * client that never sent anything is a probe and gets
			 * the short IDLE_TIMEOUT.  Using IDLE_TIMEOUT for both
			 * truncated real bidirectional jobs and made CUPS
			 * reprint them. */
			if (networkToPrinterBuffer.bytes == 0 &&
			    printerToNetworkBuffer.bytes == 0 &&
			    !networkToPrinterBuffer.eof_read &&
			    !networkToPrinterBuffer.eof_sent) {
				long limit = (networkToPrinterBuffer.saw_data == 0)
				                 ? (long)IDLE_TIMEOUT
				                 : (long)SILENT_TIMEOUT;
				if (now.tv_sec - last_activity.tv_sec >= limit) {
					dolog(LOG_NOTICE,
					      "no data transferred for %ld "
					      "seconds, stop copy stream\n",
					      limit);
					break;
				}
			}
			/* R4/E7/T1: the job is out but the printer may still
			 * owe an answer (status query).  For a regular file
			 * eof_read marks a real end, but character devices
			 * (/dev/lpX, usblp) never signal EOF, so waiting on it
			 * pinned the connection for the full window. Once the
			 * client has half-closed and both buffers are empty,
			 * give up as soon as there has been no printer->network
			 * progress for PRINTER_REPLY_WINDOW seconds, while
			 * still forwarding anything that arrives (the buffers
			 * are non-empty while data flows). */
			if (networkToPrinterBuffer.eof_sent &&
			    networkToPrinterBuffer.bytes == 0 &&
			    printerToNetworkBuffer.bytes == 0) {
				if (printerToNetworkBuffer.eof_read) {
					dolog(LOG_INFO, "printer finished sending, "
					                "bi-directional job complete\n");
					break;
				}
				/* U4: a slow printer (out of paper, off line,
				 * busy) may answer a status query only after
				 * the user clears the condition - tens of
				 * seconds to minutes later.  Wait the long
				 * window if nothing has been heard yet, but
				 * once the printer has actually replied,
				 * release the connection shortly afterwards
				 * (preserving T1's quick release for a normal
				 * job).  Either way, forward any data that
				 * arrives. */
				w = printer_replied ? PRINTER_REPLY_IDLE : PRINTER_REPLY_WINDOW;
				if (now.tv_sec - last_activity.tv_sec >= w) {
					if (printer_replied)
						dolog(LOG_INFO, "printer reply done, "
						                "closing bi-directional "
						                "job\n");
					else
						dolog(LOG_NOTICE,
						      "no printer reply for "
						      "%ld seconds, closing "
						      "bi-directional "
						      "job\n",
						      w);
					break;
				}
			}
			/* R6: never poll with zero delay, an idle iteration
			 * always costs at least this pause. */
			/* V1: bounded, direction-independent no-progress guard
			 * for the printer->network stream.  A bi-directional
			 * job where the client half-closed and then never reads
			 * the return data (or the peer vanished) used to spin
			 * forever: every other exit in this loop requires both
			 * buffers to be empty, but the 8KB return buffer stays
			 * full because the socket will not accept it.  If data
			 * sits in that buffer yet neither reaches the client
			 * nor is read from the printer for
			 * NETWORK_STALL_TIMEOUT seconds, end the job and
			 * release the lock/connection.  Healthy transfers keep
			 * resetting last_pn_progress, so they are never cut
			 * off. */
			if (printerToNetworkBuffer.bytes > 0 &&
			    (now.tv_sec - last_pn_progress.tv_sec) >=
			        NETWORK_STALL_TIMEOUT) {
				dolog(LOG_NOTICE,
				      "return path stalled for %d seconds, "
				      "ending job\n",
				      (int)NETWORK_STALL_TIMEOUT);
				break;
			}
			/* V-jobmax: an absolute ceiling on one job.  Every
			 * other deadline above is a no-progress deadline and
			 * some of them can be switched off, so this is the only
			 * configuration in which a job is guaranteed to end -
			 * and therefore the only one in which the byte-1 job
			 * lock is guaranteed to be released. */
			if (JOB_MAX_DURATION > 0 &&
			    now.tv_sec - job_start.tv_sec >= (long)JOB_MAX_DURATION) {
				dolog(LOG_ERR,
				      "job ran for %ld seconds, abandoning "
				      "(limit %d)\n",
				      now.tv_sec - job_start.tv_sec, (int)JOB_MAX_DURATION);
				break;
			}
			if (got_term) /* U6: stop promptly on shutdown */
				break;
			if (!moved)
				sleep_us(NO_PROGRESS_USLEEP);
		}
		/* C5: deliver what was already received before giving up. */
		undelivered_print =
		    flush_buffer(&networkToPrinterBuffer, PRINTER_FLUSH_TIMEOUT);
		/* R4: and deliver the printer's answer as well.  V6: a reply
		 * lost to a vanished client must be reported, not hidden. */
		undelivered_reply =
		    flush_buffer(&printerToNetworkBuffer, PRINTER_FLUSH_TIMEOUT);
		if (undelivered_reply > 0)
			dolog(LOG_NOTICE,
			      "reply: %d bytes not delivered to the client, "
			      "discarded\n",
			      undelivered_reply);
		/* U5/C6: a job succeeds iff the print direction delivered every
		 * byte. A client that disconnected (EPIPE/ECONNRESET) only
		 * affects the printer->network reply, which is discarded - it
		 * must NOT turn a fully printed job into "Job incomplete" (that
		 * would make CUPS re-print and duplicate the job). */
		/* Judge by the bytes still undelivered, never by writing
		 * "totalout >= totalin": that comparison flips if the counters
		 * wrapped (see the note on the fields above).  Unsigned
		 * subtraction yields the true difference modulo the type width,
		 * and that difference is bounded by BUFFER_SIZE, so the result
		 * is exact whether or not a wrap happened. */
		undelivered =
		    networkToPrinterBuffer.totalin - networkToPrinterBuffer.totalout;
		print_ok = (undelivered == 0) && (undelivered_print == 0) &&
		           !(networkToPrinterBuffer.err & WRITE_ERR);
		if (!print_ok) {
			dolog(LOG_ERR,
			      "Job incomplete: %lu/%lu bytes sent to printer, "
			      "%lu/%lu bytes "
			      "sent to network\n",
			      networkToPrinterBuffer.totalout,
			      networkToPrinterBuffer.totalin,
			      printerToNetworkBuffer.totalout,
			      printerToNetworkBuffer.totalin);
			return (-1);
		}
		dolog(LOG_NOTICE,
		      "Finished job: %lu/%lu bytes sent to printer, %lu/%lu "
		      "bytes sent "
		      "to network\n",
		      networkToPrinterBuffer.totalout, networkToPrinterBuffer.totalin,
		      printerToNetworkBuffer.totalout, printerToNetworkBuffer.totalin);
		if (printerToNetworkBuffer.outfd == -1)
			dolog(LOG_INFO, "client disconnected before reply, "
			                "print completed\n");
		return (0);
	} else {
		struct timeval now;
		struct timeval last_activity;
		struct timeval last_print;
		struct timeval job_start; /* V-jobmax: absolute ceiling on the job */
		struct pollfd pfd[2];
		unsigned long undelivered = 0;
		int undelivered_print = 0;
		int print_ok = 0;
		int stall_extended =
		    0; /* V4: give a slow-but-alive printer a 2nd window */
		int lock_released = 0; /* V7: byte-1 lock freed once print dir done */
		mono_now(&last_activity);
		mono_now(&last_print);
		mono_now(&job_start);
		/* Unidirectional: simply read from network, and write to
		 * printer, but driven by poll() with a timeout, a blocking
		 * read() let one silent client stop the daemon for everybody
		 * (B1).  poll() also means a high descriptor number needs no
		 * special case. */
		while (!(networkToPrinterBuffer.err & WRITE_ERR)) {
			int moved = 0;
			/* C2: a read error only ends the job once the buffer is
			 * drained. */
			if ((networkToPrinterBuffer.err & READ_ERR) &&
			    networkToPrinterBuffer.bytes == 0)
				break;
			pfd[0].fd = fd;
			pfd[0].events = buffer_read_events(&networkToPrinterBuffer);
			pfd[0].revents = 0;
			pfd[1].fd = lp;
			pfd[1].events = buffer_write_events(&networkToPrinterBuffer);
			pfd[1].revents = 0;
			/* 1s: wake up regularly to check idle time */
			result = poll(pfd, 2, 1000);
			if (result < 0) {
				if (errno == EINTR) { /* A3 */
					if (got_term)     /* U6: terminate promptly
					                   */
						break;
					continue;
				}
				dolog(LOGOPTS, "poll: %s\n", strerror(errno));
				break;
			}
			if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
				result = (int)readBuffer(&networkToPrinterBuffer);
				if (result > 0) {
					moved = 1;
					dolog(LOG_DEBUG, "read %d bytes from network\n", result);
					mono_now(&last_activity);
				}
			}
			if (pfd[1].revents & (POLLOUT | POLLERR)) {
				result = (int)writeBuffer(&networkToPrinterBuffer);
				if (result > 0) {
					moved = 1;
					dolog(LOG_DEBUG, "wrote %d bytes to printer\n", result);
					mono_now(&last_activity);
					mono_now(&last_print);
					stall_extended = 0; /* V4: real progress, full budget
					                       again */
				}
			}
			mono_now(&now);
			/* R1/U1/U2: data pending but the device takes none of
			 * it for PRINTER_STALL_TIMEOUT.  Stop, but do NOT mark
			 * failure here; the final flush and the bytes-delivered
			 * judgement decide the verdict, so a printer that
			 * recovers within the window is not reported dead. */
			if (networkToPrinterBuffer.bytes == 0) {
				mono_now(&last_print);
				/* V4: nothing pending, so this is not a stall:
				 * give the next one the full two windows again.
				 */
				stall_extended = 0;
			} else {
				/* V2: PRINTER_STALL_TIMEOUT == 0 means "wait
				 * forever for the printer" (e.g. paper added
				 * days later) - never abandon on stall. Nothing
				 * bounds that wait in this direction: there is
				 * no NETWORK_STALL_TIMEOUT here because there
				 * is no printer->network stream, so the job -
				 * and the byte-1 job lock it holds - last until
				 * the printer recovers.  Use JOB_MAX_DURATION
				 * to put a ceiling on it. */
				if (PRINTER_STALL_TIMEOUT > 0) {
					/* V4: do not destroy a slow-but-alive
					 * printer.  A device that is merely
					 * slow (heating, large buffer, walking
					 * the paper) may not accept a byte for
					 * longer than PRINTER_STALL_TIMEOUT
					 * between consumptions.  On the first
					 * stall window give it a second, equal
					 * window before declaring it dead - any
					 * byte written in either window resets
					 * the clock, and so does the extension,
					 * so a printer that recovers and stalls
					 * again later gets the same two windows
					 * and not just one. */
					long stalled = now.tv_sec - last_print.tv_sec;
					if (stalled >= PRINTER_STALL_TIMEOUT) {
						if (!stall_extended) {
							stall_extended = 1;
							mono_now(&last_print);
							dolog(LOG_NOTICE,
							      "printer accepted "
							      "no data for %ld "
							      "seconds, "
							      "extending wait\n",
							      (long)PRINTER_STALL_TIMEOUT);
						} else {
							/* Report what was
							 * actually measured,
							 * not a fixed multiple
							 * of the timeout. */
							dolog(LOG_ERR,
							      "printer stalled "
							      "for %ld seconds, "
							      "%d bytes "
							      "undelivered, "
							      "abandoning job "
							      "(unrecoverable)\n",
							      stalled, networkToPrinterBuffer.bytes);
							break;
						}
					}
				}
			}
			/* V7: once the print direction is fully delivered (the
			 * client has half-closed and the network->printer
			 * buffer is empty) release the byte-1 job lock so a
			 * real print job is not blocked behind a connection
			 * that lingers (probe, slow reply window, etc.).  Done
			 * once.  The serial printer mutex is preserved for the
			 * next writer. */
			if (!lock_released && networkToPrinterBuffer.eof_sent &&
			    networkToPrinterBuffer.bytes == 0) {
				unlock_printer_job();
				lock_released = 1;
			}
			/* R3/R9/T2: only count idle time with an empty buffer,
			 * and never after the client half closed and we are
			 * draining.  A client that never sent a byte is a probe
			 * and gets the short IDLE_TIMEOUT; one that has
			 * streamed real data is abandoned only after
			 * SILENT_TIMEOUT of no progress, so a pause in the
			 * middle of a job is not mistaken for a dead client. */
			if (!networkToPrinterBuffer.eof_read &&
			    !networkToPrinterBuffer.eof_sent &&
			    networkToPrinterBuffer.bytes == 0) {
				long limit = (networkToPrinterBuffer.saw_data == 0)
				                 ? IDLE_TIMEOUT
				                 : SILENT_TIMEOUT;
				if (now.tv_sec - last_activity.tv_sec >= limit) {
					/* U3/U10c: stop waiting on the
					 * (possibly just slow) client, but do
					 * NOT mark the job failed here - the
					 * final flush may still deliver what is
					 * buffered, and the bytes-delivered
					 * check makes the real verdict. */
					if (networkToPrinterBuffer.totalin -
					        networkToPrinterBuffer.totalout >
					    0)
						dolog(LOG_NOTICE,
						      "no data for %ld "
						      "seconds, stopping (sent "
						      "%lu/%lu to printer)\n",
						      limit, networkToPrinterBuffer.totalout,
						      networkToPrinterBuffer.totalin);
					else
						dolog(LOG_NOTICE,
						      "no data transferred for "
						      "%ld seconds, stop copy "
						      "stream\n",
						      limit);
					break;
				}
			}
			/* U1/U3: once the client has half-closed and everything
			 * is flushed to the printer, the job is done - even if
			 * we are still inside the recoverable stall window
			 * waiting for the printer to take the last bytes. */
			if (networkToPrinterBuffer.eof_sent &&
			    networkToPrinterBuffer.bytes == 0)
				break;
			/* V-jobmax: an absolute ceiling on one job, the only
			 * deadline that cannot be switched off.  Without it a
			 * configuration such as -DPRINTER_STALL_TIMEOUT=0 has
			 * nothing that ends a job waiting on a printer that
			 * never recovers - and such a job holds the byte-1 job
			 * lock, so every later job for that printer waits too.
			 */
			if (JOB_MAX_DURATION > 0 &&
			    now.tv_sec - job_start.tv_sec >= (long)JOB_MAX_DURATION) {
				dolog(LOG_ERR,
				      "job ran for %ld seconds, abandoning "
				      "(limit %d)\n",
				      now.tv_sec - job_start.tv_sec, (int)JOB_MAX_DURATION);
				break;
			}
			/* R6: never poll with zero delay */
			if (got_term) /* U6: stop promptly on shutdown */
				break;
			if (!moved)
				sleep_us(NO_PROGRESS_USLEEP);
		}
		/* C2/U10c: flush first, THEN judge - a timeout that fired a
		 * moment before the last bytes could be written must not be
		 * reported as a permanent failure if the flush actually
		 * delivered them. */
		undelivered_print =
		    flush_buffer(&networkToPrinterBuffer, PRINTER_FLUSH_TIMEOUT);
		/* U5/C6: success iff every byte reached the printer.  A stalled
		 * or timed-out client that left data undelivered is reported as
		 * a failure, never as a misleading "Finished job". */
		/* Same wrap-safe form as above: compare the undelivered byte
		 * count, not "totalout >= totalin". */
		undelivered =
		    networkToPrinterBuffer.totalin - networkToPrinterBuffer.totalout;
		print_ok = (undelivered == 0) && (undelivered_print == 0) &&
		           !(networkToPrinterBuffer.err & WRITE_ERR);
		if (!print_ok) {
			dolog(LOG_ERR, "Job incomplete: %lu/%lu bytes sent to printer\n",
			      networkToPrinterBuffer.totalout,
			      networkToPrinterBuffer.totalin);
			return (-1);
		}
		dolog(LOG_NOTICE, "Finished job: %lu/%lu bytes sent to printer\n",
		      networkToPrinterBuffer.totalout, networkToPrinterBuffer.totalin);
		return (0);
	}
	return (0);
}

/* R7/R8: everything that can block lives here.  In standalone mode this runs
 * in a forked child so the accept loop is never blocked, under (x)inetd it
 * runs in the one and only process. */
static void handle_connection(int fd, int lpnumber) {
	int lp;
	/* connected_at caps the whole wait for the printer
	 * (OPEN_PRINTER_MAX_WAIT) and is taken here, right after accept()
	 * forked us, so queueing for the job lock is part of it.  locked_at is
	 * taken once this job actually owns the printer and is what the
	 * permanent-failure budget is measured from: a job that queued behind
	 * another one still gets its full chance to see the printer switched on
	 * (V-openbudget). V-time: both taken through mono_now() so a clock step
	 * cannot change how long a job may wait for its printer. */
	struct timeval connected_at;
	struct timeval locked_at;
	struct sockaddr_storage client;
	socklen_t clientlen = sizeof(client);
	char host[INET6_ADDRSTRLEN];
	int r = 0;

	host[0] = '\0';
	(void)mono_now(&connected_at);
	if (getpeername(fd, (struct sockaddr *)&client, &clientlen) >= 0)
		get_ip_str((struct sockaddr *)&client, host, sizeof(host));
	if (!lock_printer_job()) {
		/* Only a real lock failure (or shutdown) gets here: a busy
		 * printer queues the job.  Failure means the client must see
		 * RST (V3) so it knows to retry, rather than a clean FIN that
		 * would look like success. */
		dolog(LOG_ERR,
		      "printer %c: could not take the job lock, job abandoned, "
		      "client %s\n",
		      lpnumber, host);
		close_connection(fd, 1);
		return;
	}
	(void)mono_now(&locked_at);
	/* Make sure lp device is open...  open_printer_retry() decides how long
	 * to wait: as long as it takes for a temporary failure,
	 * OPEN_PRINTER_PERMANENT_WAIT from the moment this job got the printer
	 * for a permanent one, and never longer than OPEN_PRINTER_MAX_WAIT from
	 * the connection in either case.  It logs the reason (device name +
	 * errno). Giving up is a failure: RST to the client (V3) so the job is
	 * not silently dropped. */
	lp = open_printer_retry(lpnumber, &connected_at, &locked_at);
	if (lp < 0) {
		close_connection(fd, 1);
		return;
	}
	/* D1: errno has no meaning here, but the return tells us whether the
	 * job reached the printer. */
	r = copy_stream(fd, lp);
	if (r < 0)
		dolog(LOGOPTS, "copy_stream failed\n");
	else
		/* R5: give the device a bounded chance to take the last write
		 * before the stream is closed, otherwise the tail of the job
		 * can be lost. */
		wait_printer_idle(lp);
	(void)close(lp);
	/* V3: a failed job (printer busy, device missing, stall, discarded
	 * data) closes with RST so the client sees ECONNRESET and can retry;
	 * a successful job closes with a clean FIN (R4). */
	close_connection(fd, r < 0 ? 1 : 0);
}

static void one_job(int lpnumber) {
	struct sockaddr_storage client;
	socklen_t clientlen = sizeof(client);
	char host[INET6_ADDRSTRLEN];

	/* B1/B2: fd 0 is the socket handed over by (x)inetd */
	set_socket_options(0);
	host[0] = '\0';
	memset(&client, 0, sizeof(client));
	if (getpeername(0, (struct sockaddr *)&client, &clientlen) >= 0)
		dolog(LOG_NOTICE, "Connection from %s port %hu\n",
		      get_ip_str((struct sockaddr *)&client, host, sizeof(host)),
		      get_port((struct sockaddr *)&client));
#ifdef USE_LIBWRAP
	/* The standalone loop consults hosts_ctl() on every accepted socket.
	 * Under (x)inetd the socket is handed over on fd 0 and this is the
	 * only process that sees it, so the same check belongs here: without
	 * it a build with USE_LIBWRAP silently had no tcpwrappers access
	 * control at all in one-shot mode.  A refusal is a failed job (V3),
	 * so close with RST rather than a clean FIN. */
	if (hosts_ctl("p910nd", STRING_UNKNOWN,
	              get_ip_str((struct sockaddr *)&client, host, sizeof(host)),
	              STRING_UNKNOWN) == 0) {
		dolog(LOGOPTS, "Connection from %s port %hu rejected\n",
		      get_ip_str((struct sockaddr *)&client, host, sizeof(host)),
		      get_port((struct sockaddr *)&client));
		close_connection(0, 1);
		return;
	}
#endif
	/* queue = 1: under (x)inetd every connection is its own process, so
	 * this byte is the only thing that serialises them and it must queue.
	 */
	if (get_lock(lpnumber, 1) == 0) {
		/* T6/T10c: a silent return closed the connection with no trace.
		 * Log the failure with the client address so the operator can
		 * see why the job was dropped.  V3: close with RST rather than
		 * a bare `return`, so the client is told the job failed instead
		 * of being handed a clean FIN and believing it printed (PD-03).
		 */
		dolog(LOG_ERR,
		      "printer %c: could not acquire instance lock, refusing "
		      "connection "
		      "from %s\n",
		      lpnumber, host);
		close_connection(0, 1);
		return;
	}
	handle_connection(0, lpnumber);
	free_lock();
}

/* T10b: declared early because server() calls it on shutdown. */
static void reap_children_and_exit(int status);

static void server(int lpnumber) {
#ifdef USE_GETPROTOBYNAME
	const struct protoent *proto;
#endif
	int netfd = -1, fd, one = 1;
	int rc;
	int terminating = 0;
	struct addrinfo hints, *res, *ressave;
	const struct addrinfo *tmp;
	char service[16]; /* D7: sizeof(BASEPORT+...) was sizeof(int)+1 */
	const int bufsiz = 65536;
	int listenfds[MAX_LISTEN_FDS];
	struct pollfd listenpfd[MAX_LISTEN_FDS];
	int nlisten = 0;
	int i;
	int have_ipv4 = 0;
	int invalid_rounds = 0; /* PD-41: consecutive POLLNVAL-only rounds */

#ifndef TESTING
	if (!log_to_stdout) {
		long maxfd;
		FILE *f;

		switch (fork()) {
		case -1:
			dolog(LOGOPTS, "fork: %s\n", strerror(errno));
			exit(1);
		case 0: /* child */
			break;
		default: /* parent */
			exit(0);
		}
		/* Now in child process */
		/* A4: rlim_max may be RLIM_INFINITY, never loop on that */
		maxfd = fd_limit();
		for (fd = 0; fd < maxfd; ++fd)
			(void)close(fd);
		if (setsid() < 0) {
			dolog(LOGOPTS, "setsid: %s\n", strerror(errno));
			exit(1);
		}
		if (chdir("/") < 0) {
			dolog(LOGOPTS, "chdir: %s\n", strerror(errno));
			exit(1);
		}
		(void)umask(022);
		fd = open("/dev/null", O_RDWR); /* stdin */
		if (fd < 0) {
			dolog(LOGOPTS, "/dev/null: %s\n", strerror(errno));
			exit(1);
		}
		if (dup2(fd, 0) < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0) {
			dolog(LOGOPTS, "dup2: %s\n", strerror(errno));
			exit(1);
		}
		if (fd > 2)
			(void)close(fd);
		(void)snprintf(pidfilename, sizeof(pidfilename), PIDFILE, lpnumber);
		f = fopen(pidfilename, "w");
		if (f == NULL) {
			dolog(LOGOPTS, "%s: %s\n", pidfilename, strerror(errno));
			exit(1);
		}
		(void)fprintf(f, "%d\n", getpid());
		(void)fclose(f);
		have_pidfile = 1; /* D9: remember it for the exit handler */
	}
#endif
	/* R7: always take the instance lock (byte 0) in the surviving process,
	 * whether we daemonized, ran with -d in the foreground, or built with
	 * -DTESTING.  Without it lockfd stays -1 and the per-job byte-1 lock
	 * that serializes concurrent jobs to the same printer is a silent no-op
	 * (V7). PD-01: this call used to sit inside `#ifdef LOCKFILE_DIR`,
	 * which no build defines, so the shipping configuration had no mutual
	 * exclusion at all.  The lock directory is created on demand by
	 * get_lock().
	 *
	 * queue = 0: a second instance must be told that one is already running
	 * and exit, never queue.  Queueing would leave a process that never
	 * listens behind, and it would take the port over the moment the first
	 * instance stops - a restart would silently produce the wrong daemon
	 * (V-instancelock). */
	if (get_lock(lpnumber, 0) == 0)
		exit(1);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_flags = AI_PASSIVE;
	hints.ai_socktype = SOCK_STREAM;
	(void)snprintf(service, sizeof(service), "%hu",
	               (unsigned short)(BASEPORT + lpnumber - '0'));
	rc = getaddrinfo(bindaddr, service, &hints, &res);
	if (rc != 0) {
		/* D1: getaddrinfo() does not set errno, use gai_strerror() */
		dolog(LOGOPTS, "getaddrinfo %s port %s: %s\n",
		      bindaddr ? bindaddr : "*", service, gai_strerror(rc));
		exit(1);
	}
	ressave = res;
	/* When an IPv4 candidate exists, the IPv6 socket must be v6-only so the
	 * two wildcard binds do not fight over the same IPv4 port.  With no
	 * IPv4 candidate, keep the IPv6 socket dual stack to serve both. */
	for (tmp = ressave; tmp != 0; tmp = tmp->ai_next)
		if (tmp->ai_family == AF_INET)
			have_ipv4 = 1;
	while (res) {
#ifdef USE_GETPROTOBYNAME
		proto = getprotobyname("tcp6");
		if (proto == NULL) {
			proto = getprotobyname("tcp");
			if (proto == NULL) {
				dolog(LOGOPTS, "Cannot find protocol for TCP!\n");
				exit(1);
			}
		}
		netfd = socket(res->ai_family, res->ai_socktype, proto->p_proto);
#else
		netfd = socket(res->ai_family, res->ai_socktype, IPPROTO_IP);
#endif
		if (netfd < 0) {
			/* D13: nothing to close, socket() failed and returned
			 * -1. */
			dolog(LOGOPTS, "socket: %s\n", strerror(errno));
			res = res->ai_next;
			continue;
		}
#ifdef IPV6_V6ONLY
		if (res->ai_family == AF_INET6) {
			/* R12: with net.ipv6.bindv6only=1 an IPv6 wildcard
			 * socket refuses IPv4 clients.  When a separate IPv4
			 * listener exists, keep this one v6-only so both can
			 * bind; otherwise let it serve both families. */
			int v6only = have_ipv4 ? 1 : 0;
			if (setsockopt(netfd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only,
			               sizeof(v6only)) < 0)
				dolog(LOG_DEBUG, "setsockopt: IPV6_V6ONLY: %s\n",
				      strerror(errno));
		}
#endif
		if (setsockopt(netfd, SOL_SOCKET, SO_RCVBUF, &bufsiz, sizeof(bufsiz)) <
		    0) {
			dolog(LOGOPTS, "setsocketopt: SO_RCVBUF: %s\n", strerror(errno));
			/* not fatal if it fails */
		}
		if (setsockopt(netfd, SOL_SOCKET, SO_SNDBUF, &bufsiz, sizeof(bufsiz)) <
		    0) {
			dolog(LOGOPTS, "setsocketopt: SO_SNDBUF: %s\n", strerror(errno));
			/* not fatal if it fails */
		}
		if (setsockopt(netfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) <
		    0) {
			dolog(LOGOPTS, "setsocketopt: SO_REUSEADDR: %s\n", strerror(errno));
			close(netfd);
			res = res->ai_next;
			continue;
		}
		if (bind(netfd, res->ai_addr, res->ai_addrlen) < 0) {
			dolog(LOGOPTS, "bind: %s\n", strerror(errno));
			close(netfd);
			res = res->ai_next;
			continue;
		}
		if (listen(netfd, 30) < 0) {
			dolog(LOGOPTS, "listen: %s\n", strerror(errno));
			close(netfd);
			res = res->ai_next;
			continue;
		}
		/* Keep every address that worked.  Stopping at the first one
		 * silently refused all clients of the other family, because
		 * getaddrinfo() commonly returns IPv4 before IPv6. */
		if (nlisten < MAX_LISTEN_FDS) {
			listenfds[nlisten++] = netfd;
		} else {
			dolog(LOGOPTS, "too many listen addresses, dropping one\n");
			(void)close(netfd);
		}
		res = res->ai_next;
	}
	freeaddrinfo(ressave);
	/* V-bind: every candidate address failed.  Say so and leave, instead of
	 * entering the accept loop with descriptors that were closed on the way
	 * out and reporting the resulting EBADF as if accept() were at fault.
	 */
	if (nlisten == 0) {
		dolog(LOGOPTS, "cannot listen on %s port %s: no address worked\n",
		      bindaddr ? bindaddr : "*", service);
		exit(1);
	}
	for (;;) {
		struct sockaddr_storage client;
		socklen_t clientlen;
		char host[INET6_ADDRSTRLEN];
		pid_t pid;
		/* U7: block SIGCHLD across fork + record so the handler cannot
		 * reap a child before its pid is stored. */
		sigset_t chld_mask, chld_omask;

		/* D3: get_ip_str() always terminates the buffer, but the log
		 * lines below pass it to %s, so start from a defined empty
		 * string. */
		host[0] = '\0';
		/* R10: the signal handler only raised a flag, exit cleanly here
		 */
		if (got_term) {
			dolog(LOG_NOTICE, "terminating on signal\n");
			terminating = 1;
			/* V8: close every listening socket now so new connection
			 * attempts are refused immediately (ECONNREFUSED)
			 * instead of queuing behind a daemon that has already
			 * stopped serving. */
			for (i = 0; i < nlisten; i++)
				(void)close(listenfds[i]);
			nlisten = 0;
			break;
		}

		/* V-childcap: every client has to be served, but not every
		 * client has to be forked at once.  When the cap is reached the
		 * connection is left in the listen backlog - the client waits a
		 * little and is still served - instead of forking another
		 * child.  Without this a printer that never becomes available
		 * would fork one process per connection, each holding a
		 * descriptor and each blocked on the printer, until the machine
		 * ran out of processes or memory. */
		if (MAX_JOB_CHILDREN > 0 &&
		    inflight_children >= (sig_atomic_t)MAX_JOB_CHILDREN) {
			dolog(LOG_DEBUG, "%d job(s) in flight, deferring accept\n",
			      (int)inflight_children);
			(void)sleep(1);
			continue;
		}

		/* Wait on every listening address instead of blocking in
		 * accept() on the first one: a connection can arrive on any
		 * family.  A short timeout also lets got_term and the child cap
		 * be checked when nothing is connecting. */
		for (i = 0; i < nlisten; i++) {
			listenpfd[i].fd = listenfds[i];
			listenpfd[i].events = POLLIN;
			listenpfd[i].revents = 0;
		}
		rc = poll(listenpfd, (nfds_t)nlisten, 200);
		if (rc < 0) {
			if (errno == EINTR) /* A3 */
				continue;
			dolog(LOGOPTS, "poll: %s\n", strerror(errno));
			break;
		}
		if (rc == 0) {
			invalid_rounds = 0;
			continue;
		}
		netfd = -1;
		for (i = 0; i < nlisten; i++) {
			if (listenpfd[i].revents & (POLLIN | POLLERR | POLLHUP)) {
				netfd = listenfds[i];
				break;
			}
		}
		if (netfd < 0) {
			/* Only POLLNVAL: every listening descriptor is gone.
			 * Retry briefly, then stop serving instead of spinning
			 * forever with no socket that could ever accept a
			 * client.  A transient descriptor churn resets the
			 * counter as soon as any listener becomes usable. */
			if (++invalid_rounds >= 25) {
				dolog(LOGOPTS, "poll: all listening sockets are invalid\n");
				break;
			}
			sleep_us(NO_PROGRESS_USLEEP);
			continue;
		}
		invalid_rounds = 0;

		/* D4: accept() truncates clientlen, it has to be reset every
		 * time, otherwise a shorter address (IPv4 after IPv6) is cut
		 * off. */
		memset(&client, 0, sizeof(client));
		clientlen = sizeof(client);
		fd = accept(netfd, (struct sockaddr *)&client, &clientlen);
		if (fd < 0) {
			int e = errno;
			/* A3: a signal is not a reason to stop serving */
			if (e == EINTR)
				continue;
			/* A2: only the connection or the resources failed, the
			 * listening socket is still usable, so keep going. */
			if (e == ECONNABORTED || e == EPROTO || e == EPERM) {
				dolog(LOG_DEBUG, "accept: %s, connection aborted\n",
				      strerror(e));
				continue;
			}
			if (e == EMFILE || e == ENFILE || e == ENOBUFS || e == ENOMEM ||
			    e == EAGAIN || e == EWOULDBLOCK) {
				dolog(LOGOPTS, "accept: %s, out of resources, waiting\n",
				      strerror(e));
				(void)sleep(1); /* brief backoff instead of a spin */
				continue;
			}
			/* Only these say the listening socket itself is gone.
			 * Everything else is transient (a streams or network
			 * error such as ENOSR/ENETDOWN): stopping there would
			 * end all printing because of one failed connection. */
			if (e == EBADF || e == ENOTSOCK || e == EINVAL) {
				dolog(LOGOPTS, "accept: %s, listening socket is gone\n",
				      strerror(e));
				break;
			}
			dolog(LOGOPTS, "accept: %s, transient error, waiting\n",
			      strerror(e));
			(void)sleep(1);
			continue;
		}
		/* B1/B2: bound the socket and detect dead peers before the job
		 * starts */
		set_socket_options(fd);
#ifdef USE_LIBWRAP
		if (hosts_ctl(
		        "p910nd", STRING_UNKNOWN,
		        get_ip_str((struct sockaddr *)&client, host, sizeof(host)),
		        STRING_UNKNOWN) == 0) {
			dolog(LOGOPTS, "Connection from %s port %hu rejected\n",
			      get_ip_str((struct sockaddr *)&client, host, sizeof(host)),
			      get_port((struct sockaddr *)&client));
			close(fd);
			continue;
		}
#endif
		dolog(LOG_NOTICE, "Connection from %s port %hu accepted\n",
		      get_ip_str((struct sockaddr *)&client, host, sizeof(host)),
		      get_port((struct sockaddr *)&client));
		/* T8: a high descriptor number needs no special case here.
		 * copy_stream() waits with poll(), which has no
		 * descriptor-number ceiling, so the connection is never refused
		 * for the number it happens to get. */
		/*write(fd, "Printing", 8); */

		/* R7/R8/R9: one job per child.  A slow printer, a client that
		 * never closes or a printer that has to be retried must not
		 * keep the daemon from accepting the next connection; the
		 * printer itself stays exclusive through the job lock.  Every
		 * client is served - the number of children that exist at the
		 * same time is bounded by MAX_JOB_CHILDREN (V-childcap), and
		 * clients beyond that wait in the listen backlog instead of
		 * being refused. */
		sigemptyset(&chld_mask);
		sigaddset(&chld_mask, SIGCHLD);
		sigprocmask(SIG_BLOCK, &chld_mask, &chld_omask);
		/* D16: with -d stdout is block buffered whenever it is a pipe
		 * or a file, so the log lines written above are still in the
		 * buffer when we fork.  The child would flush that inherited
		 * copy and print every line a second time, so flush first: the
		 * child then only flushes what it wrote itself. */
		if (log_to_stdout)
			(void)fflush(NULL);
		pid = fork();
		if (pid < 0) {
			dolog(LOGOPTS, "fork: %s\n", strerror(errno));
			sigprocmask(SIG_SETMASK, &chld_omask, NULL);
			(void)close(fd);
			continue;
		}
		if (pid == 0) { /* child */
			for (i = 0; i < nlisten; i++)
				(void)close(listenfds[i]);
			/* U7: SIGCHLD was blocked above only to protect
			 * record_child() in the parent.  The job child has to
			 * run with the mask it was started with, otherwise it
			 * spends its whole life with SIGCHLD blocked. */
			sigprocmask(SIG_SETMASK, &chld_omask, NULL);
			/* The child owns neither the pid file nor the lock
			 * file, but it keeps lockfd: POSIX locks are per
			 * process, so it can take the job lock through the
			 * inherited descriptor and loses it automatically when
			 * it exits. */
			have_pidfile = 0;
			handle_connection(fd, lpnumber);
			(void)fflush(NULL); /* keep -d output in order */
			_exit(0);
		}
		(void)close(fd);
		/* U7: record the child in a free slot (growing the table if
		 * needed) while SIGCHLD is still blocked, so the handler cannot
		 * reap it before its pid is stored and the table cannot be
		 * relocated under the handler.  A failed insertion only means
		 * the child cannot be signalled at shutdown; it is still served
		 * normally. */
		(void)record_child(pid);
		sigprocmask(SIG_SETMASK, &chld_omask, NULL);
	}
	for (i = 0; i < nlisten; i++)
		(void)close(listenfds[i]);
	reap_children_and_exit(terminating ? 0 : 1);
}

/* T10b: sigchld_handler() clears child_pids[i] asynchronously, so the
 * parent must block SIGCHLD while it walks the table.  The handler never
 * runs with the signal blocked, which is what makes the plain pid_t array
 * safe to share; record_child() is already called under the same block. */
static void signal_job_children(int sig) {
	sigset_t chld_mask;
	sigset_t chld_omask;
	int i;

	(void)sigemptyset(&chld_mask);
	(void)sigaddset(&chld_mask, SIGCHLD);
	(void)sigprocmask(SIG_BLOCK, &chld_mask, &chld_omask);
	for (i = 0; i < child_pids_slots; i++) {
		if (child_pids[i] != 0)
			(void)kill(child_pids[i], sig);
	}
	(void)sigprocmask(SIG_SETMASK, &chld_omask, 0);
}

/* T10b/U6: a daemon that exits must not leave printing children as orphans.
 * init would adopt them and the init script may unmount the device mid-job.
 * Signal every tracked child and wait a BOUNDED time for them to finish; the
 * SIGCHLD handler reaps them and decrements inflight_children, so we only poll
 * that counter (calling waitpid here too would race the handler and could miss
 * a decrement).  Anything still alive after the timeout is SIGKILLed. */
static void reap_children_and_exit(int status) {
	struct timeval start, now;

	/* V8: a routine service restart (SIGTERM) must not throw away a job
	 * that is mid-print.  First, simply wait up to SHUTDOWN_GRACE seconds
	 * for the in-flight children to finish on their own - a healthy job
	 * keeps draining the printer and exits.  Only the jobs still running
	 * after that (stuck on a dead printer, out of paper for good, etc.) are
	 * then forcibly terminated. */
	if (inflight_children > 0) {
		dolog(LOG_NOTICE,
		      "terminating, waiting up to %d seconds for %d in-flight "
		      "job(s) to "
		      "finish\n",
		      (int)SHUTDOWN_GRACE, (int)inflight_children);
		mono_now(&start);
		while (inflight_children > 0) {
			sleep_us(200000);
			mono_now(&now);
			if (now.tv_sec - start.tv_sec >= SHUTDOWN_GRACE)
				break;
		}
	}
	/* Force-terminate anything still running after the grace period. */
	if (inflight_children > 0) {
		dolog(LOG_NOTICE,
		      "%d job(s) still running after grace period, sending "
		      "SIGTERM\n",
		      (int)inflight_children);
		signal_job_children(SIGTERM);
		mono_now(&start);
		while (inflight_children > 0) {
			sleep_us(200000);
			mono_now(&now);
			if (now.tv_sec - start.tv_sec >= 5)
				break;
		}
		if (inflight_children > 0) {
			dolog(LOG_NOTICE,
			      "%d job(s) still running after SIGTERM, sending "
			      "SIGKILL\n",
			      (int)inflight_children);
			signal_job_children(SIGKILL);
			mono_now(&start);
			while (inflight_children > 0) {
				sleep_us(100000);
				mono_now(&now);
				if (now.tv_sec - start.tv_sec >= 5)
					break;
			}
		}
	}
	free_lock();
	remove_pidfile(); /* D9 */
	exit(status);
}

static int is_standalone(void) {
	struct sockaddr_storage bind_addr;
	struct stat st;
	socklen_t ba_len = sizeof(bind_addr);

	/*
	 * Check to see if a socket was passed to us from (x)inetd.
	 *
	 * Use getsockname() to determine if descriptor 0 is indeed a socket
	 * (and thus we are probably a child of (x)inetd) or if it is instead
	 * something else and we are running standalone.
	 */
	if (getsockname(0, (struct sockaddr *)&bind_addr, &ba_len) == 0)
		return (0);        /* under (x)inetd */
	if (errno != ENOTSOCK) /* strange... */
		dolog(LOGOPTS, "getsockname: %s\n", strerror(errno));
	/* PD-37: getsockname() can fail with EINVAL/EBADF/ENOTCONN even when
	 * fd 0 really is the socket (x)inetd handed over.  Trust the
	 * descriptor type instead of the errno: starting a listener there
	 * leaves the delivered connection unread and drops the job. */
	if (fstat(0, &st) == 0 && S_ISSOCK(st.st_mode))
		return (0);
	return (1);
}

int main(int argc, char *argv[]) {
	int lpnumber = '0';
	char *p = 0;

	if (argc <= 0) /* in case not provided in (x)inetd config */
		progname = default_progname;
	else {
		progname = argv[0];
		p = strrchr(progname, '/');
		if (p != 0)
			progname = p + 1;
	}
	setup_signals(); /* A1/D9: before any socket or printer work */
	for (;;) {
		int c = getopt(argc, argv, "bdi:f:v");

		if (c == EOF)
			break;
		switch (c) {
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
			break;
		case 'v':
			show_version();
			exit(0); /* D5: -v must not start the daemon */
			break;
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;
	/* The synopsis allows at most one printer number, 0, 1 or 2.  A
	 * non-digit token, a multi-digit token or an extra argument used to
	 * be silently ignored, so a typo started lp0 instead of telling the
	 * operator what went wrong. */
	if (argc > 1)
		usage();
	if (argc > 0) {
		/* D14: isdigit() is only defined for EOF or an unsigned char
		 * value, passing a plain (possibly negative) char is undefined.
		 */
		if (!isdigit((unsigned char)argv[0][0]) || argv[0][1] != '\0')
			usage();
		lpnumber = (int)(unsigned char)argv[0][0];
		/* The usage line, the device template, the lock/pid file names
		 * and BASEPORT+n all assume n is 0, 1 or 2.  Accepting 3..9
		 * started a daemon for a printer that cannot exist and left the
		 * operator with a silent, unusable service. */
		if (lpnumber < '0' || lpnumber > '2')
			usage();
	}
	/* change the n in argv[0] to match the port so ps will show that */
	p = strstr(progname, "p910n");
	if (p != NULL)
		p[4] = (char)lpnumber;

	/* We used to pass (LOG_PERROR|LOG_PID|LOG_LPR|LOG_ERR) to syslog, but
	 * syslog ignored the LOG_PID and LOG_PERROR option.  I.e. the intention
	 * was to add both options but the effect was to have neither.
	 * I disagree with the intention to add PERROR.  --Stef  */
	/* D6: p is NULL when the program name does not contain "p910n".
	 * V-logfd: openlog() is needed whether or not -d was given.  dolog()
	 * falls back to syslog whenever stdout is the client's own socket,
	 * which under (x)inetd it is even with -d, so without this the ident
	 * and the LOG_PID/LOG_LPR facility would be lost exactly in the
	 * configuration where the log is the only place the operator can look.
	 */
	openlog(p != NULL ? p : progname, LOG_PID, LOG_LPR);
#ifndef TESTING
	/* PD-39: a malformed PRINTERFILE would make snprintf() pass the
	 * printer number to the wrong conversion.  Refuse to start unless -f
	 * overrides the template entirely. */
	if (device == 0 && !printer_template_ok(PRINTERFILE)) {
		dolog(LOGOPTS, "PRINTERFILE must contain exactly one %%c\n");
		exit(1);
	}
#endif
	(void)atexit(cleanup_and_exit); /* D9: remove the pid file on any exit */

	/* V-logfd: -d only decides *where the log goes*, never whether this
	 * process is a daemon or a one-shot service.  `log_to_stdout ||` used
	 * to make -d under (x)inetd skip one_job() and open a listening socket
	 * instead, so the connection inetd had handed over was never read and
	 * the job was silently dropped (PD-04). */
	if (is_standalone())
		server(lpnumber);
	else
		one_job(lpnumber);
	return (0);
}
