/*
 * Functional regression tests for copy_stream().
 *
 *  - test_uni_dir:         unidirectional mode, network -> printer must be
 *                          byte-for-byte identical.
 *  - test_bidir_data:      bidirectional mode, network -> printer must be
 *                          byte-for-byte identical (printer stays silent).
 *  - test_bidir_response:  bidirectional mode, printer -> network response
 *                          must be delivered to the network peer.
 *
 * Each test forks a "client" peer (sends data) and a "printer device" peer
 * (consumes/echoes data), then runs copy_stream() in the parent with real
 * connected socketpairs, exactly as the daemon would.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>

#define RESP "ACK-9100"

static void sendall(int fd, const char *buf, size_t len)
{
	size_t off = 0;
	while (off < len)
	{
		ssize_t n = write(fd, buf + off, len - off);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			perror("write");
			exit(1);
		}
		off += (size_t)n;
	}
}

static size_t readall(int fd, char *buf, size_t cap)
{
	size_t off = 0;
	while (off < cap)
	{
		ssize_t n = read(fd, buf + off, cap - off);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			perror("read");
			exit(1);
		}
		if (n == 0)
			break;
		off += (size_t)n;
	}
	return off;
}

static void fill_pattern(char *buf, size_t len)
{
	size_t i;
	for (i = 0; i < len; i++)
		buf[i] = (char)((i * 31) + (i >> 8) + 7);
}

static void assert_exited(pid_t pid)
{
	int status;
	assert(waitpid(pid, &status, 0) == pid);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		fprintf(stderr, "child %ld exited status=0x%x exit=%d\n",
			(long)pid, status, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);
}

/* Unidirectional: network -> printer, byte-for-byte. */
static void test_uni_dir(void)
{
	int net_sv[2], prn_sv[2];
	pid_t client, prn;
	char data[65536];
	char got[65536];
	FILE *f;
	size_t n, i;
	int status;

	fill_pattern(data, sizeof(data));

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);

	f = tmpfile();
	assert(f != NULL);

	client = fork();
	assert(client >= 0);
	if (client == 0)
	{
		close(net_sv[0]);
		close(prn_sv[0]);
		close(prn_sv[1]);
		sendall(net_sv[1], data, sizeof(data));
		shutdown(net_sv[1], SHUT_WR);
		_exit(0);
	}
	prn = fork();
	assert(prn >= 0);
	if (prn == 0)
	{
		close(net_sv[0]);
		close(net_sv[1]);
		close(prn_sv[0]);
		while (1)
		{
			char buf[4096];
			ssize_t r = read(prn_sv[1], buf, sizeof(buf));
			if (r < 0)
			{
				if (errno == EINTR)
					continue;
				perror("prn read");
				_exit(2);
			}
			if (r == 0)
				break;
			fwrite(buf, 1, (size_t)r, f);
		}
		fflush(f);
		_exit(0);
	}
	close(net_sv[1]);
	close(prn_sv[1]);

	bidir = 0;
	log_to_stdout = 0;
	(void)copy_stream(net_sv[0], prn_sv[0]);
	close(net_sv[0]);
	close(prn_sv[0]);

	assert_exited(client);
	assert_exited(prn);

	rewind(f);
	n = fread(got, 1, sizeof(got), f);
	fclose(f);
	assert(n == sizeof(data));
	assert(memcmp(got, data, sizeof(data)) == 0);
	(void)status;
	(void)i;
}

