/*
 * Behaviour probe for flush_buffer()'s undelivered-byte result when the
 * ring is wrapped.  A sink (outfd < 0) consumes the ring in two passes:
 * writeBuffer() can only remove the contiguous run up to the end of the
 * buffer, then the rest from index 0.  The old loop added b->bytes to
 * `discarded` on EVERY pass, so the second pass counted the tail twice and
 * the "reply: N bytes not delivered" log over-reported the loss.
 *
 * Exit 0 means the reported loss equals the bytes actually in the ring,
 * for both a wrapped ring and a plain one.
 */
#define main p910nd_main_unused
#include "p910nd.c"
#undef main

int main(void)
{
	Buffer_t b;
	int have = 7;
	int wrapped;
	int plain;

	initBuffer(&b, -1, -1, 0);
	memset(b.buffer, 'Z', sizeof(b.buffer));
	/* Wrapped ring: 4 bytes at the tail, 3 bytes at the head. */
	b.startidx = (int)sizeof(b.buffer) - 4;
	b.endidx = 3;
	b.bytes = have;
	b.totalin = (unsigned long)have;
	wrapped = flush_buffer(&b, 1);

	initBuffer(&b, -1, -1, 0);
	memset(b.buffer, 'Z', sizeof(b.buffer));
	b.endidx = have;
	b.bytes = have;
	b.totalin = (unsigned long)have;
	plain = flush_buffer(&b, 1);

	printf("have=%d wrapped_lost=%d plain_lost=%d\n", have, wrapped, plain);
	return (wrapped == have && plain == have) ? 0 : 1;
}
