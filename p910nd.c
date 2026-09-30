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
 *	Version 0.99
 *	Bug fix release: no change to the command line or to the data streams.
 *	See the "Bug fixes (0.98)" section of README.md for the list.
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

#include	<unistd.h>
#include	<stdlib.h>
#include	<stdio.h>
#include	<stdint.h>	/* D8: byte counters must be 64 bit everywhere */
#include	<getopt.h>
#include	<ctype.h>
#include	<string.h>
#include	<fcntl.h>
#include	<netdb.h>
#include	<syslog.h>
#include	<errno.h>
#include	<stdarg.h>
#include	<signal.h>	/* A1/D9: SIGPIPE and clean shutdown */
#include	<time.h>	/* nanosleep()/struct timespec (R6 polling pause) */
#include	<sys/types.h>
#include	<sys/wait.h>	/* waitpid() for forked job children (R7) */
#include	<sys/time.h>
#include	<sys/resource.h>
#include	<sys/stat.h>
#include	<sys/socket.h>
#include	<netinet/in.h>
#include	<netinet/tcp.h>	/* B2: TCP_NODELAY and keepalive tuning */
#include	<arpa/inet.h>

#ifdef	USE_LIBWRAP
#include	"tcpd.h"
int allow_severity, deny_severity;
extern int hosts_ctl(char *daemon, char *client_name, char *client_addr, char *client_user);
#endif

#define		BASEPORT	9100
#define		PIDFILE		"/var/run/p910%cd.pid"
#ifdef		LOCKFILE_DIR
#define		LOCKFILE	LOCKFILE_DIR "/p910%cd"
#else
#define		LOCKFILE	"/var/lock/subsys/p910%cd"
#endif
#ifndef		PRINTERFILE
#define         PRINTERFILE     "/dev/lp%c"
#endif
#define		LOGOPTS		LOG_ERR

#define		BUFFER_SIZE	8192

/* Idle timeout in seconds.  A job is abandoned when neither direction has
 * moved data for this long, so a client that connects and never sends
 * anything cannot wedge the single threaded daemon (B1) and a slow printer
 * or a client waiting for status is not killed (B4/B5).  Only counts while
 * both buffers are empty.  Override at build time: -DIDLE_TIMEOUT=60 */
#ifndef		IDLE_TIMEOUT
#define		IDLE_TIMEOUT	30
#endif
/* B3: upper bound on how long a transient printer open failure is retried. */
#define		OPEN_PRINTER_MAX_WAIT	30
/* B3: longest single sleep between two open attempts (exponential backoff). */
#define		OPEN_PRINTER_MAX_SLEEP	10

/* R1/T4: a printer that accepts no data for this long is considered stalled
 * and the job is abandoned (seconds).  The final flush (PRINTER_FLUSH_TIMEOUT)
 * shares this overall budget, so the two no longer stack to 70s; 30 + 10 = 40s
 * is the worst case.  Override with -DPRINTER_STALL_TIMEOUT=n */
#ifndef		PRINTER_STALL_TIMEOUT
#define		PRINTER_STALL_TIMEOUT	30
#endif
/* R1: total budget for the final flush of a job (seconds). */
#ifndef		PRINTER_FLUSH_TIMEOUT
#define		PRINTER_FLUSH_TIMEOUT	10
#endif
/* R5: how long to wait for the device to go idle before closing it. */
#ifndef		PRINTER_DRAIN_TIMEOUT
#define		PRINTER_DRAIN_TIMEOUT	5
#endif
/* T1: once the client has half-closed and both buffers are drained, keep the
 * connection only long enough for the printer's reply (status query answer)
 * to drain.  Do NOT wait on the printer direction EOF: character devices such
 * as /dev/lpX and usblp never signal EOF, so the old fixed 120s wait locked
 * the connection and the byte-1 job lock for two minutes on every bi-di job.
 * The window is measured from the last real printer->network movement, so a
 * still-talking device is never cut off.  Override with -DPRINTER_REPLY_WINDOW. */
#ifndef		PRINTER_REPLY_WINDOW
#define		PRINTER_REPLY_WINDOW	10
#endif
/* R2: how long get_lock() waits for the printer lock before giving up. */
#ifndef		LOCK_WAIT
#define		LOCK_WAIT		3
#endif
/* R7: how long a job waits for the printer before it is rejected (seconds).
 * T5: shortened to a few seconds so a contended printer fails the new job
 * fast instead of letting a pile of children block for half a minute each. */
#ifndef		JOB_LOCK_WAIT
#define		JOB_LOCK_WAIT		3
#endif
/* T5: cap on simultaneously forked job children.  Beyond this, new
 * connections are refused (backpressure) rather than forking a DoS army.
 * Override with -DMAX_CHILDREN=n. */
#ifndef		MAX_CHILDREN
#define		MAX_CHILDREN		12
#endif
/* R9/T2: a client that connected but never sent a byte is released after
 * IDLE_TIMEOUT.  A client that has actually streamed data is a real job
 * that may legitimately pause (CUPS building pages, slow link, congestion),
 * so it is only abandoned after SILENT_TIMEOUT of no progress, which must be
 * long enough not to truncate such jobs.  Default raised from 10s to 30s
 * (override with -DSILENT_TIMEOUT=n). */
#ifndef		SILENT_TIMEOUT
#define		SILENT_TIMEOUT		30
#endif
/* R6: consecutive zero byte reads from the printer before its direction is
 * treated as finished, so an EOF device cannot spin the CPU. */
#ifndef		PRINTER_EOF_ZERO_READS
#define		PRINTER_EOF_ZERO_READS	20
#endif
/* R6: pause after an iteration that moved nothing (microseconds). */
#ifndef		NO_PROGRESS_USLEEP
#define		NO_PROGRESS_USLEEP	20000
#endif
/* R4: bounds for draining a socket so close() sends FIN instead of RST. */
#ifndef		DRAIN_TIMEOUT
#define		DRAIN_TIMEOUT		3
#endif
#ifndef		DRAIN_MAX_BYTES
#define		DRAIN_MAX_BYTES	(256 * 1024)
#endif

/* Circular buffer used for each direction. */
typedef struct {
	int detectEof;		/* If nonzero, EOF is marked when read returns 0 bytes. */
	int infd;		/* Input file descriptor. */
	int outfd;		/* Output file descriptor. */
	int startidx;		/* Index of the start of valid data. */
	int endidx;		/* Index of the end of valid data. */
	int bytes;		/* The number of bytes currently buffered. */
	uint64_t totalin;	/* Total bytes that have been read (D8: >2GB jobs). */
	uint64_t totalout;	/* Total bytes that have been written. */
	int eof_read;		/* Nonzero indicates the input file has reached EOF. */
	int eof_sent;		/* Nonzero indicates the output file has fully received all data. */
	int zero_reads;		/* Consecutive read()s that returned 0 bytes (R6). */
	int err;		/* Nonzero indicates an error detected on the output file. */
#define READ_ERR  0x01
#define WRITE_ERR 0x02
	char buffer[BUFFER_SIZE];	/* Buffered data goes here. */
} Buffer_t;

