/*
 * Behaviour probe for the first printer-open failure log line.
 *
 * p910nd.c is included so its static open_printer_retry() is reachable.  The
 * clock, sleep() and vsyslog() are overridden so the test needs no daemon, no
 * syslog service and no root:
 *
 *   - the first clock_gettime() reports second 5, i.e. a machine that booted
 *     less than OPEN_PRINTER_RETRY_LOG_EVERY seconds ago;
 *   - every later call jumps to second 1000, so OPEN_PRINTER_MAX_WAIT makes
 *     the retry loop finish after one retry;
 *   - vsyslog() records the first LOG_ERR/LOG_NOTICE line that dolog() emits.
 *     LOG_DEBUG lines are dropped by dolog(), which is exactly what the
 *     throttle defect does to the first failure.
 *
 * Exit 0 means the first failure was reported at a visible priority.
 */
#define main p910nd_main_unused
#include "p910nd.c"
#undef main

static int clock_calls;
static int log_calls;
static char first_log[256];

int clock_gettime(clockid_t id, struct timespec *ts)
{
	(void)id;
	clock_calls++;
	ts->tv_sec = (clock_calls == 1) ? 5 : 1000;
	ts->tv_nsec = 0;
	return 0;
}

unsigned int sleep(unsigned int seconds)
{
	(void)seconds;
	return 0;
}

void vsyslog(int priority, const char *fmt, va_list ap)
{
	(void)priority;
	if (log_calls == 0)
		(void)vsnprintf(first_log, sizeof(first_log), fmt, ap);
	log_calls++;
}

int main(void)
{
	struct timeval connected;
	struct timeval locked;

	connected.tv_sec = 5;
	connected.tv_usec = 0;
	locked.tv_sec = 5;
	locked.tv_usec = 0;
	device = "/definitely/not/a/printer";
	(void)open_printer_retry('0', &connected, &locked);
	printf("log_calls=%d first=%s", log_calls, first_log);
	return (strstr(first_log, "will keep retrying") != NULL) ? 0 : 1;
}
