/*
 * LD_PRELOAD shim that records the order of openlog() and vsyslog() calls.
 *
 * Used by PD-71: under (x)inetd, an invalid option or -v makes usage() /
 * show_version() log through dolog() -> vsyslog().  If openlog() has not run
 * yet, those lines use the default ident/facility and lose LOG_PID.
 *
 * glibc's fortified headers make the compiler emit __vsyslog_chk() rather
 * than vsyslog(), so both symbols have to be intercepted.  The real syslog
 * functions are deliberately not called: the probe must work on hosts with
 * no /dev/log.  Set P910ND_SYSLOG_LOG to the file that receives the trace.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>

static void record(const char *what, int a, int b)
{
	const char *path = getenv("P910ND_SYSLOG_LOG");
	FILE *fp;

	if (path == NULL || *path == '\0')
		return;
	fp = fopen(path, "a");
	if (fp == NULL)
		return;
	fprintf(fp, "%s %d %d\n", what, a, b);
	fclose(fp);
}

void openlog(const char *ident, int option, int facility)
{
	(void)ident;
	record("openlog", (facility == LOG_LPR) ? 1 : 0,
	       (option & LOG_PID) ? 1 : 0);
}

void vsyslog(int priority, const char *fmt, va_list ap)
{
	(void)fmt;
	(void)ap;
	record("vsyslog", priority, 0);
}

void __vsyslog_chk(int priority, int flag, const char *fmt, va_list ap)
{
	(void)flag;
	(void)fmt;
	(void)ap;
	record("vsyslog", priority, 0);
}