static char *progname;
static char version[] = "Version 0.99";
static char copyright[] = "Copyright (c) 2008-2014 Ken Yap and others, GPLv2";
static int lockfd = -1;
static char *device = 0;
static int bidir = 0;
static char *bindaddr = 0;
static int log_to_stdout = 0;
/* D9: remembered so the pid file can be removed when the daemon goes away. */
static char pidfilename[sizeof(PIDFILE)];
static int have_pidfile = 0;
/* R11: the lock file name and whether this process owns the lock, so the
 * file is only unlinked by its owner and never from under a running peer. */
static char lockname[sizeof(LOCKFILE)];
static int lock_held = 0;
/* R10: signal handlers only raise a flag, the main loop does the work. */
static volatile sig_atomic_t got_term = 0;
/* T5/T10a: number of forked job children still running.  The SIGCHLD handler
 * reaps and decrements it; server() reads it to apply backpressure.  Replaces
 * the dead got_sigchld variable. */
static volatile sig_atomic_t inflight_children = 0;
static pid_t child_pids[MAX_CHILDREN];


/* Helper function: convert a struct sockaddr address (IPv4 and IPv6) to a string */
char *get_ip_str(const struct sockaddr *sa, char *s, size_t maxlen)
{
	if (s == 0 || maxlen == 0)
		return s;
	switch(sa->sa_family) {
		case AF_INET:
			if (inet_ntop(AF_INET, &(((const struct sockaddr_in *)sa)->sin_addr), s, maxlen) == NULL)
				(void)snprintf(s, maxlen, "Unknown AF");
			break;
		case AF_INET6:
			if (inet_ntop(AF_INET6, &(((const struct sockaddr_in6 *)sa)->sin6_addr), s, maxlen) == NULL)
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

uint16_t get_port(const struct sockaddr *sa)
{
	uint16_t port;
	switch(sa->sa_family) {
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

void usage(void)
{
	fprintf(stderr, "%s %s %s\n", progname, version, copyright);
	fprintf(stderr, "Usage: %s [-f device] [-i bindaddr] [-bvd] [0|1|2]\n", progname);
	exit(1);
}

void show_version(void)
{
	fprintf(stdout, "%s %s\n", progname, version);
}

void dolog(int level, char* msg, ...)
{
	va_list argp;
	va_start(argp, msg);
	if (log_to_stdout)
		vfprintf(stdout, msg, argp);
	else if (level != LOG_DEBUG)
		vsyslog(level, msg, argp);
	va_end(argp);	
}

/* Sleep for a fraction of a second, retrying when a signal interrupts it. */
static void sleep_us(long usec)
{
	struct timespec ts;

	ts.tv_sec = usec / 1000000L;
	ts.tv_nsec = (usec % 1000000L) * 1000L;
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

/* D2: errno of the last open attempt, saved because dolog() (vsyslog) may
 * overwrite errno before the caller gets a chance to look at it (B3). */
static int open_printer_errno = 0;

int open_printer(int lpnumber)
{
	int lp;
	int e;
	/* C7: static storage, the global device used to point into this frame. */
	static char lpname[sizeof(PRINTERFILE)];

#ifdef	TESTING
	(void)snprintf(lpname, sizeof(lpname), "/dev/tty");
#else
	(void)snprintf(lpname, sizeof(lpname), PRINTERFILE, lpnumber);
#endif
	if (device == 0)
		device = lpname;
	/* R1: never open the device blocking.  A blocking write() to a printer
	 * that is out of paper, off line or simply full hangs forever, and
	 * neither SO_SNDTIMEO nor the idle timeout can rescue the process
	 * because it never gets back to select(). */
	if ((lp = open(device, bidir ? (O_RDWR|O_NONBLOCK) : (O_WRONLY|O_NONBLOCK))) == -1) {
		/* D2: save errno, dolog() may clobber it, and log one message only. */
		e = errno;
		open_printer_errno = e;
		if (e == EBUSY)
			dolog(LOGOPTS, "%s: %s, will try opening later\n", device, strerror(e));
		else
			dolog(LOGOPTS, "%s: %s\n", device, strerror(e));
	} else
		open_printer_errno = 0;
	return (lp);
}

/* B3: a busy printer is worth retrying, a missing device node or a
 * permission problem will never fix itself by waiting. */
static int printer_open_is_temporary(int e)
{
	return (e == EBUSY || e == EAGAIN || e == EINTR ||
		e == ENOMEM || e == ENFILE || e == EMFILE);
}

/* B3: bounded exponential backoff instead of "while (...) sleep(10);" which
 * pinned the daemon (or the inetd instance) forever and stopped accepting. */
static int open_printer_retry(int lpnumber)
{
	int lp;
	int waited = 0;
	int sleepfor = 1;

	for (;;) {
		if ((lp = open_printer(lpnumber)) >= 0)
			return lp;
		if (!printer_open_is_temporary(open_printer_errno) || waited >= OPEN_PRINTER_MAX_WAIT)
			return -1;
		(void)sleep((unsigned int)sleepfor);
		waited += sleepfor;
		if (sleepfor < OPEN_PRINTER_MAX_SLEEP)
			sleepfor *= 2;
	}
}

/* R2: F_SETLKW waited forever, so a second instance (and therefore
 * "service p910nd restart") hung with no message at all.  Take the lock
 * without blocking and retry for at most LOCK_WAIT seconds. */
static int take_lock(int lockfd, off_t start, int wait_seconds, const char *busy_msg)
{
	struct flock lplock;
	int waited_ms = 0;

	memset(&lplock, 0, sizeof(lplock));
	lplock.l_type = F_WRLCK;
	lplock.l_whence = SEEK_SET;
	lplock.l_start = start;
	lplock.l_len = 1;
	lplock.l_pid = getpid();
	for (;;) {
		if (fcntl(lockfd, F_SETLK, &lplock) == 0)
			return (1);
		if (errno != EACCES && errno != EAGAIN && errno != EINTR) {
			dolog(LOGOPTS, "lock: %s\n", strerror(errno));	/* D1 */
			return (0);
		}
		if (waited_ms >= wait_seconds * 1000) {
			if (busy_msg != 0)
				dolog(LOGOPTS, "%s\n", busy_msg);
			return (0);
		}
		sleep_us(200000);	/* brief backoff, never an endless wait */
		waited_ms += 200;
	}
}

int get_lock(int lpnumber)
{
	(void)snprintf(lockname, sizeof(lockname), LOCKFILE, lpnumber);
	if ((lockfd = open(lockname, O_CREAT | O_RDWR, 0666)) < 0) {
		dolog(LOGOPTS, "%s: %s\n", lockname, strerror(errno));	/* D1 */
		return (0);
	}
	if (take_lock(lockfd, 0, LOCK_WAIT,
		      "another p910nd is already running for this printer") == 0)
		return (0);
	lock_held = 1;	/* R11: only the owner removes the lock file */
	return (1);
}

/* R7: one printer, one writer.  The daemon holds the instance lock on byte 0
 * for its whole lifetime, every job child contends for byte 1 of the same
 * file, so jobs stay serialized although accepting no longer is. */
static int lock_printer_job(void)
{
	if (lockfd < 0)
		return (1);
	return (take_lock(lockfd, 1, JOB_LOCK_WAIT, "printer is busy, job rejected"));
}

void free_lock(void)
{
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
	lock_held = 0;
}

/* D9: a stale pid file makes start scripts believe the daemon is running. */
static void remove_pidfile(void)
{
	if (have_pidfile) {
		(void)unlink(pidfilename);
		have_pidfile = 0;
	}
}

/* D9: shared by atexit() and the termination signal handler. */
static void cleanup_and_exit(void)
{
	remove_pidfile();
	free_lock();
}

/* R10: a signal handler must only set a flag.  Calling exit() from the
 * handler could re-enter stdio or the allocator while they were interrupted,
 * so the main loop does the cleanup in the normal control flow. */
static void terminate_handler(int sig)
{
	(void)sig;
	got_term = 1;
}

/* R7/T5/T10a: reap forked job children (otherwise they pile up as zombies)
 * and keep the in-flight count and pid table in sync so server() can apply
 * backpressure and reap on shutdown. */
static void sigchld_handler(int sig)
{
	int saved_errno = errno;
	pid_t pid;
	int i;

	(void)sig;
	while ((pid = waitpid(-1, 0, WNOHANG)) > 0) {
		for (i = 0; i < MAX_CHILDREN; i++) {
			if (child_pids[i] == pid) {
				child_pids[i] = 0;
				if (inflight_children > 0)
					inflight_children--;
				break;
			}
		}
	}
	errno = saved_errno;
}

/* A1/D9: ignoring SIGPIPE keeps a vanished client from killing the daemon,
 * the write simply fails with EPIPE which is handled as "peer is gone". */
static void setup_signals(void)
{
	struct sigaction sa;

	(void)signal(SIGPIPE, SIG_IGN);
	(void)signal(SIGHUP, SIG_IGN);	/* R10: don't die when the terminal goes */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = terminate_handler;
	(void)sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;	/* no SA_RESTART: blocking calls must return EINTR (A3) */
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
static void set_socket_options(int fd)
{
	int one = 1;
	struct timeval tv;

	tv.tv_sec = IDLE_TIMEOUT;
	tv.tv_usec = 0;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	(void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef	TCP_NODELAY
	/* status queries want their answer immediately */
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
#ifdef	TCP_KEEPIDLE
	{
		int keepidle = 60;	/* probe after a minute of silence */
		(void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
#ifdef	TCP_KEEPINTVL
		{
			int keepintvl = 10;
			(void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
		}
#endif
#ifdef	TCP_KEEPCNT
		{
			int keepcnt = 6;
			(void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
		}
#endif
	}
#endif
}

/* A4: rlim_max may be RLIM_INFINITY, which made the close loop in server()
 * endless; rlim_cur is what the process can actually have open. */
static long fd_limit(void)
{
	struct rlimit rl;
	long n;

	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
		n = (long)rl.rlim_cur;
	else {
		n = (long)sysconf(_SC_OPEN_MAX);
		if (n < 0)
			n = 256;	/* last resort */
	}
	/* A child right after fork() never has more than a handful of
	 * descriptors; closing a million of them only delays startup. */
	if (n > 4096)
		n = 4096;
	return n;
}

/* Initializes the buffer, at the start. */
void initBuffer(Buffer_t * b, int infd, int outfd, int detectEof)
{
	b->detectEof = detectEof;
	b->infd = infd;
	b->outfd = outfd;
	b->startidx = 0;
	b->endidx = 0;
	b->bytes = 0;
	b->totalin = 0;
	b->totalout = 0;
	b->eof_read = 0;
	b->eof_sent = 0;
	b->zero_reads = 0;	/* T3: must be initialised, it is on the stack */
	b->err = 0;
}

/* Sets the readfds and writefds (used by select) based on current buffer state. */
void prepBuffer(Buffer_t * b, fd_set * readfds, fd_set * writefds)
{
	if (b->outfd>=0 && (!(b->err & WRITE_ERR)) && (b->bytes != 0 || (b->eof_read && !b->eof_sent))) {
		FD_SET(b->outfd, writefds);
	}
	/* reading after a read error would spin on the same failing fd (C2) */
	if (b->infd>=0 && !b->eof_read && !(b->err & READ_ERR) &&
	    (size_t)b->bytes < sizeof(b->buffer)) {
		FD_SET(b->infd, readfds);
	}
}

/* Reads data into a buffer from its input file. */
ssize_t readBuffer(Buffer_t * b)
{
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
		/* The buffer is not wrapped: from endidx to end of buffer is free. */
		avail = sizeof(b->buffer) - (size_t)b->endidx;
	} else {
		/* The buffer is wrapped: gap between endidx and startidx is free. */
		avail = (size_t)(b->startidx - b->endidx);
	}
	if (avail) {
		result = read(b->infd, b->buffer + b->endidx, avail);
		if (result > 0) {
			/* Some data was read. Update accordingly. */
			b->endidx += (int)result;
			b->totalin += (uint64_t)result;
			b->bytes += (int)result;
			b->zero_reads = 0;	/* T3: a real read clears the empty streak */
			if ((size_t)b->endidx == sizeof(b->buffer)) {
				/* Time to wrap the buffer. */
				b->endidx = 0;
			}
		} else if (result < 0) {
			int e = errno;
			/* A3/C4: interrupted by a signal or nothing available right
			 * now are not errors, the caller waits on select() again. */
			if (e == EINTR || e == EAGAIN || e == EWOULDBLOCK)
				result = 0;
			else {
				dolog(LOGOPTS, "read: %s\n", strerror(e));	/* D1: %m is a GNU extension */
				b->err |= READ_ERR;
			}
		}
		else if (b->detectEof) {
			dolog(LOG_DEBUG, "read: eof\n");
			b->eof_read = 1;
		} else {
			/* R6: count the empty reads, a device that only ever
			 * returns 0 (regular file, /dev/null, a wedged USB
			 * device) is finished and must not be polled again. */
			b->zero_reads++;
			result = 0; // in case there is still data in the buffer, ignore the error by now
		}
	}
	/* Return the value returned by read(), which is -1 (error), or #bytes read. */
	return result;
}

/* Writes data from a buffer to the output file or discard if no output file is set. */
ssize_t writeBuffer(Buffer_t * b)
{
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
		if (b->outfd>=0)
			result = write(b->outfd, b->buffer + b->startidx, avail);
		else
			result = (ssize_t)avail;
		if (result < 0) {
			int e = errno;
			/* A3/C4: interrupted or "try again later" are not errors. */
			if (e == EINTR || e == EAGAIN || e == EWOULDBLOCK)
				result = 0;
			else if (e == EPIPE || e == ECONNRESET) {
				/* A1: the peer is gone, not a reason to die or to
				 * declare the whole job broken. */
				dolog(LOG_DEBUG, "write: %s, peer closed\n", strerror(e));
				b->err |= WRITE_ERR;
			} else {
				/* Mark the output file in an error condition. */
				dolog(LOGOPTS, "write: %s\n", strerror(e));
				b->err |= WRITE_ERR;
			}
		} else {
			/* Zero or more bytes were written. */
			b->startidx += (int)result;
			if (b->outfd>=0)
				b->totalout += (uint64_t)result;
			b->bytes -= (int)result;
			if ((size_t)b->startidx == sizeof(b->buffer)) {
				/* Unwrap the buffer. */
				b->startidx = 0;
			}
		}
	}
	else if (b->eof_read && !b->eof_sent) {
		b->eof_sent = 1;
		dolog(LOG_DEBUG, "write: eof\n");
	}
	
	/* Return the write() result, -1 (error) or #bytes written. */
	return result;
}

/* Best effort: write out whatever is still buffered before giving up, so an
 * error never discards data that was already received (C2/C5).
 * R1: bounded in time and safe on a non-blocking device - the old loop spun
 * on a blocking write() and hung the daemon forever. */
static void flush_buffer(Buffer_t * b, int timeout_secs)
{
	struct timeval start;
	struct timeval now;

	gettimeofday(&start, 0);
	while (b->bytes > 0 && !(b->err & WRITE_ERR)) {
		if (writeBuffer(b) > 0) {
			gettimeofday(&start, 0);	/* progress: keep going */
			continue;
		}
		if (b->err & WRITE_ERR)
			break;
		if (b->outfd >= 0) {
			fd_set writefds;
			struct timeval tv;

			FD_ZERO(&writefds);
			FD_SET(b->outfd, &writefds);
			tv.tv_sec = 0;
			tv.tv_usec = 100000;
			if (select(b->outfd + 1, 0, &writefds, 0, &tv) < 0) {
				if (errno == EINTR)	/* A3 */
					continue;
				break;
			}
			if (!FD_ISSET(b->outfd, &writefds))
				sleep_us(NO_PROGRESS_USLEEP);
		}
		gettimeofday(&now, 0);
		if (now.tv_sec - start.tv_sec >= timeout_secs) {
			dolog(LOG_NOTICE, "gave up flushing %d bytes after %d seconds, job not delivered\n",
			      b->bytes, timeout_secs);
			b->err |= WRITE_ERR;	/* T4: the buffered data was not all written */
			break;
		}
	}
}

/* R5: the 0.97 change log says the device stream may only be closed when the
 * printer is no longer busy, otherwise the driver may drop the last write().
 * Wait until the device can take data again, with a hard limit. */
static void wait_printer_idle(int lp)
{
	struct timeval start;
	struct timeval now;
	int loops = PRINTER_DRAIN_TIMEOUT * 10;

	gettimeofday(&start, 0);
	while (loops-- > 0) {
		fd_set writefds;
		struct timeval tv;

		FD_ZERO(&writefds);
		FD_SET(lp, &writefds);
		tv.tv_sec = 0;
		tv.tv_usec = 100000;
		if (select(lp + 1, 0, &writefds, 0, &tv) < 0) {
			if (errno == EINTR)	/* A3 */
				continue;
			return;
		}
		if (FD_ISSET(lp, &writefds))
			return;		/* room in the device: no longer busy */
		gettimeofday(&now, 0);
		if (now.tv_sec - start.tv_sec >= PRINTER_DRAIN_TIMEOUT)
			break;
	}
	dolog(LOG_NOTICE, "printer still busy after %d seconds, closing anyway\n",
	      (int)PRINTER_DRAIN_TIMEOUT);
}

/* R4: closing a socket that still has unread data makes the kernel send RST,
 * the client then sees ECONNRESET instead of a clean end of job and any
 * status data still in flight is lost.  Send FIN first, then read what is
 * left, bounded in time and volume. */
static void close_connection(int fd)
{
	struct timeval start;
	struct timeval now;
	struct timeval tv;
	char drain[4096];
	size_t total = 0;
	ssize_t n;

	tv.tv_sec = 1;
	tv.tv_usec = 0;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (shutdown(fd, SHUT_WR) < 0 && errno != ENOTCONN)
		dolog(LOG_DEBUG, "shutdown: %s\n", strerror(errno));
	gettimeofday(&start, 0);
	for (;;) {
		gettimeofday(&now, 0);
		if (now.tv_sec - start.tv_sec >= DRAIN_TIMEOUT || total >= DRAIN_MAX_BYTES)
			break;
		n = read(fd, drain, sizeof(drain));
		if (n <= 0)
			break;
		total += (size_t)n;
	}
	(void)close(fd);
}

/* R6: a regular file has a real end of data, character devices (real
 * printers, /dev/null) may only return 0 bytes temporarily. */
static int printer_is_regular(int lp)
{
	struct stat st;

	return (fstat(lp, &st) == 0 && S_ISREG(st.st_mode));
}

/* Copy network data from file descriptor fd (network) to lp (printer) until EOS */
/* If bidir, also copy data from printer (lp) to network (fd). */
int copy_stream(int fd, int lp)
{
	int result;
	Buffer_t networkToPrinterBuffer;

	/* T8: select() with an fd >= FD_SETSIZE writes past the end of the
	 * fd_set, which is undefined behaviour (random corruption / crash).
	 * A long-running daemon can reach high descriptor numbers, so refuse
	 * the job instead of risking it. */
	if (fd < 0 || lp < 0 || fd >= FD_SETSIZE || lp >= FD_SETSIZE) {
		dolog(LOG_ERR, "T8: descriptor(s) %d/%d out of select() range, refusing job\n", fd, lp);
		return (-1);
	}
	initBuffer(&networkToPrinterBuffer, fd, lp, 1);

	if (bidir) {
		struct timeval now;
		struct timeval then;
		struct timeval timeout;
		struct timeval last_activity;
		struct timeval last_print;
		int timer = 0;
		int moved;
		Buffer_t printerToNetworkBuffer;
		initBuffer(&printerToNetworkBuffer, lp, fd, printer_is_regular(lp));
		fd_set readfds;
		fd_set writefds;
		gettimeofday(&last_activity, 0);
		gettimeofday(&last_print, 0);
		/* Finish when network sent EOF. */
		/* The printer to network stream may however not be finished: the
		 * answer to a status query arrives after the client half closed,
		 * so the loop below waits for it. */
		while (!(networkToPrinterBuffer.err & WRITE_ERR) && !(printerToNetworkBuffer.err & WRITE_ERR)) {
			int maxfd = lp > fd ? lp : fd;
			moved = 0;
			/* C2: a read error only ends the job once the buffer is drained. */
			if ((networkToPrinterBuffer.err & READ_ERR) && networkToPrinterBuffer.bytes == 0)
				break;
			FD_ZERO(&readfds);
			FD_ZERO(&writefds);
			prepBuffer(&networkToPrinterBuffer, &readfds, &writefds);
			prepBuffer(&printerToNetworkBuffer, &readfds, &writefds);

			if (timer) {
				/* Delay after reading from the printer, so the */
				/* return stream cannot dominate. */
				/* Don't read from the printer until the timer expires. */
				gettimeofday(&now, 0);
				if ((now.tv_sec > then.tv_sec) || (now.tv_sec == then.tv_sec && now.tv_usec > then.tv_usec))
					timer = 0;
				else
					FD_CLR(lp, &readfds);
			}
			gettimeofday(&now, 0);
			timeout.tv_sec = 0;
			timeout.tv_usec = 100000;
			result = select(maxfd + 1, &readfds, &writefds, 0, &timeout);
			if (result < 0) {
				if (errno == EINTR)	/* A3: interrupted by a signal */
					continue;
				dolog(LOGOPTS, "select: %s\n", strerror(errno));
				break;
			}
			if (FD_ISSET(fd, &readfds)) {
				/* Read network data. */
				result = (int)readBuffer(&networkToPrinterBuffer);
				if (result > 0) {
					moved = 1;
					dolog(LOG_DEBUG,"%d.%d: read %d bytes from network\n", (int)now.tv_sec, (int)now.tv_usec, result);
					gettimeofday(&last_activity, 0);
				}
			}
			if (FD_ISSET(lp, &readfds)) {
				/* Read printer data, but pace it more slowly. */
				result = (int)readBuffer(&printerToNetworkBuffer);
				if (result > 0) {
					moved = 1;
					dolog(LOG_DEBUG,"%d.%d: read %d bytes from printer\n", (int)now.tv_sec, (int)now.tv_usec, result);
					gettimeofday(&last_activity, 0);
					gettimeofday(&then, 0);
					// wait 100 msec before reading again.
					then.tv_usec += 100000;
					if (then.tv_usec > 1000000) {
						then.tv_usec -= 1000000;
						then.tv_sec++;
					}
					/* C8: need_clear_lp was never set to 1, so the two
					 * buffer clearing branches were unreachable. */
					timer = 1;
				} else if (!printerToNetworkBuffer.eof_read &&
					   printerToNetworkBuffer.zero_reads >= PRINTER_EOF_ZERO_READS) {
					/* R6: the printer keeps answering with 0 bytes,
					 * stop polling it or the loop burns all CPU. */
					printerToNetworkBuffer.eof_read = 1;
					dolog(LOG_INFO, "printer sent no data, stop reading from printer\n");
				}
			}
			if (FD_ISSET(lp, &writefds)) {
				/* Write data to printer. */
				result = (int)writeBuffer(&networkToPrinterBuffer);
				if (result > 0) {
					moved = 1;
					dolog(LOG_DEBUG,"%d.%d: wrote %d bytes to printer\n", (int)now.tv_sec, (int)now.tv_usec, result);
					gettimeofday(&last_activity, 0);
					gettimeofday(&last_print, 0);
				}
			}
			if (FD_ISSET(fd, &writefds) || printerToNetworkBuffer.outfd == -1) {
				/* Write data to network. */
				result = (int)writeBuffer(&printerToNetworkBuffer);
				/* If socket write error, discard further data from printer */
				if (result < 0) {
					/* C3: only the printer to network direction stops,
					 * the job keeps going to the printer. */
					printerToNetworkBuffer.outfd = -1;
					printerToNetworkBuffer.err = 0;
					result = 0;
					dolog(LOG_INFO,"network write error, discarding further printer data\n");	/* D10 */
				}
				else if (result > 0) {
					if (printerToNetworkBuffer.outfd == -1)
						dolog(LOG_DEBUG,"discarded %d bytes from printer\n",result);				
					else {
						moved = 1;
						dolog(LOG_DEBUG,"%d.%d: wrote %d bytes to network\n", (int)now.tv_sec, (int)now.tv_usec, result);
						gettimeofday(&last_activity, 0);
					}
				}
			}
			gettimeofday(&now, 0);
			/* R1: the device has data pending but takes none of it.  This is
			 * a stalled printer (off line, out of paper, full buffer), so
			 * give up instead of waiting forever. */
			if (networkToPrinterBuffer.bytes == 0)
				gettimeofday(&last_print, 0);
			else if (now.tv_sec - last_print.tv_sec >= PRINTER_STALL_TIMEOUT) {
				dolog(LOG_NOTICE,"printer accepted no data for %d seconds, stop copy stream\n", (int)PRINTER_STALL_TIMEOUT);
				networkToPrinterBuffer.err |= WRITE_ERR;	/* job not delivered */
				break;
			}
			/* R3/R9: idle only counts when both buffers are empty, and never
			 * after the client has half closed - then we are just waiting
			 * for the printer to drain. */
			if (networkToPrinterBuffer.bytes == 0 && printerToNetworkBuffer.bytes == 0 &&
			    !networkToPrinterBuffer.eof_read &&
			    now.tv_sec - last_activity.tv_sec >= IDLE_TIMEOUT) {
				dolog(LOG_NOTICE,"no data transferred for %d seconds, stop copy stream\n", (int)IDLE_TIMEOUT);
				break;
			}
			/* R4/E7/T1: the job is out but the printer may still owe an
			 * answer (status query).  For a regular file eof_read marks a
			 * real end, but character devices (/dev/lpX, usblp) never signal
			 * EOF, so waiting on it pinned the connection for the full window.
			 * Once the client has half-closed and both buffers are empty, give
			 * up as soon as there has been no printer->network progress for
			 * PRINTER_REPLY_WINDOW seconds, while still forwarding anything
			 * that arrives (the buffers are non-empty while data flows). */
			if (networkToPrinterBuffer.eof_sent &&
			    networkToPrinterBuffer.bytes == 0 && printerToNetworkBuffer.bytes == 0) {
				if (printerToNetworkBuffer.eof_read) {
					dolog(LOG_INFO,
					      "printer finished sending, bi-directional job complete\n");
					break;
				}
				if (now.tv_sec - last_activity.tv_sec >= PRINTER_REPLY_WINDOW) {
					dolog(LOG_NOTICE,
					      "no printer reply for %d seconds, closing bi-directional job\n",
					      (int)PRINTER_REPLY_WINDOW);
					break;
				}
			}
			/* R6: never poll with zero delay, an idle iteration always
			 * costs at least this pause. */
			if (!moved)
				sleep_us(NO_PROGRESS_USLEEP);
		}
		/* C5: deliver what was already received before giving up. */
		flush_buffer(&networkToPrinterBuffer, PRINTER_FLUSH_TIMEOUT);
		/* R4: and deliver the printer's answer as well */
		flush_buffer(&printerToNetworkBuffer, PRINTER_FLUSH_TIMEOUT);
		/* T4/C6: distinguish a clean finish from a truncated one. */
		if (networkToPrinterBuffer.err || printerToNetworkBuffer.err)
			dolog(LOG_ERR,
			      "Job incomplete: %llu/%llu bytes sent to printer, %llu/%llu bytes sent to network\n",
			      (unsigned long long)networkToPrinterBuffer.totalout,
			      (unsigned long long)networkToPrinterBuffer.totalin,
			      (unsigned long long)printerToNetworkBuffer.totalout,
			      (unsigned long long)printerToNetworkBuffer.totalin);
		else
			dolog(LOG_NOTICE,
			      "Finished job: %llu/%llu bytes sent to printer, %llu/%llu bytes sent to network\n",
			      (unsigned long long)networkToPrinterBuffer.totalout,
			      (unsigned long long)networkToPrinterBuffer.totalin,
			      (unsigned long long)printerToNetworkBuffer.totalout,
			      (unsigned long long)printerToNetworkBuffer.totalin);
		/* C6: an error in either direction has to be reported. */
		return ((networkToPrinterBuffer.err || printerToNetworkBuffer.err) ? -1 : 0);
	} else {
		struct timeval now;
		struct timeval timeout;
		struct timeval last_activity;
		struct timeval last_print;
		fd_set readfds;
		fd_set writefds;
		int maxfd = lp > fd ? lp : fd;
		int moved;
		gettimeofday(&last_activity, 0);
		gettimeofday(&last_print, 0);
		/* Unidirectional: simply read from network, and write to printer,
		 * but driven by select() with a timeout, a blocking read() let one
		 * silent client stop the daemon for everybody (B1). */
		while (!networkToPrinterBuffer.eof_sent && !(networkToPrinterBuffer.err & WRITE_ERR)) {
			moved = 0;
			/* C2: a read error only ends the job once the buffer is drained. */
			if ((networkToPrinterBuffer.err & READ_ERR) && networkToPrinterBuffer.bytes == 0)
				break;
			FD_ZERO(&readfds);
			FD_ZERO(&writefds);
			prepBuffer(&networkToPrinterBuffer, &readfds, &writefds);
			timeout.tv_sec = 1;	/* wake up regularly to check idle time */
			timeout.tv_usec = 0;
			result = select(maxfd + 1, &readfds, &writefds, 0, &timeout);
			if (result < 0) {
				if (errno == EINTR)	/* A3 */
					continue;
				dolog(LOGOPTS, "select: %s\n", strerror(errno));
				break;
			}
			if (FD_ISSET(fd, &readfds)) {
				result = (int)readBuffer(&networkToPrinterBuffer);
				if (result > 0) {
					moved = 1;
					dolog(LOG_DEBUG,"read %d bytes from network\n",result);
					gettimeofday(&last_activity, 0);
				}
			}
			if (FD_ISSET(lp, &writefds)) {
				result = (int)writeBuffer(&networkToPrinterBuffer);
				if (result > 0) {
					moved = 1;
					dolog(LOG_DEBUG,"wrote %d bytes to printer\n",result);
					gettimeofday(&last_activity, 0);
					gettimeofday(&last_print, 0);
				}
			}
			gettimeofday(&now, 0);
			/* R1: data is pending but the device takes none of it */
			if (networkToPrinterBuffer.bytes == 0)
				gettimeofday(&last_print, 0);
			else if (now.tv_sec - last_print.tv_sec >= PRINTER_STALL_TIMEOUT) {
				dolog(LOG_NOTICE,"printer accepted no data for %d seconds, stop copy stream\n", (int)PRINTER_STALL_TIMEOUT);
				networkToPrinterBuffer.err |= WRITE_ERR;	/* job not delivered */
				break;
			}
			/* R3/R9/T2: only count idle time with an empty buffer, and never
			 * after the client half closed and we are draining.  A client
			 * that never sent a byte is a probe and gets the short
			 * IDLE_TIMEOUT; one that has streamed real data is abandoned
			 * only after SILENT_TIMEOUT of no progress, so a pause in the
			 * middle of a job is not mistaken for a dead client. */
			if (!networkToPrinterBuffer.eof_read && networkToPrinterBuffer.bytes == 0) {
				long limit = (networkToPrinterBuffer.totalin == 0) ? IDLE_TIMEOUT : SILENT_TIMEOUT;
				if (now.tv_sec - last_activity.tv_sec >= limit) {
					if (networkToPrinterBuffer.totalout < networkToPrinterBuffer.totalin) {
						dolog(LOG_ERR,
						      "no data for %ld seconds, job incomplete: %llu/%llu bytes sent to printer\n",
						      limit,
						      (unsigned long long)networkToPrinterBuffer.totalout,
						      (unsigned long long)networkToPrinterBuffer.totalin);
						networkToPrinterBuffer.err |= WRITE_ERR;
					} else {
						dolog(LOG_NOTICE,
						      "no data transferred for %ld seconds, stop copy stream\n", limit);
					}
					break;
				}
			}
			/* R6: never poll with zero delay */
			if (!moved)
				sleep_us(NO_PROGRESS_USLEEP);
		}
		/* C2: don't throw away data received before the error. */
		flush_buffer(&networkToPrinterBuffer, PRINTER_FLUSH_TIMEOUT);
		/* T4/C6: report a truncated job as a failure, not a success. */
		if (networkToPrinterBuffer.err)
			dolog(LOG_ERR, "Job incomplete: %llu/%llu bytes sent to printer\n",
			      (unsigned long long)networkToPrinterBuffer.totalout,
			      (unsigned long long)networkToPrinterBuffer.totalin);
		else
			dolog(LOG_NOTICE, "Finished job: %llu/%llu bytes sent to printer\n",
			      (unsigned long long)networkToPrinterBuffer.totalout,
			      (unsigned long long)networkToPrinterBuffer.totalin);
	}
	return (networkToPrinterBuffer.err?-1:0);
}

/* R7/R8: everything that can block lives here.  In standalone mode this runs
 * in a forked child so the accept loop is never blocked, under (x)inetd it
 * runs in the one and only process. */
static void handle_connection(int fd, int lpnumber)
{
	int lp;
	struct sockaddr_storage client;
	socklen_t clientlen = sizeof(client);
	char host[INET6_ADDRSTRLEN];

	host[0] = '\0';
	if (getpeername(fd, (struct sockaddr *)&client, &clientlen) >= 0)
		get_ip_str((struct sockaddr *)&client, host, sizeof(host));
	if (!lock_printer_job()) {
		/* T6/T10c: the printer was busy; log the rejection with the client
		 * address.  We keep the clean FIN (R4) rather than an abrupt RST:
		 * AppSocket has no application-layer acknowledgement, so a RST would
		 * only risk discarding in-flight data while the client still believes
		 * the job succeeded.  The LOG_ERR at least makes the rejection
		 * visible; see the Third-round notes in README for the trade-off. */
		dolog(LOG_ERR, "printer %c busy, job rejected from %s\n", lpnumber, host);
		close_connection(fd);
		return;
	}
	/* Make sure lp device is open... */
	/* B3: bounded backoff instead of sleeping forever on a missing device */
	if ((lp = open_printer_retry(lpnumber)) < 0) {
		dolog(LOGOPTS, "cannot open printer, job abandoned\n");
		close_connection(fd);
		return;
	}
	/* D1: errno has no meaning here */
	if (copy_stream(fd, lp) < 0)
		dolog(LOGOPTS, "copy_stream failed\n");
	else
		/* R5: give the device a bounded chance to take the last write
		 * before the stream is closed, otherwise the tail of the job can
		 * be lost.  A failed job (stalled printer) is closed right away. */
		wait_printer_idle(lp);
	(void)close(lp);
	close_connection(fd);	/* R4: FIN, not RST */
}

void one_job(int lpnumber)
{
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
	if (get_lock(lpnumber) == 0) {
		/* T6/T10c: a silent return closed the connection with no trace.  Log
		 * the failure with the client address so the operator can see why the
		 * job was dropped. */
		dolog(LOG_ERR, "printer %c: could not acquire instance lock, refusing connection from %s\n",
		      lpnumber, host);
		return;
	}
	handle_connection(0, lpnumber);
	free_lock();
}

/* T10b: declared early because server() calls it on shutdown. */
static void reap_children_and_exit(int status);

void server(int lpnumber)
{
#ifdef	USE_GETPROTOBYNAME
	struct protoent *proto;
#endif
	int netfd = -1, fd, one = 1;
	int rc;
	int terminating = 0;
	struct addrinfo hints, *res, *ressave;
	char service[16];	/* D7: sizeof(BASEPORT+...) was sizeof(int)+1 */
	FILE *f;
	const int bufsiz = 65536;

#ifndef	TESTING
	if (!log_to_stdout)
	{
		long maxfd;
		switch (fork()) {
		case -1:
			dolog(LOGOPTS, "fork: %s\n", strerror(errno));
			exit(1);
		case 0:		/* child */
			break;
		default:		/* parent */
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
		fd = open("/dev/null", O_RDWR);	/* stdin */
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
		if ((f = fopen(pidfilename, "w")) == NULL) {
			dolog(LOGOPTS, "%s: %s\n", pidfilename, strerror(errno));
			exit(1);
		}
		(void)fprintf(f, "%d\n", getpid());
		(void)fclose(f);
		have_pidfile = 1;	/* D9: remember it for the exit handler */
	}
	if (get_lock(lpnumber) == 0)
		exit(1);
#endif
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_flags = AI_PASSIVE;
	hints.ai_socktype = SOCK_STREAM;
	(void)snprintf(service, sizeof(service), "%hu", (unsigned short)(BASEPORT + lpnumber - '0'));
	if ((rc = getaddrinfo(bindaddr, service, &hints, &res)) != 0) {
		/* D1: getaddrinfo() does not set errno, use gai_strerror() */
		dolog(LOGOPTS, "getaddrinfo %s port %s: %s\n",
		      bindaddr ? bindaddr : "*", service, gai_strerror(rc));
		exit(1);
	}
	ressave = res;
	while (res) {
#ifdef	USE_GETPROTOBYNAME
		if ((proto = getprotobyname("tcp6")) == NULL) {
			if ((proto = getprotobyname("tcp")) == NULL) {
				dolog(LOGOPTS, "Cannot find protocol for TCP!\n");
				exit(1);
			}
		}
		if ((netfd = socket(res->ai_family, res->ai_socktype, proto->p_proto)) < 0)
#else
		if ((netfd = socket(res->ai_family, res->ai_socktype, IPPROTO_IP)) < 0)
#endif
		{
			dolog(LOGOPTS, "socket: %s\n", strerror(errno));
			close(netfd);
			res = res->ai_next;
			continue;
		}
#ifdef	IPV6_V6ONLY
		if (res->ai_family == AF_INET6) {
			/* R12: with net.ipv6.bindv6only=1 an IPv6 wildcard socket
			 * refuses IPv4 clients, so ask for dual stack explicitly. */
			int v6only = 0;
			if (setsockopt(netfd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0)
				dolog(LOG_DEBUG, "setsockopt: IPV6_V6ONLY: %s\n", strerror(errno));
		}
#endif
		if (setsockopt(netfd, SOL_SOCKET, SO_RCVBUF, &bufsiz, sizeof(bufsiz)) < 0) {
			dolog(LOGOPTS, "setsocketopt: SO_RCVBUF: %s\n", strerror(errno));
			/* not fatal if it fails */
		}
		if (setsockopt(netfd, SOL_SOCKET, SO_SNDBUF, &bufsiz, sizeof(bufsiz)) < 0) {
			dolog(LOGOPTS, "setsocketopt: SO_SNDBUF: %s\n", strerror(errno));
			/* not fatal if it fails */
		}
		if (setsockopt(netfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
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
		break;
		}
	freeaddrinfo(ressave);
	for (;;) {
		struct sockaddr_storage client;
		socklen_t clientlen;
		char host[INET6_ADDRSTRLEN];
		pid_t pid;

		/* R10: the signal handler only raised a flag, exit cleanly here */
		if (got_term) {
			dolog(LOG_NOTICE, "terminating on signal\n");
			terminating = 1;
			break;
		}

		/* D4: accept() truncates clientlen, it has to be reset every
		 * time, otherwise a shorter address (IPv4 after IPv6) is cut off. */
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
				dolog(LOG_DEBUG, "accept: %s, connection aborted\n", strerror(e));
				continue;
			}
			if (e == EMFILE || e == ENFILE || e == ENOBUFS || e == ENOMEM ||
			    e == EAGAIN || e == EWOULDBLOCK) {
				dolog(LOGOPTS, "accept: %s, out of resources, waiting\n", strerror(e));
				(void)sleep(1);	/* brief backoff instead of a spin */
				continue;
			}
			/* EBADF/ENOTSOCK/EINVAL: the listening socket is really gone */
			dolog(LOGOPTS, "accept: %s\n", strerror(e));
			break;
		}
		/* B1/B2: bound the socket and detect dead peers before the job starts */
		set_socket_options(fd);
#ifdef	USE_LIBWRAP
		if (hosts_ctl("p910nd", STRING_UNKNOWN, get_ip_str((struct sockaddr *)&client, host, sizeof(host)), STRING_UNKNOWN) == 0) {
			dolog(LOGOPTS,
			       "Connection from %s port %hu rejected\n", get_ip_str((struct sockaddr *)&client, host, sizeof(host)), get_port((struct sockaddr *)&client));
			close(fd);
			continue;
		}
#endif
		dolog(LOG_NOTICE, "Connection from %s port %hu accepted\n", get_ip_str((struct sockaddr *)&client, host, sizeof(host)), get_port((struct sockaddr *)&client));
		/* T8: an fd at or past FD_SETSIZE would overflow the fd_set that
		 * copy_stream() builds for select(), so reject it here. */
		if (fd >= FD_SETSIZE) {
			dolog(LOG_ERR, "T8: descriptor %d out of select() range, refusing connection\n", fd);
			(void)close(fd);
			continue;
		}
		/*write(fd, "Printing", 8); */

		/* T5: bound the number of in-flight children.  Past the limit we
		 * refuse the connection (backpressure) rather than forking a
		 * denial-of-service army that all block on the printer lock. */
		if (inflight_children >= MAX_CHILDREN) {
			dolog(LOG_NOTICE,
			      "too many in-flight jobs (%d), refusing connection from %s port %hu\n",
			      (int)inflight_children,
			      get_ip_str((struct sockaddr *)&client, host, sizeof(host)),
			      get_port((struct sockaddr *)&client));
			(void)close(fd);
			continue;
		}

		/* R7/R8/R9: one job per child.  A slow printer, a client that
		 * never closes or a printer that has to be retried must not keep
		 * the daemon from accepting the next connection; the printer
		 * itself stays exclusive through the job lock. */
		pid = fork();
		if (pid < 0) {
			dolog(LOGOPTS, "fork: %s\n", strerror(errno));
			(void)close(fd);
			continue;
		}
		if (pid == 0) {		/* child */
			(void)close(netfd);
			/* The child owns neither the pid file nor the lock file, but
			 * it keeps lockfd: POSIX locks are per process, so it can
			 * take the job lock through the inherited descriptor and
			 * loses it automatically when it exits. */
			have_pidfile = 0;
			lock_held = 0;
			handle_connection(fd, lpnumber);
			(void)fflush(NULL);	/* keep -d output in order */
			_exit(0);
		}
		(void)close(fd);
		/* T5: track the child so the SIGCHLD handler can reap it and the
		 * shutdown path can signal it. */
		if (inflight_children < MAX_CHILDREN)
			child_pids[inflight_children] = pid;
		inflight_children++;
	}
	(void)close(netfd);
	reap_children_and_exit(terminating ? 0 : 1);
}

/* T10b: a daemon that exits must not leave printing children as orphans.
 * init would adopt them and the init script may unmount the device mid-job.
 * Signal every tracked child and wait a bounded time for them to finish. */
static void reap_children_and_exit(int status)
{
	int i;
	struct timeval start, now;

	for (i = 0; i < MAX_CHILDREN; i++) {
		if (child_pids[i] != 0)
			(void)kill(child_pids[i], SIGTERM);
	}
	if (inflight_children > 0) {
		dolog(LOG_NOTICE, "terminating, waiting for %d in-flight job(s)\n",
		      (int)inflight_children);
		gettimeofday(&start, 0);
		while (inflight_children > 0) {
			(void)waitpid(-1, 0, 0);
			gettimeofday(&now, 0);
			if (now.tv_sec - start.tv_sec >= 5)
				break;
		}
		if (inflight_children > 0)
			dolog(LOG_NOTICE,
			      "%d job(s) still running, leaving them to finish\n",
			      (int)inflight_children);
	}
	free_lock();
	remove_pidfile();	/* D9 */
	exit(status);
}

int is_standalone(void)
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
		return (0);	/* under (x)inetd */
	if (errno != ENOTSOCK)	/* strange... */
		dolog(LOGOPTS, "getsockname: %s\n", strerror(errno));
	return (1);
}

int main(int argc, char *argv[])
{
	int c, lpnumber;
	char *p;

	if (argc <= 0)		/* in case not provided in (x)inetd config */
		progname = "p910nd";
	else {
		progname = argv[0];
		if ((p = strrchr(progname, '/')) != 0)
			progname = p + 1;
	}
	lpnumber = '0';
	setup_signals();	/* A1/D9: before any socket or printer work */
	while ((c = getopt(argc, argv, "bdi:f:v")) != EOF) {
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
			exit(0);	/* D5: -v must not start the daemon */
			break;
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 0) {
		if (isdigit(argv[0][0]))
			lpnumber = argv[0][0];
	}
	/* change the n in argv[0] to match the port so ps will show that */
	if ((p = strstr(progname, "p910n")) != NULL)
		p[4] = lpnumber;

	/* We used to pass (LOG_PERROR|LOG_PID|LOG_LPR|LOG_ERR) to syslog, but
	 * syslog ignored the LOG_PID and LOG_PERROR option.  I.e. the intention
	 * was to add both options but the effect was to have neither.
	 * I disagree with the intention to add PERROR.  --Stef  */
	/* D6: p is NULL when the program name does not contain "p910n" */
	if (!log_to_stdout)
		openlog(p != NULL ? p : progname, LOG_PID, LOG_LPR);
	(void)atexit(cleanup_and_exit);	/* D9: remove the pid file on any exit */

	if (log_to_stdout || is_standalone())
		server(lpnumber);
	else
		one_job(lpnumber);
	return (0);
}
