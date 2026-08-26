/*
 * Regression test for the -DTESTING build warnings in server().
 *
 * Bug: server() declared four locals -- `resourcelimit`, `max_close_fd`,
 * `pidfilename`, `f` -- at function scope, but every one of them is only
 * referenced inside the `#ifndef TESTING` daemonization block.  Under a
 * -DTESTING build (the configuration used by the entire test suite) those
 * declarations are dead, so compiling with the project's mandatory
 * -Wall -Wextra emitted -Wunused-variable warnings for all four, and a
 * -Werror build of the test suite failed outright.
 *
 * Fix: move the four declarations inside the `#ifndef TESTING` block so they
 * exist only where they are used.  Both the production and the TESTING build
 * then compile cleanly under -Wall -Wextra, and a -Werror build succeeds.
 *
 * This test is a *build-time* proof rather than a runtime one: it invokes the
 * compiler on p910nd.c (the same translation unit the daemon and every
 * other test includes) with -DTESTING -Wall -Wextra -Werror and asserts the
 * command succeeds and produces no diagnostics.  That provably exercises the
 * exact code path that used to warn, on every platform the compiler targets,
 * with no special runtime harness needed.  It is run from the repository root
 * so the spawned compiler resolves the (relative) p910nd.c in the cwd.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
	char cmd[2048];
	int rc;
	FILE *fp;
	char buf[4096];
	size_t n;

	/*
	 * Reproduce the test-suite build of p910nd.c (the translation unit that
	 * contains server()) and require it to be warning- and error-free under
	 * -Werror.  We capture stderr to make the failure self-explaining.
	 */
	(void)snprintf(cmd, sizeof(cmd),
	               "gcc -Wall -Wextra -Werror -DTESTING -c -o /tmp/p910nd_server_test.o "
	               "p910nd.c 2>/tmp/p910nd_server_test.err");

	fp = popen(cmd, "r");
	if (fp == NULL)
	{
		perror("popen");
		return 2;
	}
	rc = pclose(fp);

	/* Read any compiler diagnostics so we can print them on failure. */
	{
		FILE *ef = fopen("/tmp/p910nd_server_test.err", "r");
		n = 0;
		if (ef != NULL)
		{
			n = fread(buf, 1, sizeof(buf) - 1, ef);
			buf[n] = '\0';
			fclose(ef);
		}
	}

	if (rc != 0)
	{
		fprintf(stderr,
		        "FAIL: -DTESTING build of server() still warns/errors (rc=%d)\n"
		        "compiler output:\n%s",
		        rc, buf);
		return 1;
	}
	if (n != 0)
	{
		/* -Werror should make any warning a non-zero rc, but be defensive:
		 * if diagnostics were produced yet rc==0, that itself is unexpected. */
		fprintf(stderr, "FAIL: unexpected compiler diagnostics under -Werror:\n%s", buf);
		return 1;
	}

	fprintf(stderr, "PASS: -DTESTING build of server() is warning- and error-free (-Werror)\n");
	return 0;
}
