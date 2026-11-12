/*
 * Behaviour probe for take_lock()'s blocking (F_SETLKW) path.
 *
 * A helper child holds byte 0 of the lock file.  In the parent got_term is
 * already set (SIGTERM arrived before the wait began) and take_lock() is
 * called with an unbounded wait, exactly as one_job() does under inetd.
 * With no new signal to interrupt fcntl(), the old loop blocks forever;
 * SIGALRM bounds this probe to three seconds.
 *
 * argv[1] = lock file path; argv[2] = "no-term" to skip setting got_term
 * (control: the lock really is held, so the call must still block).
 *
 * Exit 0: take_lock() returned immediately on got_term.
 * Exit 2: it blocked until SIGALRM (control expects this).
 */
#define main p910nd_main_unused
#include "p910nd.c"
#undef main

static void on_alarm(int sig)
{
	(void)sig;
	_exit(2);
}

int main(int argc, char **argv)
{
	int fd;
	int cfd;
	int rc;
	int i;
	int held = 0;
	pid_t holder;
	struct flock fl;

	if (argc < 2)
		return 3;
	fd = open(argv[1], O_CREAT | O_RDWR, 0600);
	if (fd < 0)
		return 3;
	holder = fork();
	if (holder < 0)
		return 3;
	if (holder == 0) {
		cfd = open(argv[1], O_RDWR);
		if (cfd < 0)
			_exit(4);
		memset(&fl, 0, sizeof(fl));
		fl.l_type = F_WRLCK;
		fl.l_whence = SEEK_SET;
		fl.l_start = 0;
		fl.l_len = 1;
		if (fcntl(cfd, F_SETLK, &fl) != 0)
			_exit(4);
		sleep(20);
		_exit(0);
	}
	for (i = 0; i < 200; i++) {
		memset(&fl, 0, sizeof(fl));
		fl.l_type = F_WRLCK;
		fl.l_whence = SEEK_SET;
		fl.l_start = 0;
		fl.l_len = 1;
		if (fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type != F_UNLCK) {
			held = 1;
			break;
		}
		usleep(10000);
	}
	if (!held) {
		kill(holder, SIGKILL);
		(void)waitpid(holder, 0, 0);
		return 4;
	}
	if (argc < 3 || strcmp(argv[2], "no-term") != 0)
		got_term = 1;
	(void)signal(SIGALRM, on_alarm);
	(void)alarm(3);
	rc = take_lock(fd, 0, -1L);
	(void)alarm(0);
	printf("take_lock rc=%d held=%d got_term=%d\n", rc, held,
	       (int)got_term);
	kill(holder, SIGKILL);
	(void)waitpid(holder, 0, 0);
	(void)close(fd);
	return (rc == 0) ? 0 : 1;
}
