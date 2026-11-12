#!/bin/sh
#
# PD-59  [medium]  status must survive a missing pid file.
#
# After PD-49 status trusted only /var/run/p910[0-9]d.pid.  A foreground
# -d instance or a TESTING build writes no pid file, and /var/run may be
# cleaned while the daemon lives; in all those cases status still reported
# a live daemon as dead, so try-restart / force-reload skipped the restart.
#
# main() names the process p910<port>d, so the status helper now also scans
# /proc/*/cmdline argv[0] as a second source of truth.
#
# A: live p9100d, no pid file -> status 0.
# B: no daemon at all -> status non-zero.
# C: only an unrelated process -> status non-zero.
# D: static -- the /proc fallback exists.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-59 init-status-no-pidfile

INIT="$REPO_ROOT/aux/p910nd.init"
PIDDIR="$CASE_DIR/piddir"
BIN="$CASE_DIR/bin"
CONFIG="$CASE_DIR/p910nd.conf"
mkdir -p "$PIDDIR" "$BIN"
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

say "A: live daemon, no pid file"
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
obs_kv "pid files present" "$(ls "$PIDDIR" | wc -l | tr -d ' ')"
run_status running
assert_eq "$STATUS_RC" "0" \
	"A status finds a running daemon without any pid file"
assert_contains "$CASE_DIR/running.out" "running" \
	"A and prints the LSB 'running' message"

say "B: no daemon at all"
kill -TERM "$FAKE_PID" 2>/dev/null
wait "$FAKE_PID" 2>/dev/null
run_status absent
[ "$STATUS_RC" -ne 0 ]
_b_rc=$?
assert_true "$_b_rc" \
	"B with no pid file and no p910<port>d process status stays negative"

say "C: only an unrelated live process"
"$PYTHON" -c 'import os
os.execv("/bin/sleep", ["sleep", "30"])' >/dev/null 2>&1 &
OTHER_PID=$!
sleep 0.2
run_status unrelated
[ "$STATUS_RC" -ne 0 ]
_c_rc=$?
assert_true "$_c_rc" \
	"C an unrelated process is not counted as a daemon"
kill -TERM "$OTHER_PID" 2>/dev/null
wait "$OTHER_PID" 2>/dev/null

say "D: static -- the fallback is present"
assert_contains "$INIT" 'for p910nd_proc in /proc/\[0-9\]\*/cmdline' \
	"D the status helper scans /proc argv[0]"
assert_contains "$INIT" "PD-59: a pid file can be missing" \
	"D the reason for the fallback is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
