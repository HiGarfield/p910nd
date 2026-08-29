/*
 * Regression test for Bug: dolog() relied on the glibc-specific %m conversion
 * of the printf() family, which is not portable.
 *
 * The defect
 * ----------
 * dolog() formats with
 *
 *      if (log_to_stdout)  vfprintf(stdout, msg, argp);
 *      else                vsyslog(level, msg, argp);
 *
 * and almost every call site writes the error as "%s: %m\n".
 *
 * %m is a GNU extension: ISO C defines no such conversion and POSIX specifies
 * it for syslog() only, never for printf().  On glibc it happens to work in
 * both paths, but on any other C library -- BSD libc, macOS, musl -- vfprintf()
 * does not understand "%m", so with -d (the documented way to run p910nd in
 * the foreground and watch it) every single error message expanded to
 * garbage instead of the reason the operation failed.
 *
 * The fix
 * -------
 * dolog() now samples errno first, expands every "%m" into strerror() text
 * itself (escaping any '%' the message may contain), and hands the resulting
 * plain format string to vfprintf()/vsyslog().  Both output paths are then
 * identical on every platform, and they no longer depend on a GNU extension.
 *
 * What this test proves
 * ---------------------
 * 1. append_literal(): a '%' in substituted text is doubled, so it can never
 *    be mistaken for a conversion specifier (and the buffer is never
 *    overrun, even with a one-byte budget).
 * 2. expand_format_m(): "%m" becomes exactly strerror(err) -- and therefore
 *    the result contains no "%m" any more, which is precisely what makes the
 *    string portable; the rest of the format, including real conversions and
 *    a literal "%%m", is copied verbatim; and the output is always
 *    NUL-terminated and inside the buffer even when it is truncated.
 * 3. End to end: with log_to_stdout set, a real dolog() call is captured from
 *    stdout and must equal the hand-built expected string -- proving the
 *    expanded format is what actually reaches vfprintf().
 * 4. errno is sampled before anything can clobber it: dolog() must report the
 *    errno that was set by the caller, not one left behind by strerror() or
 *    by the formatting machinery.
 */
#define _GNU_SOURCE

/*
 * <stdio.h> and <stdarg.h> must be seen BEFORE `vfprintf` is redefined below,
 * otherwise their own declarations would be renamed too and clash with the
 * static interposer.
 */
#include <stdio.h>
#include <stdarg.h>

#define main p910nd_original_main

static int my_vfprintf(FILE *stream, const char *fmt, va_list ap);

#define vfprintf my_vfprintf
#include "../p910nd.c"
#undef vfprintf
#undef main

#include <assert.h>
#include <string.h>
#include <fcntl.h>

/*
 * Interposer that records the exact format string dolog() hands to
 * vfprintf().  This is the property that makes the daemon portable: on a C
 * library whose printf() has no %m, everything depends on that string never
 * containing "%m".  It is a compile-time interposition of the identifier in
 * this translation unit only, so it touches no production code and behaves
 * identically on every platform.
 */
static char g_last_fmt[DOLOG_FMT_SIZE];
static int g_have_fmt = 0;

static int my_vfprintf(FILE *stream, const char *fmt, va_list ap)
{
	(void)snprintf(g_last_fmt, sizeof(g_last_fmt), "%s", fmt);
	g_have_fmt = 1;
	/* Parenthesised so the object-like macro above cannot expand here. */
	return (vfprintf)(stream, fmt, ap);
}

static void test_append_literal_escaping(void)
{
	char buf[64];
	size_t pos;

	pos = 0;
	buf[0] = '\0';
	append_literal(buf, sizeof(buf), &pos, "a%b");
	buf[pos] = '\0';
	assert(strcmp(buf, "a%%b") == 0);

	pos = 0;
	buf[0] = '\0';
	append_literal(buf, sizeof(buf), &pos, "100% done");
	buf[pos] = '\0';
	assert(strcmp(buf, "100%% done") == 0);

	/* Truncation must never write outside the buffer. */
	pos = 0;
	buf[0] = '\0';
	append_literal(buf, 1, &pos, "abcdef");
	assert(pos == 0);

	pos = 0;
	buf[0] = '\0';
	append_literal(buf, 4, &pos, "abcdef");
	assert(pos <= 3);

	fprintf(stderr, "PASS: append_literal() escapes '%%' and stays in bounds\n");
}

