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
 *
 * With "throttle <file>" the clock advances one second per call and stdout
 * is redirected to <file>, so the whole retry loop runs with -d semantics.
 * Only the LOG_ERR warning may say "will keep retrying"; a throttled attempt
 * must say "log throttled" instead, otherwise the throttle is invisible in
 * the debug log and its line count grows with every retry.
 */
#define main p910nd_main_unused
#include "p910nd.c"
#undef main

static int throttle_mode;
static int clock_calls;
static int log_calls;
static char first_log[256];

int clock_gettime(clockid_t id, struct timespec *ts)
{
	(void)id;
	clock_calls++;
	if (throttle_mode)
		ts->tv_sec = 5 + clock_calls;
	else
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

static int count_needle(const char *path, const char *needle)
{
	char buf[512];
	FILE *fp;
	int n = 0;

	fp = fopen(path, "r");
	if (fp == NULL)
		return -1;
	while (fgets(buf, sizeof(buf), fp) != NULL)
		if (strstr(buf, needle) != NULL)
			n++;
	(void)fclose(fp);
	return n;
}

int main(int argc, char **argv)
{
	struct timeval connected;
	struct timeval locked;
	int warned;
	int throttled;

	connected.tv_sec = 5;
	connected.tv_usec = 0;
	locked.tv_sec = 5;
	locked.tv_usec = 0;
	device = "/definitely/not/a/printer";

	if (argc > 2 && strcmp(argv[1], "throttle") == 0) {
		throttle_mode = 1;
		if (freopen(argv[2], "w", stdout) == NULL)
			return 2;
		log_to_stdout = 1;
		(void)open_printer_retry('0', &connected, &locked);
		(void)fflush(stdout);
		warned = count_needle(argv[2], "will keep retrying");
		throttled = count_needle(argv[2], "log throttled");
		fprintf(stderr, "warned_lines=%d throttled_debug_lines=%d\n",
		        warned, throttled);
		return (warned >= 1 && warned <= 2 && throttled >= 5) ? 0 : 1;
	}
	(void)open_printer_retry('0', &connected, &locked);
	printf("log_calls=%d first=%s", log_calls, first_log);
	return (strstr(first_log, "will keep retrying") != NULL) ? 0 : 1;
}
