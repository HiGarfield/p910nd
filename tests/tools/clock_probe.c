/*
 * Behaviour probe for mono_now()'s clock source.
 *
 * p910nd.c is included so mono_now() is reachable.  clock_gettime() succeeds
 * once (monotonic second 5) and fails afterwards; gettimeofday() reports a
 * realistic epoch value that advances by a few seconds between calls.  The
 * three samples must stay on ONE time base: the old implementation jumped
 * from 5 to ~1700000000, which would fire every deadline at once.
 *
 * Exit 0 means the fallback kept the monotonic base and kept advancing.
 */
#define main p910nd_main_unused
#include "p910nd.c"
#undef main

static int cg_calls;
static time_t wall_sec = 1700000000L;

int clock_gettime(clockid_t id, struct timespec *tp)
{
	(void)id;
	if (cg_calls++ == 0) {
		tp->tv_sec = 5;
		tp->tv_nsec = 0;
		return 0;
	}
	errno = EINVAL;
	return -1;
}

int gettimeofday(struct timeval *tv, void *tz)
{
	(void)tz;
	tv->tv_sec = wall_sec;
	tv->tv_usec = 0;
	return 0;
}

int main(void)
{
	struct timeval a, b, c;

	mono_now(&a);
	wall_sec += 3;
	mono_now(&b);
	wall_sec += 7;
	mono_now(&c);
	printf("first=%ld second=%ld third=%ld step1=%ld step2=%ld\n",
	       (long)a.tv_sec, (long)b.tv_sec, (long)c.tv_sec,
	       (long)(b.tv_sec - a.tv_sec), (long)(c.tv_sec - b.tv_sec));
	return (b.tv_sec - a.tv_sec == 3 && c.tv_sec - b.tv_sec == 7) ? 0 : 1;
}
