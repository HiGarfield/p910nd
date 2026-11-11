/*
 * LD_PRELOAD shim that records calls to libwrap's hosts_ctl().
 *
 * P910ND_HOSTS_CTL_LOG names a file; every call appends the daemon name and
 * the client address and allows the connection.  The real hosts.allow /
 * hosts.deny configuration is deliberately bypassed so the probe only answers
 * "was the access check made at all?", which is what one_job() used to miss
 * in (x)inetd mode.
 *
 * The shim is inert without the environment variable.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>

int hosts_ctl(char *daemon, char *client_name, char *client_addr,
              char *client_user)
{
	const char *path = getenv("P910ND_HOSTS_CTL_LOG");
	FILE *fp;

	(void)client_name;
	(void)client_user;
	if (path != NULL) {
		fp = fopen(path, "a");
		if (fp != NULL) {
			fprintf(fp, "%s %s\n", daemon, client_addr);
			fclose(fp);
		}
	}
	return 1; /* allow */
}
