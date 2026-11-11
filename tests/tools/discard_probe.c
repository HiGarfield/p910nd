/*
 * Behaviour probe for the bidirectional "client vanished" path.
 *
 * p910nd.c is included so discard_printer_reply() is reachable.  When a
 * write of the printer's reply fails, the reply direction becomes a sink,
 * but a READ_ERR raised on the same round must survive: the copy loop uses
 * it to stop polling a printer that can no longer be read (PD-05).
 *
 * Exit 0 means only WRITE_ERR was cleared.
 */
#define main p910nd_main_unused
#include "p910nd.c"
#undef main

int main(void)
{
	Buffer_t b;

	initBuffer(&b, 0, 1, 0);
	b.err = READ_ERR | WRITE_ERR;
	discard_printer_reply(&b);
	printf("outfd=%d err=%d read_err_kept=%d write_err_cleared=%d\n",
	       b.outfd, (int)b.err, (b.err & READ_ERR) ? 1 : 0,
	       (b.err & WRITE_ERR) ? 0 : 1);
	return (b.outfd == -1 && (b.err & READ_ERR) &&
	        !(b.err & WRITE_ERR)) ? 0 : 1;
}
