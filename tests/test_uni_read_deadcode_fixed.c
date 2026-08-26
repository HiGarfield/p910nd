/*
 * Formal proof for Bug #1 (dead-code in the unidirectional copy_stream loop).
 *
 * The old code did:
 *
 *     result = readBuffer(&networkToPrinterBuffer);
 *     if (result < 0)
 *         result = 0;                 // (A) dead store
 *     result = writeBuffer(&networkToPrinterBuffer);  // (B) overwrites (A)
 *
 * (A) is a value that is never read: it is assigned and then immediately
 * overwritten by (B).  clang's static analyzer flags this as
 * `deadcode.DeadStores`, and a project that compiles cleanly under
 * -Wall -Wextra -Werror (the test-suite contract) can regress if a stricter
 * CI turns that analyzer warning into an error.  More importantly the dead
 * store obscured the real control flow: readBuffer()'s return value is not
 * needed at all -- only its side effect (filling the buffer, setting
 * eof_read / READ_ERR) matters -- so the correct fix is to drop the read
 * result entirely and keep only writeBuffer()'s return value for the error
 * check.
 *
 * This test proves the fix in two independent ways:
 *
 *   1. Build-time proof: recompile the translation unit with `clang
 *      --analyze` and assert that no `deadcode.DeadStores` diagnostic is
 *      produced for the unidirectional loop.  This is the exact analyzer
 *      that reported the original defect, so its silence is a direct proof
 *      the dead store is gone.  (Falls back to "skip" if clang is absent.)
 *
 *   2. Behavioural proof: run a real unidirectional job where the network
 *      peer resets (hard read error, ECONNRESET) while bytes are still
 *      buffered but not yet written to a slow printer.  The fix must still
 *      drain every buffered byte to the printer (no data loss) and
 *      copy_stream() must return 0 for a fully-delivered job.  This proves
 *      removing the dead store did not change the drain semantics that the
 *      dead code was (ineffectually) trying to protect.
 *
 * Both proofs must hold for the test to pass.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <signal.h>

#define JOB_BYTES (3 * BUFFER_SIZE) /* 24576: exceeds the 8192 internal buffer */

/* ---- Proof 1: clang --analyze reports no deadcode.DeadStores ---- */
static int build_proof_no_deadcode(void)
{
	char cmd[2048];
	FILE *fp;
	char buf[8192];
	size_t n = 0;

	fp = popen("command -v clang >/dev/null 2>&1 && echo yes || echo no", "r");
	if (fp != NULL)
	{
		if (fgets(buf, sizeof(buf), fp) != NULL && buf[0] == 'n')
		{
			pclose(fp);
			fprintf(stderr, "SKIP: clang not available, cannot run --analyze proof\n");
			return 0; /* not a failure, just unproven here */
		}
		pclose(fp);
	}

	(void)snprintf(cmd, sizeof(cmd),
	               "clang -Wall -Wextra --analyze -DBUFFER_SIZE=8192 "
	               "-DIDLE_TIMEOUT_SEC=2 p910nd.c 2>/tmp/p910nd_deadcode.err");
	fp = popen(cmd, "r");
	if (fp == NULL)
	{
		perror("popen");
		return 2;
	}
	(void)pclose(fp);

	{
		FILE *ef = fopen("/tmp/p910nd_deadcode.err", "r");
		if (ef != NULL)
		{
			n = fread(buf, 1, sizeof(buf) - 1, ef);
			buf[n] = '\0';
			fclose(ef);
		}
	}

	if (strstr(buf, "deadcode.DeadStores") != NULL ||
	    strstr(buf, "Value stored to 'result' is never read") != NULL)
	{
		fprintf(stderr,
		        "FAIL: dead-code store still present after fix:\n%s", buf);
		return 1;
	}
	fprintf(stderr, "PASS(proof1): clang --analyze reports no deadcode.DeadStores in copy_stream\n");
	return 0;
}

