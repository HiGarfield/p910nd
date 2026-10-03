/*
 * LD_PRELOAD shim that makes the wall clock controllable.
 *
 * p910nd derives every one of its timeouts from gettimeofday()/time(), i.e.
 * from CLOCK_REALTIME.  When the system clock is stepped backwards (NTP
 * correction, an administrator, a VM resuming from suspend) every "now - start"
 * difference becomes negative and the timeout never fires again -- the job and
 * the daemon hang.  That failure mode cannot be produced reliably by waiting,
 * so the cases preload this shim and drive the clock themselves.
 *
 * The shim is inert unless P910ND_FAKE_CLOCK names a control file, so a normal
 * run is unaffected.
 *
 * Control file format (re-read on every call, so it can change mid-job):
 *
 *   skew=<seconds>          offset added to the real clock  (default 0)
 *   abs=<sec.usec>          absolute value to report instead, overrides skew
 *   abs_step_us=<n>         when set together with abs, every call advances the
 *                           absolute clock by n microseconds
 *
 * Example: echo 'skew=-3600' > /tmp/clock        # jump one hour into the past
 *          echo 'abs=1700000000.900000' > /tmp/clock
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>

static int (*real_gettimeofday)(struct timeval *, void *);
static time_t (*real_time)(time_t *);

static const char *clock_path(void)
{
	const char *p = getenv("P910ND_FAKE_CLOCK");

	return (p != NULL && *p != '\0') ? p : NULL;
}

static void read_control(double *skew, int *have_abs,
			 double *abs_now, double *abs_step)
{
	const char *path = clock_path();
	FILE *fp;
	char line[256];

	*skew = 0.0;
	*have_abs = 0;
	*abs_now = 0.0;
	*abs_step = 0.0;
	if (path == NULL)
		return;
	fp = fopen(path, "r");
	if (fp == NULL)
		return;
	while (fgets(line, sizeof(line), fp) != NULL) {
		if (strncmp(line, "skew=", 5) == 0)
			*skew = atof(line + 5);
		else if (strncmp(line, "abs=", 4) == 0) {
			*abs_now = atof(line + 4);
			*have_abs = 1;
		} else if (strncmp(line, "abs_step_us=", 12) == 0)
			*abs_step = atof(line + 12);
	}
	fclose(fp);
}

/* Absolute-clock mode keeps its own cursor so successive calls advance. */
static double abs_cursor = -1.0;

int gettimeofday(struct timeval *tv, void *tz)
{
	double skew, abs_now, abs_step;
	int have_abs;

	if (real_gettimeofday == NULL)
		real_gettimeofday = dlsym(RTLD_NEXT, "gettimeofday");
	read_control(&skew, &have_abs, &abs_now, &abs_step);
	if (have_abs) {
		if (abs_cursor < 0.0)
			abs_cursor = abs_now;
		abs_cursor += abs_step / 1000000.0;
		tv->tv_sec = (time_t)abs_cursor;
		tv->tv_usec = (suseconds_t)((abs_cursor - (double)tv->tv_sec) * 1000000.0);
		return 0;
	}
	if (real_gettimeofday(tv, tz) != 0)
		return -1;
	tv->tv_sec += (time_t)skew;
	tv->tv_usec += (suseconds_t)((skew - (double)(long)skew) * 1000000.0);
	if (tv->tv_usec >= 1000000) {
		tv->tv_sec++;
		tv->tv_usec -= 1000000;
	} else if (tv->tv_usec < 0) {
		tv->tv_sec--;
		tv->tv_usec += 1000000;
	}
	return 0;
}

time_t time(time_t *t)
{
	struct timeval tv;

	if (real_time == NULL)
		real_time = dlsym(RTLD_NEXT, "time");
	if (gettimeofday(&tv, NULL) != 0) {
		if (real_time == NULL)
			return (time_t)-1;
		return real_time(t);
	}
	if (t != NULL)
		*t = tv.tv_sec;
	return tv.tv_sec;
}
