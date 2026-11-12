/*
 * Behaviour probe for mono_now()'s "never return an undefined timeval"
 * contract (PD-67).
 *
 * clock_gettime()/gettimeofday() are replaced so their success or failure is
 * controlled by argv[1]:
 *
 *   both-fail  both clocks fail         -> a defined zero fallback, rc 0
 *   wall       monotonic fails, wall ok -> the wall value, rc 0
 *   mono       monotonic works          -> the monotonic value, rc 1
 *
 * The timeval is poisoned before the call: leaving that poison in place is
 * exactly the old bug (callers pass uninitialised stack structs).
 */
#define main p910nd_main_unused
#include "p910nd.c"
#undef main

static int fail_cg;
static int fail_gtod;

int clock_gettime(clockid_t id, struct timespec *tp)
{
	(void)id;
	if (fail_cg) {
		errno = EINVAL;
		return -1;
	}
	tp->tv_sec = 5;
	tp->tv_nsec = 0;
	return 0;
}

int gettimeofday(struct timeval *tv, void *tz)
{
	(void)tz;
	if (fail_gtod) {
		errno = EINVAL;
		return -1;
	}
	tv->tv_sec = 1700000000L;
	tv->tv_usec = 0;
	return 0;
}

int main(int argc, char **argv)
{
	struct timeval tv;
	int r;

	if (argc < 2)
		return 3;
	if (!strcmp(argv[1], "both-fail")) {
		fail_cg = 1;
		fail_gtod = 1;
	} else if (!strcmp(argv[1], "wall")) {
		fail_cg = 1;
	} else if (!strcmp(argv[1], "mono")) {
		fail_cg = 0;
	} else {
		return 3;
	}
	tv.tv_sec = 12345;
	tv.tv_usec = 678;
	r = mono_now(&tv);
	printf("mode=%s rc=%d sec=%ld usec=%ld\n", argv[1], r,
	       (long)tv.tv_sec, (long)tv.tv_usec);
	if (!strcmp(argv[1], "both-fail"))
		return (r == 0 && tv.tv_sec == 0 && tv.tv_usec == 0) ? 0 : 1;
	if (!strcmp(argv[1], "wall"))
		return (r == 0 && tv.tv_sec == 1700000000L) ? 0 : 1;
	return (r == 1 && tv.tv_sec == 5 && tv.tv_usec == 0) ? 0 : 1;
}
