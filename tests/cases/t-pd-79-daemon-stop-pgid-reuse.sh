#!/bin/sh
#
# PD-79  [medium]  daemon_stop() must not kill a recycled process group.
#
# aux/p910nd.init already revalidates pids before SIGKILL (PD-12), but the
# harness did not: daemon_stop() sent TERM and then an unconditional
# `kill -KILL "-$DAEMON_PGID"`.  A process-group id is just a number; once
# the daemon exits the kernel can give it to an unrelated group.  Signalling
# -PGID then kills processes the test never started (a parallel CI job, an
# editor, another worktree's helper).
#
# A: with a dead daemon pid and a live unrelated group holding that pgid,
#    daemon_stop() must leave the unrelated process alone.
# B: control -- a real daemon started by daemon_start() is still stopped.
# C: static -- the guard exists and gates the signals.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-79 daemon-stop-pgid-reuse

LIB="$TESTS_DIR/lib.sh"
PIDFILE="$CASE_DIR/unrelated.pid"

say "A: a recycled pgid must not be signalled"
rm -f "$PIDFILE"
# setsid gives the helper its own session, so its pid IS the process-group
# id.  exec keeps that number, and argv[0] becomes "sleep" -- nothing the
# harness could mistake for one of its daemons.
setsid sh -c 'echo $$ > "$1"; shift; exec "$@"' _ "$PIDFILE" sleep 30 \
	>/dev/null 2>&1 </dev/null &
_n=0
UNRELATED_PID=
while [ "$_n" -lt 50 ]; do
	[ -s "$PIDFILE" ] && UNRELATED_PID=$(cat "$PIDFILE")
	[ -n "$UNRELATED_PID" ] && break
	sleep 0.1
	_n=$((_n + 1))
done
obs_kv "unrelated process pid/pgid" "${UNRELATED_PID:-missing}"
if [ -n "$UNRELATED_PID" ]; then _up=0; else _up=1; fi
assert_true "$_up" "A the unrelated process group is running"

# A pid that is already gone and reaped: exactly what a stopped daemon
# leaves behind before its number gets recycled.
sleep 0 &
DEAD_PID=$!
wait "$DEAD_PID"
obs_kv "dead daemon pid" "$DEAD_PID"
DAEMON_PID=$DEAD_PID
DAEMON_PGID=$UNRELATED_PID

daemon_stop

_state=$(ps -o stat= -p "$UNRELATED_PID" 2>/dev/null | tr -d ' ')
obs_kv "unrelated process state after daemon_stop" "${_state:-gone}"
case "$_state" in
Z*|"")	assert_true 1 "A daemon_stop did not kill an unrelated group" ;;
*)	assert_true 0 "A daemon_stop did not kill an unrelated group" ;;
esac
assert_eq "${DAEMON_PID:-}" "" "A daemon_stop still clears its state"
assert_eq "${DAEMON_PGID:-}" "" "A and clears the pgid too"
kill -KILL "$UNRELATED_PID" 2>/dev/null
wait 2>/dev/null

say "B: control -- a real daemon is still stopped normally"
CASE_BIN="$BUILD_DIR/test"
DEV="$CASE_DIR/lp0"
: >"$DEV"
daemon_start "$CASE_DIR/daemon.log" "$CASE_BIN" -d -i 127.0.0.1 \
	-f "$DEV" 0
if [ -z "$DAEMON_PID" ]; then
	obs "control daemon failed to start: $(tr '\n' '|' <"$CASE_DIR/daemon.log")"
	assert_true 1 "B the control daemon starts"
else
	wait_for_port 127.0.0.1 "$TEST_PORT" 5
	assert_eq "$?" "0" "B the control daemon listens"
	daemon_stop
	wait_for_gone 127.0.0.1 "$TEST_PORT" 5
	assert_eq "$?" "0" "B daemon_stop still stops the daemon it started"
fi

say "C: static -- the guard gates every signal"
assert_contains "$LIB" "daemon_group_is_ours" \
	"C daemon_stop revalidates the group leader"
assert_contains "$LIB" '] && daemon_group_is_ours; then' \
	"C and the signals are inside that condition"
assert_contains "$LIB" "PD-79: a process-group id" \
	"C the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
