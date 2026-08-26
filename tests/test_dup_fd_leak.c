/*
 * Regression test for Bug #1: dup_fd_below_fdsetsize() leaked the original
 * file descriptor whenever it had to duplicate a descriptor that was at or
 * above FD_SETSIZE (select() cannot watch such a descriptor, so it is copied
 * to a low fd).  The old code returned the duplicate but never closed the
 * original, so that high-numbered descriptor stayed open for the whole
 * process lifetime -- a real descriptor leak (and, with enough connections,
 * an exhaustion of the descriptor table).
 *
 * Fix: dup_fd_below_fdsetsize() now closes the original fd right after a
 * successful dup2(), transferring ownership to the duplicate.  The caller
 * only manages the returned (in-range) descriptor.
 *
 * This test exercises the exact scenario by opening the "network" end on a
 * descriptor >= FD_SETSIZE, running a full bidirectional copy_stream(), and
 * proving the high fd is closed afterwards (fcntl(F_GETFD) == -1 / EBADF).
 * It also proves the job is still delivered intact, so the fix introduced no
 * regression in the data path.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <sys/resource.h>

/* Pick a descriptor comfortably above FD_SETSIZE so select() would be unsafe
 * with it and the daemon must duplicate it down. */
#ifndef TEST_HIGH_FD
#define TEST_HIGH_FD (FD_SETSIZE + 64)
#endif

static int open_high_fd(int low_fd)
{
	int hi;
	struct rlimit rl;

	/* Make sure we are allowed to hold a descriptor that high. */
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
	{
		if (rl.rlim_cur < (rlim_t)TEST_HIGH_FD + 1)
		{
			rlim_t want = (rlim_t)TEST_HIGH_FD + 1;
			rl.rlim_cur = want;
			if (rl.rlim_max < want)
				rl.rlim_max = want;
			(void)setrlimit(RLIMIT_NOFILE, &rl);
		}
	}
	hi = dup2(low_fd, TEST_HIGH_FD);
	assert(hi == TEST_HIGH_FD);
	return hi;
}

static void sendall(int fd, const char *buf, size_t len)
{
	size_t off = 0;
	while (off < len)
	{
		ssize_t n = write(fd, buf + off, len - off);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			perror("write");
			exit(1);
		}
		off += (size_t)n;
	}
}

/* waitpid helper (kept local to avoid clashing with other tests' names). */
static void assert_exited_helper(pid_t a, pid_t b)
{
	int st;
	assert(waitpid(a, &st, 0) == a);
	assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
	assert(waitpid(b, &st, 0) == b);
	assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

int main(void)
{
	int low_net[2], prn_sv[2], rep[2];
	int hi_net;                  /* network end living at >= FD_SETSIZE */
	pid_t client, prn;
	char data[65536];
	long received = -1;
	size_t i;
	int fd_after;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, low_net) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
	assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);
	assert(pipe(rep) == 0);

	/* Promote the daemon-side network fd to a high descriptor. */
	hi_net = open_high_fd(low_net[0]);
	(void)close(low_net[0]); /* original low fd is replaced by hi_net */

	for (i = 0; i < sizeof(data); i++)
		data[i] = (char)(i * 29 + 11);

	(void)alarm(30);

	client = fork();
	assert(client >= 0);
	if (client == 0)
	{
		close(low_net[0]); /* daemon side */
		close(hi_net);     /* client does not touch the daemon's high fd */
		close(prn_sv[0]);
		close(prn_sv[1]);
		close(rep[0]);
		close(rep[1]);
		sendall(low_net[1], data, sizeof(data));
		shutdown(low_net[1], SHUT_WR);
		_exit(0);
	}
	prn = fork();
	assert(prn >= 0);
	if (prn == 0)
	{
		char b;
		long got_count = 0;
		close(low_net[0]);
		close(low_net[1]);
		close(hi_net);
		close(prn_sv[0]);
		close(rep[0]);
		while (1)
		{
			ssize_t r = read(prn_sv[1], &b, 1);
			if (r < 0)
			{
				if (errno == EINTR)
					continue;
				perror("prn read");
				_exit(2);
			}
			if (r == 0)
				break;
			got_count += 1;
		}
		if (write(rep[1], &got_count, sizeof(got_count)) != (ssize_t)sizeof(got_count))
			_exit(4);
		_exit(0);
	}
	close(low_net[1]);
	close(prn_sv[1]);
	close(rep[1]);

	bidir = 1;
	log_to_stdout = 0;
	/* hi_net is >= FD_SETSIZE, forcing the dup-down path inside copy_stream. */
	assert(copy_stream(hi_net, prn_sv[0]) == 0);
	close(prn_sv[0]);

	/* THE FIX: the high network fd must now be closed (no leak). */
	fd_after = fcntl(hi_net, F_GETFD);
	if (fd_after != -1)
	{
		fprintf(stderr, "FAIL: high network fd %d still open after copy_stream (leak)\n",
			hi_net);
		close(hi_net);
		return 1;
	}
	assert(errno == EBADF);

	assert_exited_helper(client, prn);

	if (read(rep[0], &received, sizeof(received)) != (ssize_t)sizeof(received))
		received = -1;
	close(rep[0]);

	assert(received == (long)sizeof(data));

	fprintf(stderr, "PASS: high network fd %d closed (no leak), %ld bytes delivered intact\n",
		hi_net, received);
	return 0;
}
