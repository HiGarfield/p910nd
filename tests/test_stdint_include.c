/*
 * Regression test: p910nd.c must include the standard header that owns every
 * type it names, rather than relying on another header's internals.
 *
 * Bug
 * ---
 * get_port() is declared as
 *     static uint16_t get_port(const struct sockaddr *sa)
 * and uses a uint16_t local, but p910nd.c did not include <stdint.h>.  POSIX
 * places the exact-width integer types (uint16_t and friends) in <stdint.h>.
 * <netinet/in.h> is only required to define in_port_t / in_addr_t plus whatever
 * types its own structures need; it is explicitly NOT required to make
 * <stdint.h>'s namespace visible, and it may satisfy its internal needs through
 * private implementation headers.  glibc does exactly that -- it pulls in
 * <bits/stdint-uintn.h> -- so uint16_t happened to be visible and the file
 * compiled, purely by accident of that one C library's layout.
 *
 * Why this is a real defect and not a style nit: the project explicitly targets
 * more than one platform (see the porting note in the file header).  On a C
 * library whose <netinet/in.h> does not leak the uintN_t names, p910nd.c fails
 * to compile outright with "unknown type name 'uint16_t'".  A build that
 * depends on a transitive include is fragile in the other direction too: a
 * future C library or kernel-header reshuffle can remove the accidental
 * exposure at any time, breaking a previously working build for no visible
 * reason.
 *
 * Fix: include <stdint.h> directly.
 *
 * How this test proves the fix
 * ---------------------------
 * A runtime harness cannot observe a missing include -- the failure mode is a
 * compile error, and on glibc the pre-fix code compiles anyway, so simply
 * building p910nd.c proves nothing.  Instead this test performs the proof
 * *symbolically*, at build time, in two complementary directions:
 *
 *   Direction 1 (the include is present).  Scan p910nd.c and assert that it
 *   contains a top-level `#include <stdint.h>`.  This is the property that had
 *   been violated; it holds independently of which C library the test happens
 *   to run on, so the guard cannot be silently satisfied by a glibc accident
 *   the way a mere compile check would be.
 *
 *   Direction 2 (the include is necessary and sufficient).  Compile a small
 *   translation unit that uses uint16_t with ONLY <stdint.h> included, and
 *   assert it succeeds.  This confirms <stdint.h> genuinely provides the type,
 *   so Direction 1 is the correct property to require rather than an arbitrary
 *   textual rule.
 *
 * Together: p910nd.c names uint16_t (checked), it includes <stdint.h>
 * (Direction 1), and <stdint.h> is what defines uint16_t (Direction 2).
 * Therefore the type is in scope from a guaranteed source on every conforming
 * platform, which is exactly the property that was missing.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reads the whole file into a NUL-terminated heap buffer. */
static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long len;
	size_t got;

	if (f == NULL)
	{
		fprintf(stderr, "FAIL: cannot open %s\n", path);
		exit(1);
	}
	if (fseek(f, 0, SEEK_END) != 0)
	{
		fprintf(stderr, "FAIL: fseek(%s)\n", path);
		exit(1);
	}
	len = ftell(f);
	if (len < 0)
	{
		fprintf(stderr, "FAIL: ftell(%s)\n", path);
		exit(1);
	}
	rewind(f);
	buf = malloc((size_t)len + 1);
	assert(buf != NULL);
	got = fread(buf, 1, (size_t)len, f);
	buf[got] = '\0';
	(void)fclose(f);
	return buf;
}

/*
 * Returns 1 if the source contains an active (non-commented) line whose only
 * content is the given #include directive.  Kept deliberately simple: it
 * matches at line granularity and ignores leading whitespace, which is enough
 * for a directive that must sit at file scope.
 */
static int has_include(const char *src, const char *header)
{
	char needle[128];
	const char *p;

	(void)snprintf(needle, sizeof(needle), "#include <%s>", header);
	for (p = src; (p = strstr(p, needle)) != NULL; p += strlen(needle))
	{
		const char *bol = p;
		int commented = 0;
		/* Walk back to the start of the line. */
		while (bol > src && bol[-1] != '\n')
			--bol;
		/* Reject a directive that is inside a // comment on the same line. */
		{
			const char *q;
			for (q = bol; q < p - 1; ++q)
			{
				if (q[0] == '/' && q[1] == '/')
				{
					commented = 1;
					break;
				}
			}
		}
		/* Only whitespace may precede the directive on its line. */
		{
			const char *q;
			for (q = bol; q < p; ++q)
			{
				if (*q != ' ' && *q != '\t')
				{
					commented = 1;
					break;
				}
			}
		}
		if (!commented)
			return 1;
	}
	return 0;
}

int main(void)
{
	char *src = slurp("p910nd.c");
	int uses_uint16, includes_stdint, probe_rc;

	/* Sanity: the type really is used, so the requirement is not vacuous. */
	uses_uint16 = (strstr(src, "uint16_t") != NULL);
	includes_stdint = has_include(src, "stdint.h");

	fprintf(stderr, "stdint include check: uses uint16_t=%d, includes <stdint.h>=%d\n",
	        uses_uint16, includes_stdint);

	if (!uses_uint16)
	{
		/* If a refactor removed every uint16_t the requirement is moot, but say
		 * so loudly instead of passing silently on a changed premise. */
		fprintf(stderr,
		        "NOTE: p910nd.c no longer uses uint16_t; the <stdint.h> requirement is vacuous\n");
		free(src);
		return 0;
	}

	/* Direction 1: the guaranteed provider of the type must be included. */
	if (!includes_stdint)
	{
		fprintf(stderr,
		        "FAIL: p910nd.c uses uint16_t but does not include <stdint.h>; it relies on\n"
		        "      <netinet/in.h> leaking the type, which POSIX does not guarantee and\n"
		        "      which breaks the build on a C library that does not do so.\n");
		free(src);
		return 1;
	}

	/* Direction 2: confirm <stdint.h> is indeed what supplies uint16_t, so the
	 * property asserted above is the right one. */
	{
		FILE *probe = fopen("/tmp/p910nd_stdint_probe.c", "w");
		assert(probe != NULL);
		(void)fputs("#include <stdint.h>\n"
		            "uint16_t v;\n"
		            "int main(void){ v = 1; return (int)v - 1; }\n",
		            probe);
		(void)fclose(probe);
		probe_rc = system("${CC:-cc} -std=c89 -pedantic -Wall -Wextra "
		                  "-c /tmp/p910nd_stdint_probe.c -o /tmp/p910nd_stdint_probe.o "
		                  "2>/tmp/p910nd_stdint_probe.log");
		(void)remove("/tmp/p910nd_stdint_probe.c");
		(void)remove("/tmp/p910nd_stdint_probe.o");
	}
	if (probe_rc != 0)
	{
		fprintf(stderr,
		        "FAIL: <stdint.h> alone did not provide uint16_t (probe exit %d); see "
		        "/tmp/p910nd_stdint_probe.log\n",
		        probe_rc);
		free(src);
		return 1;
	}

	free(src);
	fprintf(stderr,
	        "PASS: p910nd.c includes <stdint.h>, the standard header that defines the\n"
	        "      uint16_t it uses, so the type no longer depends on <netinet/in.h>\n"
	        "      internals and the file compiles on any conforming C library\n");
	return 0;
}
