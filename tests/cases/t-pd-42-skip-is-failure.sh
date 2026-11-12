#!/bin/sh
#
# PD-42  [high]  A skipped case must fail the suite.
#
# run-all.sh checks that TCP TEST_PORT is free before every case.  When it
# is not (a stray daemon, a leftover listener from a broken run, a service
# on a shared CI worker) the case is marked "skipped" and the driver still
# exited 0.  `make test` and CI then reported a green suite in which not a
# single assertion had executed -- the exact vacuous-green failure PD-99
# guards against for missing case files.
#
# The case holds the port with a socket that does NOT set SO_REUSEADDR (so
# lib.sh's port_is_free() really sees it as busy), runs a nested driver for
# one case, and requires a non-zero exit plus an explicit failure message.
# A control run after releasing the port proves the port really was the
# cause and that the driver still accepts a healthy environment.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-42 skip-is-failure

DRIVER="$TESTS_DIR/run-all.sh"
HOLDER_LOG="$CASE_DIR/holder.log"

hold_port() {
	"$PYTHON" -c '
import socket, sys, time
s = socket.socket()
s.bind(("127.0.0.1", int(sys.argv[1])))
s.listen(1)
print("ready", flush=True)
time.sleep(120)
' "$1" >"$HOLDER_LOG" 2>&1 &
	HOLDER_PID=$!
	_n=0
	while [ "$_n" -lt 50 ]; do
		grep -q ready "$HOLDER_LOG" 2>/dev/null && return 0
		kill -0 "$HOLDER_PID" 2>/dev/null || return 1
		sleep 0.1
		_n=$((_n + 1))
	done
	return 1
}

release_port() {
	[ -n "${HOLDER_PID:-}" ] || return 0
	kill -TERM "$HOLDER_PID" 2>/dev/null
	wait "$HOLDER_PID" 2>/dev/null
	HOLDER_PID=
	_n=0
	while [ "$_n" -lt 50 ]; do
		port_is_free "$TEST_PORT" && return 0
		sleep 0.1
		_n=$((_n + 1))
	done
	return 1
}

say "A: hold TCP $TEST_PORT and run a nested one-case driver"
HOLDER_PID=
if ! hold_port "$TEST_PORT"; then
	obs "holder could not bind: $(tr '\n' '|' <"$HOLDER_LOG")"
	assert_true 1 "the port holder starts"
	kill_stray
	[ "$ASSERT_FAIL" -eq 0 ] || exit 1
	exit 0
fi
obs "holder pid: $HOLDER_PID"
# port_is_free() returns 0 when free; here it must return 1.
port_is_free "$TEST_PORT" >/dev/null 2>&1
busy_rc=$?
obs_kv "port_is_free while held (exit)" "$busy_rc"
assert_eq "$busy_rc" "1" "the harness really sees the port as busy"

TEST_PORT="$TEST_PORT" sh "$DRIVER" PD-01 >"$CASE_DIR/nested-busy.log" 2>&1
nested_rc=$?
obs "nested driver exit: $nested_rc"
obs "nested driver output:"
sed 's/^/    /' "$CASE_DIR/nested-busy.log" | tail -12 |
	while read -r l; do obs "$l"; done
assert_eq "$nested_rc" "1" \
	"a suite with skipped cases exits non-zero instead of passing"
assert_contains "$CASE_DIR/nested-busy.log" "skipped cases ran no assertions" \
	"and says why, so the failure is not a mystery exit code"
assert_contains "$CASE_DIR/nested-busy.log" "SKIPPED, TCP $TEST_PORT" \
	"the skipped case is still reported"
assert_not_contains "$CASE_DIR/nested-busy.log" "PASS pd-01" \
	"the skipped case is not reported as passed"

say "B: control -- release the port, the same driver runs the case"
if ! release_port; then
	obs "port did not become free again"
	assert_true 1 "the port is free after releasing the holder"
fi
TEST_PORT="$TEST_PORT" sh "$DRIVER" PD-01 >"$CASE_DIR/nested-free.log" 2>&1
control_rc=$?
obs_kv "control driver exit" "$control_rc"
assert_eq "$control_rc" "0" \
	"with the port free the same driver runs the case and passes"
assert_contains "$CASE_DIR/nested-free.log" "PASS pd-01" \
	"the control run really executed the case"

release_port
kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
