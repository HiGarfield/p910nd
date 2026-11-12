#!/bin/sh
#
# PD-74  [medium]  SIGINT/SIGTERM must actually stop the test driver.
#
# run-all.sh installed `trap 'cleanup; kill_stray' EXIT INT TERM`.  A trap
# handler that does not exit returns to the interrupted command, so a
# cancellation signal from CI only ran cleanup and then the driver kept
# building and running cases -- and could finally exit 0, reporting a green
# suite for a job that was cancelled.  The signal traps must exit explicitly,
# while EXIT still performs the cleanup on every path.
#
# The probe below is generated from the REAL trap lines of run-all.sh (with
# cleanup/kill_stray stubbed out), so it exercises the installed trap text
# rather than a copy of the intended behaviour.
#
# A: SIGTERM makes the probe exit promptly with 143.
# B: control -- with no signal the probe reaches its normal exit 0.
# C: static -- the signal trap exits and the combined trap is gone.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-74 runall-signal-exit

RUNALL="$TESTS_DIR/run-all.sh"

make_probe() {
	_out=$1
	_marker=$2
	_pidfile=$3
	{
		printf '%s\n' '#!/bin/sh'
		printf '%s\n' 'cleanup() { :; }'
		printf '%s\n' 'kill_stray() { :; }'
		grep '^trap ' "$RUNALL"
		printf 'echo $$ > "%s"\n' "$_pidfile"
		printf ': > "%s"\n' "$_marker"
		# A shell defers traps until the foreground child returns, so one
		# long `sleep 2` would hide the fix.  A loop of tiny sleeps keeps
		# the trap latency <= 0.05 s while an ignored signal still keeps
		# the probe busy for ~2 s (well past the 1.2 s poll window).
		printf '%s\n' 'i=0'
		printf '%s\n' 'while [ "$i" -lt 40 ]; do'
		printf '\tsleep 0.05\n'
		printf '\ti=$((i + 1))\n'
		printf '%s\n' 'done'
		printf '%s\n' 'exit 0'
	} >"$_out"
	chmod +x "$_out"
}

wait_file() {
	_f=$1
	_max=$2
	_n=0
	while [ ! -f "$_f" ] && [ "$_n" -lt "$_max" ]; do
		sleep 0.05
		_n=$((_n + 1))
	done
	[ -f "$_f" ]
}

say "A: SIGTERM terminates the driver instead of being ignored"
make_probe "$CASE_DIR/probe-signal.sh" \
	"$CASE_DIR/signal.ready" "$CASE_DIR/signal.pid"
obs "probe trap lines:"
grep '^trap ' "$CASE_DIR/probe-signal.sh" | while read -r l; do obs "    $l"; done
assert_ge "$(count_lines "$CASE_DIR/probe-signal.sh" '^trap ')" "1" \
	"the real trap lines were extracted into the probe"

rm -f "$CASE_DIR/signal.rc" "$CASE_DIR/signal.ready" "$CASE_DIR/signal.pid"
(
	sh "$CASE_DIR/probe-signal.sh" >"$CASE_DIR/probe-signal.out" 2>&1
	printf '%s\n' "$?" >"$CASE_DIR/signal.rc"
) &
WRAPPER_PID=$!
wait_file "$CASE_DIR/signal.ready" 60
ready_rc=$?
assert_eq "$ready_rc" "0" "A the probe reached its running state"
PROBE_PID=$(cat "$CASE_DIR/signal.pid" 2>/dev/null)
obs_kv "probe pid" "${PROBE_PID:-missing}"
kill -TERM "$PROBE_PID" 2>/dev/null

# 24 * 0.05 = 1.2 s: the old trap keeps the probe alive for the full 2 s
# sleep, so the rc file cannot appear in this window.
wait_file "$CASE_DIR/signal.rc" 24
prompt_rc=$?
assert_eq "$prompt_rc" "0" \
	"A the probe exits promptly after SIGTERM (rc file appeared in 1.2 s)"
if [ "$prompt_rc" -eq 0 ]; then
	SIGNAL_RC=$(cat "$CASE_DIR/signal.rc")
else
	SIGNAL_RC=missing
	kill -KILL "$PROBE_PID" 2>/dev/null
	kill -KILL "$WRAPPER_PID" 2>/dev/null
fi
# The wrapper wrote the rc file, so it has finished; do not wait on it (a
# second wait on an already-reaped job wedges some /bin/sh implementations).
obs_kv "probe exit after SIGTERM" "$SIGNAL_RC"
assert_eq "$SIGNAL_RC" "143" \
	"A and reports the standard terminated-by-signal status"

say "B: control -- a normal run still exits 0"
make_probe "$CASE_DIR/probe-normal.sh" \
	"$CASE_DIR/normal.ready" "$CASE_DIR/normal.pid"
sh "$CASE_DIR/probe-normal.sh" >"$CASE_DIR/probe-normal.out" 2>&1
NORMAL_RC=$?
obs_kv "normal run exit" "$NORMAL_RC"
assert_eq "$NORMAL_RC" "0" \
	"B without a signal the driver reaches its normal exit status"

say "C: static -- signal traps exit explicitly"
assert_eq "$(count_lines "$RUNALL" "^trap 'exit 143' INT TERM")" "1" \
	"C INT/TERM are trapped with an explicit exit"
assert_eq "$(count_lines "$RUNALL" "^trap 'cleanup' EXIT")" "1" \
	"C EXIT still runs the cleanup"
assert_not_contains "$RUNALL" "EXIT INT TERM" \
	"C the old combined trap that never exited is gone"
assert_contains "$RUNALL" "PD-74: a signal trap" \
	"C the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
