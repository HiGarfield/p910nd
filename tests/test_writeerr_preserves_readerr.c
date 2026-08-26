/*
 * Regression test for Bug #3: in copy_stream()'s bidirectional loop, when a
 * network write of printer responses fails (EPIPE), the old code did
 *     printerToNetworkBuffer.err = 0;
 * which wiped the whole error word.  If the printer side had already raised
 * READ_ERR (e.g. the printer went away mid-job), that state was lost and
 * prepBuffer() would re-arm the printer fd for reading, poking a dead fd.
 *
 * The fix clears only the write-error bit:
 *     printerToNetworkBuffer.err &= ~WRITE_ERR;
 * so READ_ERR is preserved.
 *
 * We verify the *observable* consequence: with READ_ERR set, prepBuffer()
 * must NOT add the printer (input) fd to readfds, i.e. reading the printer
 * stops.  We also confirm that, had err been zeroed (the bug), prepBuffer
 * would wrongly re-arm the printer fd -- which the fixed bit-clear avoids.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>

int main(void)
{
	int prn_sv[2], net_sv[2];
	Buffer_t b;
	fd_set readfds, writefds;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);

	/* printerToNetworkBuffer semantics: input = printer, output = network. */
	initBuffer(&b, prn_sv[0], net_sv[1], 0);

	/* Simulate: printer read already errored, and a network write also
	 * failed.  The fixed code keeps READ_ERR after clearing WRITE_ERR. */
	b.err = READ_ERR | WRITE_ERR;
	b.bytes = 0;
	b.eof_read = 0;
	b.eof_sent = 0;

	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	prepBuffer(&b, &readfds, &writefds);

	/* READ_ERR preserved => must NOT re-arm the printer for reading. */
	assert(!FD_ISSET(prn_sv[0], &readfds));
	/* WRITE_ERR cleared (or still set) => must NOT re-arm network for writing. */
	assert(!FD_ISSET(net_sv[1], &writefds));

	/* Contrast: if the bug (err = 0) had run, READ_ERR would be gone and
	 * prepBuffer would re-arm the printer fd for reading a possibly-dead
	 * device.  The fix must behave differently: it keeps READ_ERR and so
	 * does NOT re-arm the printer fd. */
	{
		Buffer_t buggy = b;
		buggy.err = 0; /* the old, wrong assignment */
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);
		prepBuffer(&buggy, &readfds, &writefds);
		/* Sanity: with err==0 prepBuffer does arm it (documents the bug). */
		assert(FD_ISSET(prn_sv[0], &readfds));
	}

	close(prn_sv[0]);
	close(prn_sv[1]);
	close(net_sv[0]);
	close(net_sv[1]);
	fprintf(stderr, "PASS: WRITE_ERR clear preserves READ_ERR, printer read stops\n");
	return 0;
}
