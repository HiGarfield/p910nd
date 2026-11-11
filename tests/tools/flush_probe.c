/*
 * Behaviour probe for flush_buffer()'s undelivered-byte result.
 *
 * p910nd.c is included so the static function is reachable.  A sink buffer
 * (outfd < 0) consumes its ring without delivering anything; those bytes
 * must be reported as undelivered, otherwise copy_stream() cannot tell the
 * operator that a printer reply was lost when the client vanished.
 *
 * Exit 0 means discarded bytes are reported and a normal flush still
 * reports success.
 */
#define main p910nd_main_unused
#include "p910nd.c"
#undef main

static void seed(Buffer_t *b, int outfd)
{
	initBuffer(b, -1, outfd, 0);
	memset(b->buffer, 'Z', 7);
	b->endidx = 7;
	b->bytes = 7;
	b->totalin = 7;
}

int main(void)
{
	Buffer_t b;
	int sink_left;
	int delivered_left;
	int devnull;

	seed(&b, -1);
	sink_left = flush_buffer(&b, 1);

	devnull = open("/dev/null", O_WRONLY);
	if (devnull < 0)
		return 2;
	seed(&b, devnull);
	delivered_left = flush_buffer(&b, 1);
	(void)close(devnull);

	printf("sink_left=%d delivered_left=%d\n", sink_left, delivered_left);
	return (sink_left == 7 && delivered_left == 0) ? 0 : 1;
}
