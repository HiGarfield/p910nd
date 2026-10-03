/*
 * Check the printer read pacing arithmetic from copy_stream().
 *
 * The pacing code normalises a target timestamp like this:
 *
 *     gettimeofday(&then, 0);
 *     then.tv_usec += PRINTER_READ_PACE_US;
 *     if (then.tv_usec >= 1000000) {          <-- the shipped comparison
 *             then.tv_usec -= 1000000;
 *             then.tv_sec++;
 *     }
 *
 * With the previous strict `>` a tv_usec of exactly 900000 plus a 100000us pace
 * summed to 1000000, the carry did not happen, and tv_usec was left out of
 * range -- a legal timeval is 0..999999.  The release then came one clock tick
 * early.
 *
 * For every alignment this probe
 *
 *   1. runs the shipped normalisation and flags any tv_usec left outside
 *      0..999999, and
 *   2. compares the release instant that produces with the one the caller
 *      means, computed independently as "start + pace" in absolute
 *      microseconds.
 *
 * Both must come out clean.  This is a test of the arithmetic that ships, not a
 * model of the old bug.
 */

#include <stdio.h>

#define PACE_US 100000L
#define TICK_US 1000L		/* the probe's clock granularity */
#define BASE_SEC 1000000000L

struct stamp {
	long sec;
	long usec;
};

static int expired(struct stamp then, struct stamp now)
{
	/* the release condition from copy_stream(): now > then, field-wise */
	if (now.sec > then.sec)
		return (1);
	if (now.sec == then.sec && now.usec > then.usec)
		return (1);
	return (0);
}

/* The shipped normalisation: add the pace to tv_usec and carry if needed. */
static struct stamp shipped_then(long start_sec, long start_usec)
{
	struct stamp then;

	then.sec = start_sec;
	then.usec = start_usec + PACE_US;
	if (then.usec >= 1000000) {
		then.usec -= 1000000;
		then.sec++;
	}
	return then;
}

/* The target the caller means, computed without any normalisation step. */
static struct stamp ideal_then(long start_sec, long start_usec)
{
	struct stamp then;
	long target = start_sec * 1000000L + start_usec + PACE_US;

	then.sec = target / 1000000L;
	then.usec = target % 1000000L;
	return then;
}

/* Ticks after the read at which the timer is released, 0 if never. */
static long ticks_to_release(struct stamp then, long start_sec, long start_usec)
{
	long t;
	long now = start_sec * 1000000L + start_usec;

	for (t = 0; t < 100000; t++) {
		struct stamp cur;

		now += TICK_US;
		cur.sec = now / 1000000L;
		cur.usec = now % 1000000L;
		if (expired(then, cur))
			return (t + 1);
	}
	return (0);
}

int main(void)
{
	long align;
	int bad_range = 0;
	int early = 0;
	int late = 0;

	printf("pacing release vs the ideal start+%ldus, in %ldus ticks\n\n",
	       PACE_US, TICK_US);
	printf("  %-10s %-12s %-10s %-8s %s\n",
	       "tv_usec", "normalised", "released", "ideal", "verdict");
	for (align = 0; align < 1000000; align += 100000) {
		struct stamp then = shipped_then(BASE_SEC, align);
		struct stamp ideal = ideal_then(BASE_SEC, align);
		long released = ticks_to_release(then, BASE_SEC, align);
		long want = ticks_to_release(ideal, BASE_SEC, align);
		const char *verdict = "ok";

		if (then.usec < 0 || then.usec > 999999) {
			bad_range++;
			verdict = "ILLEGAL tv_usec";
		} else if (released < want) {
			early++;
			verdict = "EARLY";
		} else if (released > want) {
			late++;
			verdict = "LATE";
		}
		printf("  %-10ld %-12ld %-10ld %-8ld %s\n", align, then.usec,
		       released, want, verdict);
	}

	printf("\nsummary: out-of-range=%d early=%d late=%d\n",
	       bad_range, early, late);
	if (bad_range == 0 && early == 0 && late == 0) {
		printf("VERDICT clean\n");
		return 0;
	}
	printf("VERDICT dirty\n");
	return 1;
}
