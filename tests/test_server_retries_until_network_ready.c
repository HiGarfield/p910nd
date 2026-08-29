/*
 * Regression test for Bug: server() gave up permanently -- exit(1) -- when the
 * listening socket could not be created, so a daemon started before the
 * network was ready never served a single job.
 *
 * The defect
 * ----------
 * server() treated every listen-setup failure as fatal:
 *
 *   - getaddrinfo() != 0        -> dolog(); free_lock(); exit(1);
 *   - no socket could be bound  -> dolog(); free_lock(); exit(1);
 *
 * But the printer, the network and the daemon may become ready in ANY order
 * with unbounded gaps between them (stated requirement).  The two failure
 * modes above are exactly what "the network is not ready yet" looks like:
 *
 *   - EADDRNOTAVAIL: the address given with -i is not assigned to any
 *     interface yet -- the classic boot race on a diskless print server,
 *     where p910nd starts long before the interface gets its address;
 *   - EAI_AGAIN / EAI_NONAME: the -i name does not resolve because the
 *     resolver is not up yet.
 *
 * The printer side already retried without limit
 * (`while ((lp = open_printer(lpnumber)) == -1) sleep(10);`); the network side
 * did not, so one early failure killed the daemon forever.
 *
 * The fix
 * -------
 * server() now retries the whole listen setup in a loop.  Failures are
 * classified:
 *
 *   - listen_error_is_permanent() / gai_error_is_permanent(): mistakes that
 *     waiting can never repair stay fatal (a bad argument, an unsupported
 *     address family or protocol, a privileged port without permission, our
 *     own malformed hints);
 *   - everything else is retried every LISTEN_RETRY_SEC, with the diagnostic
 *     rate-limited by listen_log_due() so a permanently unreachable network
 *     cannot flood syslog.
 *
 * The wait is a blocking select(), so an unbounded gap costs no CPU.
 *
 * What this test proves
 * ---------------------
 * 1. Classification: unit assertions on both predicates (pure functions, no
 *    timing), pinning down which errors must be retried and which stay fatal.
 * 2. Recovery, end to end: with the port held by a foreign process, server()
 *    must STAY ALIVE (pre-fix it exits(1) within milliseconds) and must then
 *    acquire the port once it is released -- proven by an actual connect()
 *    succeeding, which requires a listening socket in the kernel.
 * 3. Unresolvable bind address: server() must stay alive and keep retrying
 *    instead of exiting.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define FIRST_PORT 9100
#define LAST_PORT 9109

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

/* Bind and listen on 127.0.0.1:port, i.e. occupy it the way a foreign process
 * (or a still-shutting-down peer instance) would.  -1 if the port is taken. */
static int squat_on_port(int port)
{
	int s;
	int one = 1;
	struct sockaddr_in sa;

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return -1;
	(void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0)
	{
		(void)close(s);
		return -1;
	}
	if (listen(s, 5) < 0)
	{
		(void)close(s);
		return -1;
	}
	return s;
}

/* 1 when a TCP connection to 127.0.0.1:port is accepted, i.e. something is
 * listening there and its backlog took our connection. */
static int can_connect(int port)
{
	int c;
	int rc;
	struct sockaddr_in sa;

	c = socket(AF_INET, SOCK_STREAM, 0);
	if (c < 0)
		return -1;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	rc = connect(c, (struct sockaddr *)&sa, sizeof(sa));
	(void)close(c);
	return (rc == 0) ? 1 : 0;
}

static pid_t spawn_server(int lpnumber, char *addr, int fd_to_close)
{
	pid_t p = fork();
	assert(p >= 0);
	if (p == 0)
	{
		(void)signal(SIGALRM, SIG_DFL);
		(void)alarm(150);
		/*
		 * The listening socket that occupies the port is inherited across
		 * fork(); the daemon must not keep it open, otherwise the port
		 * stays bound by its own inherited descriptor and no retry could
		 * ever succeed.
		 */
		if (fd_to_close >= 0)
			(void)close(fd_to_close);
		bindaddr = addr;
		log_to_stdout = 0;
		server(lpnumber);
		_exit(0);
	}
	return p;
}

/* Nonzero when the child has already terminated. */
static int child_exited(pid_t p, int *status)
{
	return waitpid(p, status, WNOHANG) == p;
}

/* 1. Pure classification: which listen-setup failures must be retried and
 * which are genuinely hopeless. */
