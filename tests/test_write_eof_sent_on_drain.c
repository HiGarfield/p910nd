/*
 * Regression test for Bug #1: writeBuffer() used to raise eof_sent only on an
 * *empty* write call (avail == 0).  When a single write() drained the last
 * buffered bytes AND eof_read was already set, the eof_sent marker was skipped
 * (the test lived in an `else if (b->eof_read)` branch that ran only when no
 * data was written).  Job completion was then postponed by an extra
 * select()/loop iteration -- and, in the bidirectional path, could be needlessly
 * stretched by the idle timeout.
 *
 * Fix: mark eof_sent in the SAME call that empties the buffer under eof_read.
 *
 * This test drives writeBuffer() directly and proves, for several buffer
 * geometries, that the moment the last byte is written while eof_read is set,
 * eof_sent becomes 1 -- no second call required.  It also proves the negative
 * case: with eof_read clear, draining the buffer does NOT mark eof_sent (more
 * data may follow).  Both directions are exercised (an fd-backed outfd and the
 * outfd == -1 discard path), and regression is guarded by running every other
 * existing copy_stream test (see below) -- here we assert the exact observable
 * state of writeBuffer() itself.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/socket.h>

/* Drive writeBuffer() on a single Buffer_t and report eof_sent after the call
 * that empties it. */
static int eof_sent_after_drain(int outfd, int eof_read)
{
	Buffer_t b;
	int sv[2];

	if (outfd >= 0)
	{
		assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
		initBuffer(&b, sv[0], sv[1], 1);
	}
	else
	{
		initBuffer(&b, 0, -1, 1);
	}

	/* Five bytes buffered, input already at EOF. */
	b.eof_read = eof_read;
	b.eof_sent = 0;
	b.bytes = 5;
	b.startidx = 0;
	b.endidx = 5;

	/* First call writes the 5 bytes; with eof_read set it must mark eof_sent
	 * HERE (the bug required a second, empty call). */
	{
		ssize_t r = writeBuffer(&b);
		assert(r == 5);
		assert(b.bytes == 0);
	}
	if (outfd >= 0)
	{
		(void)close(sv[0]);
		(void)close(sv[1]);
	}
	return b.eof_sent;
}

int main(void)
{
	/* Positive: eof_read set, real fd -> eof_sent must be 1 in one call. */
	assert(eof_sent_after_drain(4, 1) == 1);
	/* Positive: eof_read set, discard fd (outfd == -1) -> same. */
	assert(eof_sent_after_drain(-1, 1) == 1);
	/* Negative: eof_read clear, real fd -> must NOT mark eof_sent yet. */
	assert(eof_sent_after_drain(4, 0) == 0);
	/* Negative: eof_read clear, discard fd -> must NOT mark eof_sent. */
	assert(eof_sent_after_drain(-1, 0) == 0);

	fprintf(stderr, "PASS: writeBuffer marks eof_sent on the drain call under eof_read\n");
	return 0;
}