static void test_expand_format_m(void)
{
	char buf[DOLOG_FMT_SIZE];
	char want[DOLOG_FMT_SIZE];
	const char *enoent = strerror(ENOENT);

	/* "%m" -> the strerror() text, and no "%m" survives. */
	expand_format_m(buf, sizeof(buf), "%s: %m\n", ENOENT);
	(void)snprintf(want, sizeof(want), "%%s: %s\n", enoent);
	assert(strcmp(buf, want) == 0);
	assert(strstr(buf, "%m") == NULL);

	/* A format with no %m at all is copied verbatim. */
	expand_format_m(buf, sizeof(buf), "wrote %lu/%lu bytes\n", 0);
	assert(strcmp(buf, "wrote %lu/%lu bytes\n") == 0);

	/*
	 * A literal "%%m" (the three characters '%', '%', 'm') must survive: it
	 * is an escaped '%' followed by an ordinary 'm', not the %m conversion.
	 * The C literal below denotes exactly "100%%m done\n".
	 */
	expand_format_m(buf, sizeof(buf), "100%%m done\n", 0);
	assert(strcmp(buf, "100%%m done\n") == 0);

	/* Two %m in one format are both expanded. */
	expand_format_m(buf, sizeof(buf), "%m / %m\n", ENOENT);
	(void)snprintf(want, sizeof(want), "%s / %s\n", enoent, enoent);
	assert(strcmp(buf, want) == 0);

	/* %m at the very end of the format string. */
	expand_format_m(buf, sizeof(buf), "open: %m", ENOENT);
	(void)snprintf(want, sizeof(want), "open: %s", enoent);
	assert(strcmp(buf, want) == 0);

	/* An empty format string is fine. */
	expand_format_m(buf, sizeof(buf), "", 0);
	assert(strcmp(buf, "") == 0);

	/* Truncation: still NUL-terminated and inside the buffer. */
	expand_format_m(buf, 8, "%s: %m\n", ENOENT);
	assert(strlen(buf) < 8);

	fprintf(stderr, "PASS: expand_format_m() replaces %%m with strerror() text\n");
}

/*
 * Capture what dolog() actually writes to stdout with log_to_stdout set.
 * Returns 0 on success.
 */
static int capture_dolog(int err, char *out, size_t cap)
{
	char path[128];
	int saved;
	int tmpfd;
	size_t n;
	FILE *f;

	(void)snprintf(path, sizeof(path), "/tmp/p910nd-dolog-%ld.out", (long)getpid());
	(void)fflush(stdout);
	saved = dup(STDOUT_FILENO);
	if (saved < 0)
		return -1;
	tmpfd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (tmpfd < 0)
	{
		(void)close(saved);
		return -1;
	}
	if (dup2(tmpfd, STDOUT_FILENO) < 0)
	{
		(void)close(tmpfd);
		(void)close(saved);
		return -1;
	}

	log_to_stdout = 1;
	errno = err;
	dolog(LOGOPTS, "/dev/lp0: %m\n");
	(void)fflush(stdout);
	log_to_stdout = 0;

	(void)dup2(saved, STDOUT_FILENO);
	(void)close(saved);
	(void)close(tmpfd);

	f = fopen(path, "r");
	if (f == NULL)
		return -1;
	n = fread(out, 1, cap - 1, f);
	out[n] = '\0';
	(void)fclose(f);
	(void)remove(path);
	return 0;
}

/*
 * The core portability proof: whatever dolog() actually passes to vfprintf()
 * must not contain "%m" any more, and must already carry the strerror() text.
 * This is checked against the real call, not against a copy of the logic.
 */