static void test_error_classification(void)
{
	/* Transient / "network not ready yet": must be retried. */
	assert(listen_error_is_permanent(EADDRNOTAVAIL) == 0);
	assert(listen_error_is_permanent(EADDRINUSE) == 0);
	assert(listen_error_is_permanent(ENETDOWN) == 0);
	assert(listen_error_is_permanent(ENETUNREACH) == 0);
	assert(listen_error_is_permanent(ENOBUFS) == 0);
	assert(listen_error_is_permanent(ENOMEM) == 0);
	assert(listen_error_is_permanent(0) == 0);

	/* Hopeless: retrying could never make these succeed. */
	assert(listen_error_is_permanent(EINVAL) == 1);
	assert(listen_error_is_permanent(EAFNOSUPPORT) == 1);
	assert(listen_error_is_permanent(EPROTONOSUPPORT) == 1);
	assert(listen_error_is_permanent(EOPNOTSUPP) == 1);
	assert(listen_error_is_permanent(EACCES) == 1);
	assert(listen_error_is_permanent(EPERM) == 1);

	/* The resolver may simply not be up yet. */
	assert(gai_error_is_permanent(EAI_AGAIN) == 0);
	assert(gai_error_is_permanent(EAI_NONAME) == 0);
	assert(gai_error_is_permanent(EAI_SYSTEM) == 0);
	assert(gai_error_is_permanent(EAI_MEMORY) == 0);

	/* Our own hints / service string: our bug, not the network's. */
	assert(gai_error_is_permanent(EAI_BADFLAGS) == 1);
	assert(gai_error_is_permanent(EAI_SOCKTYPE) == 1);
	assert(gai_error_is_permanent(EAI_FAMILY) == 1);
	assert(gai_error_is_permanent(EAI_SERVICE) == 1);

	fprintf(stderr, "PASS: listen/getaddrinfo error classification is correct\n");
}

/*
 * 2. End-to-end recovery: the port is unavailable when the daemon starts, and
 * becomes available later.  server() must survive and then take the port.
 */
static void test_recovers_when_port_becomes_free(void)
{
	int port;
	int squatter = -1;
	pid_t child;
	int status;
	int i;
	int connected = 0;

	for (port = FIRST_PORT; port <= LAST_PORT; port++)
	{
		squatter = squat_on_port(port);
		if (squatter >= 0)
			break;
	}
	if (squatter < 0)
	{
		fprintf(stderr, "FAIL: could not occupy any of ports %d..%d\n",
		        FIRST_PORT, LAST_PORT);
		exit(1);
	}

	child = spawn_server((int)('0' + (port - BASEPORT)), (char *)"127.0.0.1", squatter);
	g_child = child;

	/* Long enough for several retry attempts. */
	(void)sleep(3);

	if (child_exited(child, &status))
	{
		fprintf(stderr,
		        "FAIL: server() exited while the port was temporarily "
		        "unavailable (status=0x%x exit=%d); it must retry until the "
		        "network is ready\n",
		        status, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		(void)close(squatter);
		g_child = -1;
		exit(1);
	}

	/* Release the port: the daemon must now bring the listener up. */
	(void)close(squatter);
	for (i = 0; i < 60 && !connected; i++)
	{
		connected = (can_connect(port) == 1);
		if (!connected)
			(void)usleep(250000);
	}
	reap_child();

	if (!connected)
	{
		fprintf(stderr,
		        "FAIL: server() never acquired port %d after it became free\n",
		        port);
		exit(1);
	}
	fprintf(stderr,
	        "PASS: server() survived a temporarily unavailable port and bound "
	        "it once it was released (port %d)\n",
	        port);
}

/*
 * 3. A bind address that the resolver cannot turn into an address yet (or
 * ever, for this reserved name) must not kill the daemon either.
 */
static void test_unresolvable_bindaddr_retries(void)
{
	pid_t child;
	int status;

	/*
	 * RFC 2606 reserves ".invalid": a correctly configured resolver always
	 * fails on it, so this deterministically reproduces the "name service
	 * not ready / name not resolvable" startup ordering.
	 */
	child = spawn_server('0', (char *)"p910nd-test-no-such-host.invalid", -1);
	g_child = child;

	(void)sleep(3);

	if (child_exited(child, &status))
	{
		fprintf(stderr,
		        "FAIL: server() exited on an unresolvable bind address "
		        "(status=0x%x exit=%d); it must keep retrying\n",
		        status, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		g_child = -1;
		exit(1);
	}
	reap_child();
	fprintf(stderr, "PASS: server() keeps retrying an unresolvable bind address\n");
}

int main(void)
{
	(void)signal(SIGPIPE, SIG_IGN);
	/*
	 * Backstop.  Every stage is bounded by its own retry budget; this only
	 * guards against a pathological hang, and gives the daemon ample time to
	 * notice a released port (LISTEN_RETRY_SEC is 1 s).
	 */
	(void)alarm(120);

	test_error_classification();
	test_recovers_when_port_becomes_free();
	test_unresolvable_bindaddr_retries();

	reap_child();
	fprintf(stderr,
	        "PASS: server() retries the listening socket until the network is "
	        "ready\n");
	return 0;
}
