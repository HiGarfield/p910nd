/*
 * Behaviour probe for a reset on the printer->network stream.
 *
 * p910nd.c is included so readBuffer() is reachable.  A loopback TCP pair
 * whose server end is closed with SO_LINGER{on,0} makes the next read()
 * fail with ECONNRESET.  The buffer is initialised with detectEof=0, exactly
 * like the printer->network direction on a character device: a reset there
 * must end that direction, otherwise the bidirectional loop keeps polling a
 * descriptor that can never produce data again.
 *
 * Exit 0 means readBuffer() marked the stream as finished.
 */
#define main p910nd_main_unused
#include "p910nd.c"
#undef main

int main(void)
{
	struct sockaddr_in addr;
	struct linger l;
	socklen_t len;
	Buffer_t b;
	int lst;
	int c;
	int s;

	lst = socket(AF_INET, SOCK_STREAM, 0);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (lst < 0 || bind(lst, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(lst, 1) < 0)
		return 2;
	len = sizeof(addr);
	if (getsockname(lst, (struct sockaddr *)&addr, &len) < 0)
		return 2;
	c = socket(AF_INET, SOCK_STREAM, 0);
	if (c < 0 || connect(c, (struct sockaddr *)&addr, len) < 0)
		return 2;
	s = accept(lst, 0, 0);
	if (s < 0)
		return 2;
	l.l_onoff = 1;
	l.l_linger = 0;
	(void)setsockopt(s, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
	(void)close(s);
	(void)close(lst);

	initBuffer(&b, c, -1, 0);
	(void)readBuffer(&b);
	printf("eof_read=%d err=%d\n", b.eof_read, b.err);
	(void)close(c);
	return (b.eof_read == 1) ? 0 : 1;
}
