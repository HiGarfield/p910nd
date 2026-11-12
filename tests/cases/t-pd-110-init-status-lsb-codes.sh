#!/bin/sh
#
# PD-110  [medium]  status must honour the LSB codes it documents.
#
# aux/p910nd.init carried a template "# TODO: Add checking for lockfiles"
# next to checkproc(), and its status branch documented four LSB outcomes
# (0 running, 1 dead+pid file, 2 dead+lock file, 3 not running) but
# implemented only "a process is visible" vs "delegate to checkproc()".
# checkproc() is a name lookup, so every non-running case collapsed into a
# generic failure/unknown; stale pid and lock files -- exactly what an init
# script must report after a crashed daemon -- were never distinguished.
#
# A: genuinely stopped service returns 3.
# B: a stale pid file returns 1.
# C: a stale lock file returns 2.
# D: both present -> the pid file wins (1).
# E: control -- a live renamed daemon still returns 0 / "running".
# F: static -- the TODO is gone and the lock directory is a test hook.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-110 init-status-lsb-codes

INIT="$REPO_ROOT/aux/p910nd.init"
PIDDIR="$CASE_DIR/piddir"
LOCKDIR="$CASE_DIR/lockdir"
BIN="$CASE_DIR/bin"
CONFIG="$CASE_DIR/p910nd.conf"
mkdir -p "$PIDDIR" "$LOCKDIR" "$BIN" || exit 1
cp /bin/true "$BIN/p910nd" && chmod +x "$BIN/p910nd"
printf 'P910ND_NUM=""\nP910ND_OPTS=""\n' >"$CONFIG"
sed -e "s|^P910ND_BIN=/usr/sbin/p910nd|P910ND_BIN=$BIN/p910nd|" \
	-e "s|^P910ND_CONFIG=/etc/sysconfig/p910nd|P910ND_CONFIG=$CONFIG|" \
	"$INIT" >"$CASE_DIR/init-copy"
chmod +x "$CASE_DIR/init-copy"

run_status() {
	P910ND_PIDDIR="$PIDDIR" P910ND_LOCKDIR="$LOCKDIR" \
		bash "$CASE_DIR/init-copy" status \
		>"$CASE_DIR/$1.out" 2>&1
	STATUS_RC=$?
	obs "$1 exit: $STATUS_RC output: $(tr '\n' '|' <"$CASE_DIR/$1.out")"
}

clear_state() {
	rm -f "$PIDDIR"/p910[0-9]d.pid "$LOCKDIR"/p910[0-9]d
}

say "A: no daemon, no files -> 3 (not running)"
clear_state
run_status stopped
assert_eq "$STATUS_RC" "3" \
	"A a genuinely stopped service reports LSB 3"

say "B: a stale pid file -> 1"
clear_state
printf '99999999\n' >"$PIDDIR/p9100d.pid"
run_status stale-pid
assert_eq "$STATUS_RC" "1" \
	"B a dead service with a pid file reports LSB 1"

say "C: a stale lock file -> 2"
clear_state
: >"$LOCKDIR/p9100d"
run_status stale-lock
assert_eq "$STATUS_RC" "2" \
	"C a dead service with a lock file reports LSB 2"

say "D: both stale files -> pid file wins (1)"
clear_state
printf '99999999\n' >"$PIDDIR/p9101d.pid"
: >"$LOCKDIR/p9101d"
run_status both
assert_eq "$STATUS_RC" "1" \
	"D the pid file takes precedence over the lock file"

say "E: control -- a live renamed daemon still reports running"
clear_state
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
printf '%s\n' "$FAKE_PID" >"$PIDDIR/p9100d.pid"
run_status live
assert_eq "$STATUS_RC" "0" \
	"E a running daemon with a renamed argv[0] reports LSB 0"
assert_contains "$CASE_DIR/live.out" "running" \
	"E and prints the LSB 'running' message"
kill -TERM "$FAKE_PID" 2>/dev/null
wait "$FAKE_PID" 2>/dev/null

say "F: static -- the TODO is resolved"
assert_not_contains "$INIT" "TODO: Add checking for lockfiles" \
	"F the outdated TODO is gone"
assert_contains "$INIT" 'P910ND_LOCKDIR=${P910ND_LOCKDIR:-/var/lock/subsys}' \
	"F the lock directory is a configurable hook like P910ND_PIDDIR"
assert_contains "$INIT" "PD-110: implement the LSB codes" \
	"F the reason is documented"
assert_contains "$INIT" 'if test -n "$(p910nd_running_pids)"; then' \
	"F the PD-49 process source of truth is unchanged"
assert_contains "$INIT" 'checkproc "$P910ND_BIN"' \
	"F and the name-based fallback is still consulted"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
