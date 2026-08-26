/*
 * Regression test: prepBuffer() must stop arming writefds once the buffer
 * has fully drained (eof_sent == 1).
 *
 * Background: after a hard read error on the printer side, readBuffer()
 * sets READ_ERR and eof_read on the printer-to-network buffer.  With
 * eof_read stuck at 1 and no remaining bytes, prepBuffer() used to keep
 * adding the network fd to writefds forever, because its condition was
 * (bytes != 0 || eof_read).  The network socket is almost always writable,
 * so select() returned immediately every iteration and writeBuffer() spun
 * with no progress (100% CPU busy loop) until network EOF or the 30s
 * timeout.
 *
 * The fix: once eof_sent is set (all buffered bytes have been delivered)
 * the write side no longer needs to be polled.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>

int main(void)
{
	Buffer_t b;
	fd_set readfds, writefds;
	int sv[2];

	/* Test must never hang: SIGALRM default action terminates the process. */
	(void)alarm(10);

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

	/* Printer-to-network semantics: detectEof off, printer fd as input. */
	initBuffer(&b, sv[0], sv[1], 0);

	/* Simulate a hard read error on the printer side. */
	b.eof_read = 1;
	b.err = READ_ERR;

	/* eof_read with no buffered bytes: reading stops, but the write side is
	 * still armed once so that writeBuffer() can propagate eof_sent. */
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	prepBuffer(&b, &readfds, &writefds);
	assert(!FD_ISSET(sv[0], &readfds));  /* eof_read: stop reading */
	assert(FD_ISSET(sv[1], &writefds));  /* one poll to propagate EOF */

	/* Drain step: writeBuffer() with no bytes left reports EOF sent. */
	assert(writeBuffer(&b) == 0);
	assert(b.eof_sent == 1);

	/*
	 * Before the fix this second prepBuffer() would still arm writefds
	 * (eof_read stays 1), making select() return immediately forever.
	 */
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	prepBuffer(&b, &readfds, &writefds);
	assert(!FD_ISSET(sv[1], &writefds));

	/*
	 * With eof_read already set (the prior block left it at 1) and five
	 * fresh bytes buffered, writeBuffer() must emit those bytes AND, because
	 * the input has reached EOF (eof_read) and the buffer is now drained
	 * (bytes == 0), propagate eof_sent in the same call.  This verifies the
	 * "drain + EOF propagation" coupling: a partial write that empties the
	 * buffer under eof_read marks the stream fully sent.
	 */
	b.eof_sent = 0;
	b.bytes = 5;
	b.startidx = 0;
	b.endidx = 5;
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	prepBuffer(&b, &readfds, &writefds);
	assert(FD_ISSET(sv[1], &writefds));
	assert(writeBuffer(&b) == 5);
	assert(b.bytes == 0);
	/* The five bytes must have reached the peer (sv[1] -> sv[0]). */
	{
		char chunk[8];
		ssize_t n = read(sv[0], chunk, sizeof(chunk));
		assert(n == 5);
		assert(memcmp(chunk, b.buffer, 5) == 0);
	}
	/* EOF is already propagated by the write that drained the buffer under
	 * eof_read, so eof_sent is now set; a subsequent empty writeBuffer()
	 * (simulating the next poll) leaves it set and returns 0. */
	assert(b.eof_sent == 1);
	assert(writeBuffer(&b) == 0);
	assert(b.eof_sent == 1);
	/* And after eof_sent the write side is no longer polled. */
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	prepBuffer(&b, &readfds, &writefds);
	assert(!FD_ISSET(sv[1], &writefds));

	close(sv[0]);
	close(sv[1]);
	fprintf(stderr, "PASS: eof_sent stops writefds arming, drain still works\n");
	return 0;
}
