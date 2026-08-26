/*
 * Independent review scenario: in bidirectional mode, the printer emits a
 * response AFTER the network peer has already sent EOF (all job data). The
 * daemon must still forward that late response to the network before the job
 * completes, because a printer may answer a query issued in the last bytes of
 * the job only after the host has closed its send side.
 *
 * This exercises the "any timing" requirement: network EOF and printer
 * response are not ordered. The job is complete only after BOTH the network
 * data has reached the printer AND the printer's response has been delivered
 * to the network (or the printer has gone silent). We assert the response is
 * received intact by the network peer.
 */
#define _GNU_SOURCE
#define main p910nd_original_main
#include "../p910nd.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <sys/time.h>

#define RESP "LATE-RESP-9100"

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
			if (errno == ECONNRESET)
				break;
			perror("read");
			exit(1);
		}
		if (n == 0)
			break;
		off += (size_t)n;
	}
	return off;
}

int main(void)
{
	int net_sv[2], prn_sv[2];
	pid_t client, prn;
	char data[32768];
	char resp[64];
	char prn_got[32768];
	FILE *f;
	size_t n, gotresp;
	int status;

	for (n = 0; n < sizeof(data); n++)
		data[n] = (char)((n * 37) + (n >> 7) + 11);

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
		/* Send all job data then immediately close the send side (EOF),
		 * WITHOUT waiting for the printer response. The daemon must still
		 * forward the late response. */
		sendall(net_sv[1], data, sizeof(data));
		shutdown(net_sv[1], SHUT_WR);
		/* Now read the late response the printer will emit. */
		gotresp = readall(net_sv[1], resp, sizeof(resp));
		_exit(gotresp == strlen(RESP) && memcmp(resp, RESP, gotresp) == 0 ? 0 : 4);
	}
	prn = fork();
	assert(prn >= 0);
	if (prn == 0)
	{
		close(net_sv[0]);
		close(net_sv[1]);
		close(prn_sv[0]);
		size_t total = 0;
		int saw_data = 0;
		/*
		 * Drain the whole job with a non-blocking poll (a real printer device
		 * consumes bytes as they arrive; a socketpair would otherwise deadlock
		 * because the daemon keeps its write end open while it waits for our
		 * late response).  We do NOT answer while the job is still streaming
		 * in; instead we answer only AFTER the daemon has delivered every byte
		 * (i.e. after network EOF), which is exactly the "printer responds late"
		 * timing the fix must handle.  The non-blocking drain still collects all
		 * bytes before the daemon's grace window elapses.
		 */
		assert(fcntl(prn_sv[1], F_SETFL, O_NONBLOCK) == 0);
		{
			struct timeval dl;
			gettimeofday(&dl, NULL);
			dl.tv_sec += 10;
			while (total < sizeof(data))
			{
				char buf[4096];
				ssize_t r = read(prn_sv[1], buf, sizeof(buf));
				if (r > 0)
				{
					fwrite(buf, 1, (size_t)r, f);
					total += (size_t)r;
					saw_data = 1;
					continue;
				}
				if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
				{
					if (errno == ECONNRESET)
						break;
					perror("prn read");
					_exit(2);
				}
				/* No data yet: spin briefly until the deadline. */
				{
					struct timeval now;
					gettimeofday(&now, NULL);
					if (now.tv_sec > dl.tv_sec ||
						(now.tv_sec == dl.tv_sec && now.tv_usec > dl.tv_usec))
						break;
					usleep(2000);
				}
			}
		}
		/* Emit the response only after the full job has been consumed.  The
		 * daemon, having reached network EOF, keeps the connection open for a
		 * grace period; our late response must still be forwarded. */
		if (saw_data)
		{
			ssize_t wr = write(prn_sv[1], RESP, strlen(RESP));
			if (wr < 0 && errno != EPIPE)
				perror("prn resp write");
		}
		fflush(f);
		_exit(total == sizeof(data) ? 0 : 3);
	}
	close(net_sv[1]);
	close(prn_sv[1]);

	bidir = 1;
	log_to_stdout = 0;
	(void)copy_stream(net_sv[0], prn_sv[0]);
	close(net_sv[0]);
	close(prn_sv[0]);

	assert(waitpid(client, &status, 0) == client);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(waitpid(prn, &status, 0) == prn);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	rewind(f);
	n = fread(prn_got, 1, sizeof(prn_got), f);
	fclose(f);
	assert(n == sizeof(data));
	assert(memcmp(prn_got, data, sizeof(data)) == 0);

	fprintf(stderr, "PASS: bidirectional late printer response after network EOF delivered\n");
	return 0;
}