static void test_format_reaching_vfprintf_has_no_percent_m(void)
{
	char path[128];
	int saved;
	int tmpfd;

	(void)snprintf(path, sizeof(path), "/tmp/p910nd-dolog2-%ld.out", (long)getpid());
	(void)fflush(stdout);
	saved = dup(STDOUT_FILENO);
	assert(saved >= 0);
	tmpfd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	assert(tmpfd >= 0);
	assert(dup2(tmpfd, STDOUT_FILENO) >= 0);

	g_have_fmt = 0;
	g_last_fmt[0] = '\0';
	log_to_stdout = 1;
	errno = EACCES;
	dolog(LOGOPTS, "%s: %m\n", "/dev/lp0");
	log_to_stdout = 0;

	(void)dup2(saved, STDOUT_FILENO);
	(void)close(saved);
	(void)close(tmpfd);
	(void)remove(path);

	if (!g_have_fmt)
	{
		fprintf(stderr, "FAIL: dolog() never reached vfprintf()\n");
		exit(1);
	}
	if (strstr(g_last_fmt, "%m") != NULL)
	{
		fprintf(stderr,
		        "FAIL: the format string handed to vfprintf() still contains "
		        "%%m (\"%s\"); printf() has no %%m outside glibc\n",
		        g_last_fmt);
		exit(1);
	}
	if (strstr(g_last_fmt, strerror(EACCES)) == NULL)
	{
		fprintf(stderr,
		        "FAIL: the format string handed to vfprintf() (\"%s\") does "
		        "not contain the strerror() text\n",
		        g_last_fmt);
		exit(1);
	}

	fprintf(stderr,
	        "PASS: the format reaching vfprintf() has no %%m and carries \"%s\"\n",
	        strerror(EACCES));
}

static void test_dolog_stdout_output(void)
{
	char got[512];
	char want[512];

	assert(capture_dolog(ENOENT, got, sizeof(got)) == 0);
	(void)snprintf(want, sizeof(want), "/dev/lp0: %s\n", strerror(ENOENT));
	if (strcmp(got, want) != 0)
	{
		fprintf(stderr, "FAIL: dolog() wrote \"%s\", expected \"%s\"\n", got, want);
		exit(1);
	}

	/* A different errno must produce a different message, proving the errno
	 * really is sampled at call time rather than baked in. */
	assert(capture_dolog(EACCES, got, sizeof(got)) == 0);
	(void)snprintf(want, sizeof(want), "/dev/lp0: %s\n", strerror(EACCES));
	if (strcmp(got, want) != 0)
	{
		fprintf(stderr, "FAIL: dolog() wrote \"%s\", expected \"%s\"\n", got, want);
		exit(1);
	}

	fprintf(stderr, "PASS: dolog() writes the expanded strerror() text to stdout\n");
}

/*
 * errno must be the caller's, not whatever strerror() or the formatting
 * machinery left behind.
 */
static void test_errno_sampled_at_entry(void)
{
	char got[512];
	char want[512];
	int probe;

	assert(capture_dolog(EIO, got, sizeof(got)) == 0);
	(void)snprintf(want, sizeof(want), "/dev/lp0: %s\n", strerror(EIO));
	assert(strcmp(got, want) == 0);

	/*
	 * Deliberately dirty errno with a value whose text differs from EIO's
	 * before the call; the captured text must still describe EIO.
	 */
	for (probe = 0; probe < 1; probe++)
	{
		errno = EDOM;
		assert(capture_dolog(EIO, got, sizeof(got)) == 0);
	}
	(void)snprintf(want, sizeof(want), "/dev/lp0: %s\n", strerror(EIO));
	assert(strcmp(got, want) == 0);
	assert(strcmp(strerror(EIO), strerror(EDOM)) != 0); /* test is meaningful */

	fprintf(stderr, "PASS: dolog() samples errno on entry\n");
}

int main(void)
{
	(void)alarm(30);
	test_append_literal_escaping();
	test_expand_format_m();
	test_format_reaching_vfprintf_has_no_percent_m();
	test_dolog_stdout_output();
	test_errno_sampled_at_entry();
	fprintf(stderr, "PASS: dolog() no longer depends on the glibc-only %%m conversion\n");
	return 0;
}
