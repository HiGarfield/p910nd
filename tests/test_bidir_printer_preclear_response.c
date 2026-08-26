/*
 * Regression test for Bug #1: in bidirectional mode, printer responses that
 * arrive BEFORE p910nd has written the first byte to the printer used to be
 * discarded (the old "need_clear_lp" mechanism zeroed the
 * printerToNetworkBuffer when no data had yet been sent to the printer).
 *
 * With arbitrary startup/ready ordering (a requirement: the printer, the
 * network, and p910nd itself may become ready in any order, with unbounded
 * gaps), many real devices handshake or emit status on connect *before* they
 * receive any job data.  Those early bytes must reach the network peer, not be
 * thrown away.
 *
 * This test makes the printer send a response burst on connect (before any job
 * data is received), then consume the whole job.  It asserts the network peer
 * receives the full early response AND the job is delivered byte-for-byte to
 * the printer.  Run repeatedly to defeat timing luck.
 *
 * Formal proof sketch (post-fix):
 *   - need_clear_lp mechanism removed; printerToNetworkBuffer is drained to the
 *     network whenever io_fd is writable (prepBuffer arms it once bytes>0).
 *   - The early response is read into printerToNetworkBuffer; prepBuffer arms
 *     writefds for io_fd; writeBuffer forwards every byte.
 *   - Therefore bytes sent before any printer-write are delivered, not cleared.
 * The test executes this scenario ITERS times and asserts the exact response
 * length and integrity, plus full job delivery, on every iteration.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>

#define JOB_BYTES 65536
#define EARLY_RESP "PRE-JOB-HANDSHAKE-STATUS-0xAZ"
#define ITERS 30

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

static size_t readall(int fd, char *buf, size_t cap)
{
	size_t off = 0;
	while (off < cap)
	{
		ssize_t n = read(fd, buf + off, cap - off);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			perror("read");
			exit(1);
		}
		if (n == 0)
			break;
		off += (size_t)n;
	}
	return off;
}

/* Run one job: printer emits EARLY_RESP before consuming the job; the network
 * peer must receive EARLY_RESP and deliver the whole job to the printer.
 * Returns copy_stream() rc; *printer_got receives the printer byte count,
 * *resp_ok receives 1 if the network peer got the exact early response. */
static int run_once(long *printer_got, int *resp_ok)
{
	int net_sv[2], prn_sv[2], rep[2];
	pid_t client, prn;
	char job[JOB_BYTES];
	size_t i;
	int rc;

	for (i = 0; i < sizeof(job); i++)
		job[i] = (char)(i * 11 + 5);

	assert(pipe(rep) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
	assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);

	(void)alarm(30);
	bidir = 1;
	log_to_stdout = 0;

	client = fork();
	assert(client >= 0);
	if (client == 0)
	{
		char resp[256];
		size_t got;
		(void)close(net_sv[0]);
		(void)close(prn_sv[0]);
		(void)close(prn_sv[1]);
		(void)close(rep[0]);
		(void)close(rep[1]);
		sendall(net_sv[1], job, sizeof(job));
		/* Close our write side so p910nd sees network EOF and can finish. */
		(void)shutdown(net_sv[1], SHUT_WR);
		got = readall(net_sv[1], resp, sizeof(resp));
		(void)close(net_sv[1]);
		_exit(got == strlen(EARLY_RESP) &&
		      memcmp(resp, EARLY_RESP, got) == 0 ? 0 : 3);
	}

	prn = fork();
	assert(prn >= 0);
	if (prn == 0)
	{
		char b;
		long got = 0;
		(void)close(net_sv[0]);
		(void)close(net_sv[1]);
		(void)close(prn_sv[0]);
		(void)close(rep[0]);
		/* Send the early handshake BEFORE any job data arrives. */
		sendall(prn_sv[1], EARLY_RESP, strlen(EARLY_RESP));
		/* Then consume the whole job. */
		while (read(prn_sv[1], &b, 1) == 1)
			got += 1;
		if (write(rep[1], &got, sizeof(got)) != (ssize_t)sizeof(got))
			_exit(4);
		_exit(0);
	}

	(void)close(net_sv[1]);
	(void)close(prn_sv[1]);
	(void)close(rep[1]);

	rc = copy_stream(net_sv[0], prn_sv[0]);
	(void)close(net_sv[0]);
	(void)close(prn_sv[0]);

	if (read(rep[0], printer_got, sizeof(*printer_got)) != (ssize_t)sizeof(*printer_got))
		*printer_got = -1;
	(void)close(rep[0]);
	*resp_ok = 0;
	{
		int status;
		assert(waitpid(client, &status, 0) == client);
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			*resp_ok = 1;
	}
	(void)waitpid(prn, NULL, 0);
	return rc;
}

int main(void)
{
	int i, bad_rc = 0, trunc = 0, lost_resp = 0;

	for (i = 0; i < ITERS; i++)
	{
		long pg = -1;
		int ok = 0;
		int rc = run_once(&pg, &ok);
		if (rc != 0)
			bad_rc++;
		if (pg != (long)JOB_BYTES)
			trunc++;
		if (!ok)
			lost_resp++;
	}
	fprintf(stderr,
	        "bidir pre-clear response: %d iters, copy_stream!=0: %d, truncated: %d, lost-response: %d\n",
	        ITERS, bad_rc, trunc, lost_resp);
	if (lost_resp != 0)
	{
		fprintf(stderr, "FAIL: early printer response dropped in %d/%d iterations\n",
		        lost_resp, ITERS);
		return 1;
	}
	if (trunc != 0)
	{
		fprintf(stderr, "FAIL: job truncated in %d/%d iterations (data lost)\n", trunc, ITERS);
		return 1;
	}
	if (bad_rc != 0)
	{
		fprintf(stderr, "FAIL: copy_stream returned -1 in %d/%d iterations\n", bad_rc, ITERS);
		return 1;
	}
	fprintf(stderr, "PASS: %d/%d jobs delivered intact and early printer response preserved\n",
	        ITERS, ITERS);
	return 0;
}
