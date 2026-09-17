/*
 * Stub hosts_ctl() used to exercise p910nd's libwrap rejection path.
 *
 * hosts_access() decides from /etc/hosts.allow and /etc/hosts.deny, and
 * writing those needs root, which the test suite must not require (and must
 * not do to the machine it runs on).  Preloading this shared object replaces
 * the real hosts_ctl() with one that always denies, so the daemon's own
 * "connection rejected" branch runs for real: it must log, close the
 * descriptor and carry on serving.
 *
 * Build: cc -shared -fPIC -o hosts_ctl_stub.so tests/hosts_ctl_stub.c
 * Use:   LD_PRELOAD=<that> ./p910nd ...   (with a -DUSE_LIBWRAP build)
 *
 * GPLv2, like the rest of this project.
 */

int hosts_ctl(char *daemon, char *client_name, char *client_addr,
              char *client_user)
{
	(void)daemon;
	(void)client_name;
	(void)client_addr;
	(void)client_user;
	return 0; /* deny */
}