/* Bidirectional: network -> printer while printer stays silent. */
static void test_bidir_data(void)
{
	int net_sv[2], prn_sv[2];
	pid_t client, prn;
	char data[65536];
	char got[65536];
	FILE *f;
	size_t n;

	fill_pattern(data, sizeof(data));

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
	assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);

	f = tmpfile();
	assert(f != NULL);

	client = fork();
	assert(client >= 0);
	if (client == 0)
	{
		close(net_sv[0]);
		close(prn_sv[0]);
		close(prn_sv[1]);
		sendall(net_sv[1], data, sizeof(data));
		shutdown(net_sv[1], SHUT_WR);
		_exit(0);
	}
	prn = fork();
	assert(prn >= 0);
	if (prn == 0)
	{
		close(net_sv[0]);
		close(net_sv[1]);
		close(prn_sv[0]);
		while (1)
		{
			char buf[4096];
			ssize_t r = read(prn_sv[1], buf, sizeof(buf));
			if (r < 0)
			{
				if (errno == EINTR)
					continue;
				perror("prn read");
				_exit(2);
			}
			if (r == 0)
				break;
			fwrite(buf, 1, (size_t)r, f);
		}
		fflush(f);
		_exit(0);
	}
	close(net_sv[1]);
	close(prn_sv[1]);

	bidir = 1;
	log_to_stdout = 0;
	(void)copy_stream(net_sv[0], prn_sv[0]);
	close(net_sv[0]);
	close(prn_sv[0]);

	assert_exited(client);
	assert_exited(prn);

	rewind(f);
	n = fread(got, 1, sizeof(got), f);
	fclose(f);
	assert(n == sizeof(data));
	assert(memcmp(got, data, sizeof(data)) == 0);
}

/* Bidirectional: printer echoes a response after receiving data; the
 * network peer must receive it before it shuts the connection down. */
static void test_bidir_response(void)
{
	int net_sv[2], prn_sv[2];
	pid_t client, prn;
	char data[65536];
	char resp[64];
	char got[65536];
	FILE *f;
	size_t n, total = 0, gotresp = 0;

	fill_pattern(data, sizeof(data));

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, net_sv) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, prn_sv) == 0);
	assert(fcntl(prn_sv[0], F_SETFL, O_NONBLOCK) == 0);

	f = tmpfile();
	assert(f != NULL);

	client = fork();
	assert(client >= 0);
	if (client == 0)
	{
		close(net_sv[0]);
		close(prn_sv[0]);
		close(prn_sv[1]);
		sendall(net_sv[1], data, sizeof(data));
		/* Keep the connection open until the response has been read. */
		gotresp = readall(net_sv[1], resp, sizeof(resp));
		/* Now allow the daemon-side to finish. */
		shutdown(net_sv[1], SHUT_WR);
		_exit(gotresp == strlen(RESP) && memcmp(resp, RESP, gotresp) == 0 ? 0 : 3);
	}
	prn = fork();
	assert(prn >= 0);
	if (prn == 0)
	{
		int sent = 0;
		close(net_sv[0]);
		close(net_sv[1]);
		close(prn_sv[0]);
		while (1)
		{
			char buf[4096];
			ssize_t r = read(prn_sv[1], buf, sizeof(buf));
			if (r < 0)
			{
				if (errno == EINTR)
					continue;
				/* Parent closed its end after copy_stream() finished; a
				 * pending response byte may make that a RST. */
				if (errno == ECONNRESET || errno == EPIPE)
					_exit(0);
				perror("prn read");
				_exit(2);
			}
			if (r == 0)
				break;
			fwrite(buf, 1, (size_t)r, f);
			total += (size_t)r;
			/* After the first bytes have arrived the daemon has already
			 * written to the printer, so need_clear_lp is off and this
			 * response will be forwarded to the network. */
			if (!sent && total >= 100)
			{
				ssize_t wr = write(prn_sv[1], RESP, strlen(RESP));
				sent = 1;
				if (wr < 0 && errno != EPIPE)
					perror("prn resp write");
			}
		}
		fflush(f);
		_exit(0);
	}
	close(net_sv[1]);
	close(prn_sv[1]);

	bidir = 1;
	log_to_stdout = 0;
	(void)copy_stream(net_sv[0], prn_sv[0]);
	close(net_sv[0]);
	close(prn_sv[0]);

	assert_exited(client);
	assert_exited(prn);

	rewind(f);
	n = fread(got, 1, sizeof(got), f);
	fclose(f);
	assert(n == sizeof(data));
	assert(memcmp(got, data, sizeof(data)) == 0);
}

int main(void)
{
	test_uni_dir();
	test_bidir_data();
	test_bidir_response();
	fprintf(stderr, "PASS: all copy_stream functional tests\n");
	return 0;
}
