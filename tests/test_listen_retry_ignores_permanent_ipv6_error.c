/*
 * Regression test for a hole in the listen-retry logic: an attempt was judged
 * by the LAST address entry's error alone.
 *
 * The defect
 * ----------
 * server() walks the whole getaddrinfo() list for one attempt and then decides
 * whether to retry by looking at the errno of the final entry that failed.
 * That is wrong when the entries disagree:
 *
 *   getaddrinfo(node, service, {AF_UNSPEC, AI_PASSIVE, SOCK_STREAM}) returns
 *   one entry per family, and POSIX does not fix the order.  On an entry for
 *   IPv6, socket() fails permanently with EAFNOSUPPORT on any kernel built
 *   without IPv6 -- extremely common on the small diskless hosts this daemon
 *   targets.  On the IPv4 entry, bind() fails with EADDRNOTAVAIL while the
 *   network is not up yet -- which is precisely the transient condition the
 *   retry loop exists to survive.
 *
 * If the IPv6 entry happens to come last, the attempt looks permanently
 * hopeless, and server() exits even though the IPv4 bind would have succeeded
 * a second later once the interface got its address.  The daemon then never
 * serves a job, breaking the requirement that printer, network and daemon may
 * become ready in any order with unbounded gaps.
 *
 * The fix
 * -------
 * An attempt is only hopeless when EVERY address entry failed with an error
 * waiting can never repair (all_permanent), not merely when the last one did.
 *
 * What this test proves
 * ---------------------
 * getaddrinfo()'s result list is reversed so the IPv6 entry comes last, the
 * IPv6 socket() is made to fail with EAFNOSUPPORT, and the IPv4 bind() is made
 * to fail with EADDRNOTAVAIL a few times before succeeding.  The daemon must
 * keep retrying and must then serve a real job.  Judging by the last error, the
 * attempt looks fatal and the daemon exits within a second; judging by every
 * error, it retries and comes up.
 *
 * All three interpositions are compile-time renamings of identifiers inside
 * this translation unit, so they touch no production code and behave
 * identically on every platform.
 */
#define _GNU_SOURCE

/* These must be seen before the identifiers below are redefined. */
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>

#define main p910nd_original_main

static int my_getaddrinfo(const char *node, const char *service,
                          const struct addrinfo *hints, struct addrinfo **res);
static int my_socket(int domain, int type, int protocol);
static int my_bind(int fd, const struct sockaddr *addr, socklen_t len);

#define getaddrinfo my_getaddrinfo
#define socket my_socket
#define bind my_bind
#include "../p910nd.c"
#undef getaddrinfo
#undef socket
#undef bind
#undef main

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <arpa/inet.h>

#define TEST_PORT 9107
#define JOB "JOB-AFTER-DUALSTACK-RETRY"

static int g_reverse_list = 0;
static int g_ipv6_unavailable = 0;
static int g_bind_fail_left = 0;

/*
 * Force the IPv6 entry to the END of the list, whichever order the platform's
 * getaddrinfo() happens to produce.  That is the ordering in which the bug
 * bites, and making it explicit keeps the test deterministic everywhere
 * (glibc yields AF_INET first, other libraries may differ).
 */
static void ensure_ipv6_last(struct addrinfo **res)
{
	struct addrinfo *tail;
	struct addrinfo *prev;
	struct addrinfo *cur;

	if (*res == NULL)
		return;
	for (tail = *res; tail->ai_next != NULL; tail = tail->ai_next)
		;
	if (tail->ai_family == AF_INET6)
		return; /* already last */

	prev = NULL;
	for (cur = *res; cur != NULL;)
	{
		struct addrinfo *next = cur->ai_next;
		cur->ai_next = prev;
		prev = cur;
		cur = next;
	}
	*res = prev;
}

static int my_getaddrinfo(const char *node, const char *service,
                          const struct addrinfo *hints, struct addrinfo **res)
{
	int rc = (getaddrinfo)(node, service, hints, res);
	if (rc == 0 && g_reverse_list)
		ensure_ipv6_last(res);
	return rc;
}

static int my_socket(int domain, int type, int protocol)
{
	if (g_ipv6_unavailable && domain == AF_INET6)
	{
		errno = EAFNOSUPPORT;
		return -1;
	}
	return (socket)(domain, type, protocol);
}

