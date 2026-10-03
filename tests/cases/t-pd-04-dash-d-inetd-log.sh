#!/bin/sh
#
# PD-04  [高]  -d must not change what the process does, and must not write the
# log into the client's byte stream.
#
# (x)inetd hands the accepted socket to the service on fd 0, 1 and 2.  Two bugs
# grew out of that: `log_to_stdout ||` in main() made -d skip one_job() and open
# a listening socket instead, so the job was never served; and dolog() wrote to
# stdout, which *is* the client socket, so log lines were injected into the print
# stream.  The log now goes to syslog whenever fd 1 is the client's socket, and
# -d no longer influences the daemon/one-shot decision at all.
#
# A: the handed-over connection is served
# B: no log text reaches the client
# C: without -d nothing changes (control)

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-04 dash-d-inetd-log
# test-lockdir, not test: one_job() takes the instance lock unconditionally, and
# a non-root run needs a writable lock directory.  Every part uses the same
# binary and differs only in -d.
CASE_BIN="$BUILD_DIR/test-lockdir"
assert_file_exists "$CASE_BIN" "the instrumented variant builds"

run_inetd() {
	# run_inetd <label> <device> [extra inetd_sim args...]
	_label=$1
	_dev=$2
	shift 2
	: >"$_dev"
	"$PYTHON" "$TOOLS_DIR/inetd_sim.py" --port 19199 --target "$CASE_BIN" \
		--send-repeat 256 --send-byte 0x50 --half-close --read-timeout 4 \
		--device "$_dev" --preview 300 "$@" \
		>"$CASE_DIR/$_label.out" 2>&1
	sed 's/^/    /' "$CASE_DIR/$_label.out" | while read -r l; do obs "$l"; done
	eval "RECV_$_label=\$(sed -n 's/^client_received=\\([0-9]*\\).*/\\1/p' "$CASE_DIR/$_label.out")"
	eval "DEVB_$_label=\$(sed -n 's/^device_bytes=\\([0-9]*\\).*/\\1/p' "$CASE_DIR/$_label.out")"
	eval "HOW_$_label=\$(sed -n 's/^RESULT how=\\([a-z-]*\\).*/\\1/p' "$CASE_DIR/$_label.out")"
}

# ---------------------------------------------------------------- part A ----
say "A: -d under inetd -- the handed-over connection is served"
run_inetd a "$CASE_DIR/lp0" -- -d -i 127.0.0.1 -f "$CASE_DIR/lp0" 0
obs_kv "A device bytes" "${DEVB_a:-?}"
obs_kv "A how the client ended" "${HOW_a:-?}"
assert_eq "${DEVB_a:-x}" "256" "A the job reaches the printer"
assert_eq "${HOW_a:-x}" "fin" "A and the client is told the job finished"

# ---------------------------------------------------------------- part B ----
say "B: no log line is injected into the client's stream"
run_inetd b "$CASE_DIR/lp0-b" --extra-connect "$TEST_PORT" --extra-delay 0.6 \
	-- -d -i 127.0.0.1 -f /dev/null 0
obs_kv "B bytes the first client received" "${RECV_b:-?}"
assert_eq "${RECV_b:-x}" "0" "B the first client receives nothing but its own job"
assert_not_contains "$CASE_DIR/b.out" "client_received_preview=Connection from" \
	"B no p910nd log line is visible to the client"

# The log must have gone somewhere: it is on syslog now, which the case cannot
# read, so assert the negative that matters -- nothing on stdout.
say "B': with -d and fd 1 being the client socket, stdout carries no log"
: >"$CASE_DIR/lp0-c"
"$PYTHON" "$TOOLS_DIR/inetd_sim.py" --port 19199 --target "$CASE_BIN" \
	--send-repeat 16 --half-close --read-timeout 3 --device "$CASE_DIR/lp0-c" \
	-- -d -i 127.0.0.1 -f "$CASE_DIR/lp0-c" 0 >"$CASE_DIR/c.out" 2>&1
grep -E '^client_received=' "$CASE_DIR/c.out" | sed 's/^/    /' | while read -r l; do obs "$l"; done
assert_contains "$CASE_DIR/c.out" "client_received=0" "B' stdout was not used for logging"

# ---------------------------------------------------------------- part C ----
say "C: control, the same invocation without -d"
run_inetd d "$CASE_DIR/lp0-d" -- -i 127.0.0.1 -f "$CASE_DIR/lp0-d" 0
obs_kv "C device bytes" "${DEVB_d:-?}"
assert_eq "${DEVB_d:-x}" "256" "C without -d the job still reaches the printer"

say "D: -d in the foreground still logs to stdout (nothing regressed)"
daemon_start "$CASE_DIR/fg.log" "$CASE_BIN" -d -i 127.0.0.1 -f "$CASE_DIR/lp0-e" 0
wait_for_port 127.0.0.1 "$TEST_PORT" 5
"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" --send-repeat 8 \
	--half-close --expect-reply 1 --read-timeout 10 --label FG \
	>"$CASE_DIR/fg.client" 2>&1
daemon_stop
obs "D foreground log:"
sed 's/^/    /' "$CASE_DIR/fg.log" | while read -r l; do obs "$l"; done
assert_contains "$CASE_DIR/fg.log" "Connection from" \
	"D with -d in the foreground the log still goes to stdout"

sleep 0.5
if port_is_free "$TEST_PORT"; then
	obs "test port $TEST_PORT is free again"
	assert_true 0 "no daemon is left holding the test port"
else
	assert_true 1 "no daemon is left holding the test port"
fi

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
