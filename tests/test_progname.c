/*
 * Regression test for the fallback progname path in main().
 *
 * When argc <= 0 (defensive branch for exotic startup environments)
 * progname is set to a writable static array, and main() then rewrites
 * the "p910n" prefix of the program name in place (p[4] = digit).  If
 * progname pointed at a string literal this would be a write to
 * read-only memory, i.e. undefined behaviour (typically SIGSEGV).
 *
 * The test drives exactly that rewrite against the fallback buffer and
 * verifies it succeeds and produces the expected name.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>

int main(void)
{
	char *p;

	/* Emulate the argc <= 0 branch of main(). */
	progname = default_progname;
	assert(progname != NULL);

	/* Emulate the in-place "p910n" -> "p910<digit>" rewrite. */
	p = strstr(progname, "p910n");
	assert(p != NULL);
	p[4] = (char)'5';
	assert(strcmp(progname, "p9105d") == 0);

	/* The syslog ident and ps name now reflect the chosen printer. */
	assert(strstr(progname, "p9105d") != NULL);

	fprintf(stderr, "PASS: fallback progname buffer is writable, rewrite safe\n");
	return 0;
}