static int my_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
	if (g_bind_fail_left > 0)
	{
		g_bind_fail_left--;
		errno = EADDRNOTAVAIL;
		return -1;
	}
	return (bind)(fd, addr, len);
}

static char g_devpath[256];
static pid_t g_child = -1;

static void reap_child(void)
{
	if (g_child > 0)
	{
		int status;
		(void)kill(g_child, SIGKILL);
		(void)waitpid(g_child, &status, 0);
		g_child = -1;
	}
}

static void reset_device(void)
{
	int fd = open(g_devpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
	{
		perror("open devpath");
		exit(1);
	}
	(void)close(fd);
}

static size_t slurp(const char *path, char *buf, size_t cap)
{
	FILE *f = fopen(path, "r");
	size_t n = 0;
	if (f == NULL)
		return 0;
	n = fread(buf, 1, cap, f);
	(void)fclose(f);
	return n;
}

int main(void)
{
	pid_t child;
	int i;
	int connected = 0;
	int served = 0;
	char got[256];

	(void)signal(SIGPIPE, SIG_IGN);
	(void)alarm(120);
	(void)snprintf(g_devpath, sizeof(g_devpath), "/tmp/p910nd-ds-%ld.dev",
	               (long)getpid());
	reset_device();

	child = fork();
	assert(child >= 0);
	if (child == 0)
	{
		(void)signal(SIGALRM, SIG_DFL);
		(void)alarm(90);
		g_reverse_list = 1;    /* force the IPv6 entry to be tried last */
		g_ipv6_unavailable = 1;/* socket(AF_INET6) -> EAFNOSUPPORT */
		g_bind_fail_left = 3;  /* IPv4 bind() -> EADDRNOTAVAIL x3 */
		bindaddr = NULL;
		device = g_devpath;
		log_to_stdout = 0;
		server((int)('0' + (TEST_PORT - BASEPORT)));
		_exit(0);
	}
	g_child = child;

	/*
	 * With LISTEN_RETRY_SEC == 1 the daemon needs a few seconds to get past
	 * the injected bind() failures.  Poll for a connection, then send the
	 * job and wait for it to reach the printer device.
	 */
	for (i = 0; i < 60 && !connected; i++)
	{
		struct sockaddr_in sa;
		int c = socket(AF_INET, SOCK_STREAM, 0);
		if (c >= 0)
		{
			memset(&sa, 0, sizeof(sa));
			sa.sin_family = AF_INET;
			sa.sin_port = htons((unsigned short)TEST_PORT);
			sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if (connect(c, (struct sockaddr *)&sa, sizeof(sa)) == 0)
			{
				size_t off = 0;
				size_t len = strlen(JOB);
				while (off < len)
				{
					ssize_t w = write(c, JOB + off, len - off);
					if (w < 0)
					{
						if (errno == EINTR)
							continue;
						break;
					}
					off += (size_t)w;
				}
				(void)shutdown(c, SHUT_WR);
				connected = 1;
				(void)usleep(200000);
			}
			(void)close(c);
		}
		if (!connected)
			(void)usleep(250000);
	}

	for (i = 0; i < 60 && !served; i++)
	{
		size_t n = slurp(g_devpath, got, sizeof(got) - 1);
		got[n < sizeof(got) ? n : sizeof(got) - 1] = '\0';
		if (n >= strlen(JOB) && memcmp(got, JOB, strlen(JOB)) == 0)
			served = 1;
		else
			(void)usleep(250000);
	}

	reap_child();
	if (!connected)
	{
		fprintf(stderr,
		        "FAIL: the daemon never came up; it treated the permanent "
		        "IPv6 socket() error as fatal although the IPv4 bind was "
		        "only transiently failing\n");
		(void)remove(g_devpath);
		return 1;
	}
	if (!served)
	{
		size_t n = slurp(g_devpath, got, sizeof(got) - 1);
		got[n < sizeof(got) ? n : sizeof(got) - 1] = '\0';
		fprintf(stderr,
		        "FAIL: the daemon accepted the connection but the job never "
		        "reached the printer (device %s holds %lu bytes: \"%s\")\n",
		        g_devpath, (unsigned long)n, got);
		(void)remove(g_devpath);
		return 1;
	}
	(void)remove(g_devpath);
	fprintf(stderr,
	        "PASS: a permanent IPv6 error does not mask a transient IPv4 bind "
	        "failure\n");
	return 0;
}
