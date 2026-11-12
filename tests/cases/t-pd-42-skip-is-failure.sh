#!/bin/sh
#
# PD-42  [high]  A skipped case must fail the suite.
#
# run-all.sh checks that TCP TEST_PORT is free before every case.  When it
# is not (a stray daemon, a leftover listener from a broken run, a service
# on a shared CI worker) the case is marked "skipped" and the driver used to
# exit 0.  `make test` and CI then reported a green suite in which not a
# single assertion had executed -- the exact vacuous-green failure PD-99
# guards against for missing case files.
#
# The case holds a private port with a socket that does NOT set SO_REUSEADDR
# (so lib.sh's port_is_free() really sees it as busy), runs a nested driver
# for one synthetic case, and requires a non-zero exit plus an explicit
# failure message.  A control run after releasing the port proves the port
# really was the cause and that the driver still accepts a healthy
# environment.
#
# Like PD-77/91/104, the nested drivers run in an isolated copy of the tree.
# Running them in the live tree made them rebuild the shared variants and
# let their kill_stray() sweep reach this case's own helper processes, so a
# loaded CI runner failed PD-42 even though the driver logic was correct.
# The synthetic case also removes the dependency on PD-01's strace, lock
# directory permissions and daemon start-up: none of those is what this
# case asserts.
#
# case-timeout: 150
# Two nested drivers rebuild all four variants; 45 s is not enough on a
# loaded runner (PD-58).

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-42 skip-is-failure

DRIVER="$TESTS_DIR/run-all.sh"
TREE="$CASE_DIR/tree"
PORT=$((TEST_PORT + 42))
PROBE_MARKER="$CASE_DIR/probe.marker"
HOLDER_LOG="$CASE_DIR/holder.log"

# Only the files the nested driver needs; no git metadata required.
mkdir -p "$TREE/tests/cases" "$TREE/tests/tools" || exit 1
cp "$REPO_ROOT/p910nd.c" "$REPO_ROOT/Makefile" "$TREE/" &&
	cp "$DRIVER" "$TESTS_DIR/lib.sh" "$TESTS_DIR/variants.sh" \
		"$TREE/tests/" &&
	cp "$TOOLS_DIR/patch_source.py" "$TOOLS_DIR/faketime_preload.c" \
		"$TREE/tests/tools/" || {
	obs "FATAL: could not create the isolated checkout"
	assert_true 1 "the isolated checkout is created"
	kill_stray
	exit 1
}

# One synthetic case: PD-42 asserts the driver's skip policy, not the
# behaviour of a real regression case.  The marker proves the control run
# executed the case body instead of merely printing PASS.
cat >"$TREE/tests/cases/t-pd-42-probe.sh" <<EOF
#!/bin/sh
printf '%s\n' ran > "$PROBE_MARKER"
echo "PD-42 probe case ran"
exit 0
EOF
chmod +x "$TREE/tests/cases/t-pd-42-probe.sh"

run_nested() {
	# run_nested <label>
	(cd "$TREE" && TEST_PORT="$PORT" sh tests/run-all.sh PD-42) \
		>"$CASE_DIR/$1.log" 2>&1
	NESTED_RC=$?
	obs "nested driver ($1) exit: $NESTED_RC"
	obs "nested driver ($1) output:"
	sed 's/^/    /' "$CASE_DIR/$1.log" | tail -12 |
		while read -r l; do obs "$l"; done
}

hold_port() {
	"$PYTHON" -c '
import socket, sys, time
s = socket.socket()
s.bind(("127.0.0.1", int(sys.argv[1])))
s.listen(1)
print("ready", flush=True)
time.sleep(300)
' "$PORT" >"$HOLDER_LOG" 2>&1 &
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
		port_is_free "$PORT" && return 0
		sleep 0.1
		_n=$((_n + 1))
	done
	return 1
}

say "A: hold TCP $PORT and run a nested one-case driver"
HOLDER_PID=
if ! hold_port; then
	obs "holder could not bind: $(tr '\n' '|' <"$HOLDER_LOG")"
	assert_true 1 "the port holder starts"
	kill_stray
	exit 1
fi
obs "holder pid: $HOLDER_PID"
port_is_free "$PORT" >/dev/null 2>&1
busy_rc=$?
obs_kv "port_is_free while held (exit)" "$busy_rc"
assert_eq "$busy_rc" "1" "the harness really sees the port as busy"

run_nested busy
assert_eq "$NESTED_RC" "1" \
	"a suite with skipped cases exits non-zero instead of passing"
assert_contains "$CASE_DIR/busy.log" "skipped cases ran no assertions" \
	"and says why, so the failure is not a mystery exit code"
assert_contains "$CASE_DIR/busy.log" "SKIPPED, TCP $PORT" \
	"the skipped case is still reported"
assert_not_contains "$CASE_DIR/busy.log" "PASS pd-42" \
	"the skipped case is not reported as passed"
kill -0 "$HOLDER_PID" 2>/dev/null
holder_after=$?
obs_kv "holder alive after the busy run (exit)" "$holder_after"
assert_eq "$holder_after" "0" \
	"the nested driver did not reap this case's port holder"

say "B: control -- release the port, the same driver runs the case"
if ! release_port; then
	obs "port did not become free again"
	assert_true 1 "the port is free after releasing the holder"
fi
rm -f "$PROBE_MARKER"
run_nested free
assert_eq "$NESTED_RC" "0" \
	"with the port free the same driver runs the case and passes"
assert_contains "$CASE_DIR/free.log" "PASS pd-42" \
	"the control run really executed the case"
assert_file_exists "$PROBE_MARKER" \
	"the control case body ran, not just the driver's summary"

release_port
kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
