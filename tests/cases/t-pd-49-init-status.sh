#!/bin/sh
#
# PD-49  [high]  `service p910nd status` must find a running daemon.
#
# main() rewrites argv[0] to p910<port>d, but the status branch only called
# checkproc $P910ND_BIN, which matches by the binary name.  On systems
# without /etc/rc.status the script's own fallback defines checkproc() as
# "rc_failed 4" -- always "unknown".  So status reported "not running /
# unknown" for a live daemon, and try-restart / force-reload (which ask
# status first) silently skipped the restart.
#
# The stop branch already solves this by reading the daemon's pid files and
# validating /proc/PID/cmdline; status must use the same source of truth.
#
# A: a live daemon (argv[0]=p9100d) with its pid file -> status 0.
# B: no pid file -> status non-zero.
# C: a pid file naming an unrelated live process -> status non-zero.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-49 init-status

INIT="$REPO_ROOT/aux/p910nd.init"
PIDDIR="$CASE_DIR/piddir"
BIN="$CASE_DIR/bin"
CONFIG="$CASE_DIR/p910nd.conf"
mkdir -p "$PIDDIR" "$BIN"

# The script insists on an installed binary and config file.  Give it a
# private, writable copy of both so the case needs no root privileges.
cp /bin/true "$BIN/p910nd" || true
chmod +x "$BIN/p910nd"
printf 'P910ND_NUM=""\nP910ND_OPTS=""\n' >"$CONFIG"
sed -e "s|^P910ND_BIN=/usr/sbin/p910nd|P910ND_BIN=$BIN/p910nd|" \
	-e "s|^P910ND_CONFIG=/etc/sysconfig/p910nd|P910ND_CONFIG=$CONFIG|" \
	"$INIT" >"$CASE_DIR/init-copy"
chmod +x "$CASE_DIR/init-copy"

run_status() {
	P910ND_PIDDIR="$PIDDIR" bash "$CASE_DIR/init-copy" status \
		>"$CASE_DIR/$1.out" 2>&1
	STATUS_RC=$?
	obs "$1 output: $(tr '\n' '|' <"$CASE_DIR/$1.out")"
	obs_kv "$1 exit" "$STATUS_RC"
}

say "A: live daemon with a pid file"
# execv keeps the pid but changes argv[0] to exactly what p910nd writes.
"$PYTHON" -c 'import os
os.execv("/bin/sleep", ["p9100d", "30"])' >/dev/null 2>&1 &
FAKE_PID=$!
_n=0
while [ "$_n" -lt 50 ]; do
	_argv0=$(tr '\0' '\n' <"/proc/$FAKE_PID/cmdline" 2>/dev/null | sed -n 1p)
	[ "$_argv0" = "p9100d" ] && break
	sleep 0.1
	_n=$((_n + 1))
done
obs_kv "fake daemon argv0" "${_argv0:-missing}"
printf '%s\n' "$FAKE_PID" >"$PIDDIR/p9100d.pid"
run_status running
assert_eq "$STATUS_RC" "0" \
	"A status reports a running daemon as running"
assert_contains "$CASE_DIR/running.out" "running" \
	"A and prints the LSB 'running' message"

say "B: neither a pid file nor a live daemon"
rm -f "$PIDDIR/p9100d.pid"
# PD-59 added a /proc argv[0] fallback: the fake daemon from A must be
# gone too, otherwise status would correctly see it by name.
kill -TERM "$FAKE_PID" 2>/dev/null
wait "$FAKE_PID" 2>/dev/null
FAKE_PID=
run_status absent
[ "$STATUS_RC" -ne 0 ]
_b_rc=$?
assert_true "$_b_rc" \
	"B with no pid file and no daemon status does not claim it is running"

say "C: pid file naming an unrelated live process"
"$PYTHON" -c 'import os
os.execv("/bin/sleep", ["sleep", "30"])' >/dev/null 2>&1 &
OTHER_PID=$!
sleep 0.2
printf '%s\n' "$OTHER_PID" >"$PIDDIR/p9100d.pid"
run_status recycled
[ "$STATUS_RC" -ne 0 ]
_c_rc=$?
assert_true "$_c_rc" \
	"C a recycled/unrelated pid is not mistaken for our daemon"

kill -TERM "$OTHER_PID" 2>/dev/null
wait "$OTHER_PID" 2>/dev/null

say "D: static -- status shares the pid-file source of truth"
assert_contains "$INIT" "p910nd_running_pids" \
	"D the status helper exists"
assert_contains "$INIT" "P910ND_PIDDIR" \
	"D the pid directory is configurable for testing"
assert_contains "$INIT" 'if test -n "$(p910nd_running_pids)"; then' \
	"D the status branch prefers the pid files over checkproc"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