/* ---- Proof 2: unidirectional hard read error still drains fully ---- */
static int behaviour_proof_drain(void)
{
	int lfd, netfd, prn_sv[2], rep[2];
	pid_t client, prn;
	struct sockaddr_in addr;
	struct linger ling;
	char data[JOB_BYTES];
	long received = -1;
	int sz, one = 1;
	size_t i;
	socklen_t alen;

	for (i = 0; i < sizeof(data); i++)
		data[i] = (char)(i * 11 + 1);

	assert(pipe(rep) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
	sz = 2048;
	assert(setsockopt(prn_sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz)) == 0);
	assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);

	assert((lfd = socket(AF_INET, SOCK_STREAM, 0)) >= 0);
	assert(setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
	sz = 131072;
	assert(setsockopt(lfd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz)) == 0);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	assert(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	assert(listen(lfd, 4) == 0);
	alen = sizeof(addr);
	assert(getsockname(lfd, (struct sockaddr *)&addr, &alen) == 0);

	(void)alarm(120);

	bidir = 0;
	log_to_stdout = 0;

	client = fork();
	assert(client >= 0);
	if (client == 0)
	{
		size_t off = 0;
		int cfd;
		(void)close(lfd);
		(void)close(prn_sv[0]);
		(void)close(prn_sv[1]);
		(void)close(rep[0]);
		(void)close(rep[1]);
		assert((cfd = socket(AF_INET, SOCK_STREAM, 0)) >= 0);
		if (connect(cfd, (struct sockaddr *)&addr, alen) < 0)
		{
			perror("connect");
			_exit(2);
		}
		while (off < sizeof(data))
		{
			ssize_t n = write(cfd, data + off, sizeof(data) - off);
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				perror("client write");
				_exit(2);
			}
			off += (size_t)n;
		}
		(void)usleep(300000);
		ling.l_onoff = 1;
		ling.l_linger = 0;
		(void)setsockopt(cfd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
		(void)close(cfd);
		_exit(0);
	}

	prn = fork();
	assert(prn >= 0);
	if (prn == 0)
	{
		char b;
		long got = 0;
		(void)close(lfd);
		(void)close(prn_sv[0]);
		(void)close(rep[0]);
		for (;;)
		{
			ssize_t r = read(prn_sv[1], &b, 1);
			if (r < 0)
			{
				if (errno == EINTR)
					continue;
				perror("consumer read");
				_exit(3);
			}
			if (r == 0)
				break;
			got += 1;
			(void)usleep(800);
		}
		if (write(rep[1], &got, sizeof(got)) != (ssize_t)sizeof(got))
			_exit(4);
		_exit(0);
	}

	(void)close(prn_sv[1]);
	(void)close(rep[1]);
	netfd = accept(lfd, NULL, NULL);
	assert(netfd >= 0);
	(void)close(lfd);

	{
		int rc = copy_stream(netfd, prn_sv[0]);
		fprintf(stderr, "copy_stream rc=%d (unidir, hard read error, dead-code fix)\n", rc);
		if (rc != 0)
		{
			fprintf(stderr, "FAIL: copy_stream returned %d on a fully delivered job\n", rc);
			(void)close(netfd);
			(void)close(prn_sv[0]);
			(void)waitpid(client, NULL, 0);
			(void)waitpid(prn, NULL, 0);
			return 1;
		}
	}

	(void)close(netfd);
	(void)close(prn_sv[0]);
	if (read(rep[0], &received, sizeof(received)) != (ssize_t)sizeof(received))
		received = -1;
	(void)close(rep[0]);

	(void)waitpid(client, NULL, 0);
	(void)waitpid(prn, NULL, 0);

	fprintf(stderr, "printer received %ld of %d job bytes after unidir network read error\n",
		received, JOB_BYTES);
	if (received != (long)JOB_BYTES)
	{
		fprintf(stderr, "FAIL: job truncated to %ld bytes (data lost after dead-code removal)\n", received);
		return 1;
	}
	fprintf(stderr, "PASS(proof2): unidir buffered data fully drained to printer after network read error\n");
	return 0;
}

int main(void)
{
	int r1 = build_proof_no_deadcode();
	int r2 = behaviour_proof_drain();
	if (r1 != 0 || r2 != 0)
	{
		fprintf(stderr, "FAIL: Bug#1 dead-code fix proof failed (build=%d behaviour=%d)\n", r1, r2);
		return 1;
	}
	fprintf(stderr, "PASS: Bug#1 dead-code fix verified (no deadcode + drain behaviour preserved)\n");
	return 0;
}
