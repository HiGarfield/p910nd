#!/bin/sh
#
# PD-48  [medium]  A client read timeout is not a successful job.
#
# tests/tools/client.py is the helper every case uses to observe how a job
# ended.  It returned exit 1 only for rst/connect-error; how=timeout and
# how=oserror:* both returned 0.  A timeout means the daemon never
# confirmed completion (no FIN, no reply), so a case that checks only the
# helper's exit status could pass on a job that never finished -- the same
# vacuous-green class as PD-42.
#
# A: a server that accepts and never replies must produce a non-zero exit.
# B: control -- a server that closes cleanly still produces exit 0.
# C: static -- timeout and OS errors are in the failure classification.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-48 client-timeout-status

CLIENT="$TOOLS_DIR/client.py"
SRV_PID=

free_port() {
	"$PYTHON" -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()'
}

start_server() {
	_mode=$1
	_port=$2
	"$PYTHON" -c '
import socket, sys, time
mode, port = sys.argv[1], int(sys.argv[2])
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", port))
s.listen(1)
print("ready", flush=True)
c, _ = s.accept()
if mode == "hold":
    time.sleep(15)
else:
    c.close()
s.close()
' "$_mode" "$_port" >"$CASE_DIR/$_mode.server.log" 2>&1 &
	SRV_PID=$!
	_n=0
	while [ "$_n" -lt 50 ]; do
		grep -q ready "$CASE_DIR/$_mode.server.log" 2>/dev/null && return 0
		kill -0 "$SRV_PID" 2>/dev/null || return 1
		sleep 0.1
		_n=$((_n + 1))
	done
	return 1
}

stop_server() {
	[ -n "${SRV_PID:-}" ] || return 0
	kill -TERM "$SRV_PID" 2>/dev/null
	wait "$SRV_PID" 2>/dev/null
	SRV_PID=
}

say "A: server accepts and never replies"
A_PORT=$(free_port)
if ! start_server hold "$A_PORT"; then
	obs "server failed: $(tr '\n' '|' <"$CASE_DIR/hold.server.log")"
	assert_true 1 "the silent test server starts"
	kill_stray
	[ "$ASSERT_FAIL" -eq 0 ] || exit 1
	exit 0
fi
"$PYTHON" "$CLIENT" --port "$A_PORT" --expect-reply 1 \
	--read-timeout 1 --label SILENT >"$CASE_DIR/hold.client" 2>&1
hold_rc=$?
obs "A client: $(tr '\n' '|' <"$CASE_DIR/hold.client")"
obs_kv "A client exit" "$hold_rc"
assert_eq "$hold_rc" "1" \
	"A an unconfirmed job (read timeout) exits non-zero"
assert_contains "$CASE_DIR/hold.client" "RESULT how=timeout" \
	"A the timeout is still reported in the observation line"
stop_server

say "B: control -- clean FIN is a success"
B_PORT=$(free_port)
if ! start_server close "$B_PORT"; then
	obs "server failed: $(tr '\n' '|' <"$CASE_DIR/close.server.log")"
	assert_true 1 "the clean-close test server starts"
else
	"$PYTHON" "$CLIENT" --port "$B_PORT" --expect-reply 1 \
		--read-timeout 3 --label FIN >"$CASE_DIR/fin.client" 2>&1
	fin_rc=$?
	obs "B client: $(tr '\n' '|' <"$CASE_DIR/fin.client")"
	obs_kv "B client exit" "$fin_rc"
	assert_eq "$fin_rc" "0" "B a clean FIN still exits zero"
	assert_contains "$CASE_DIR/fin.client" "RESULT how=fin" \
		"B and is still reported as a clean end of job"
fi
stop_server

say "C: static -- the failure classification is complete"
assert_contains "$CLIENT" '"timeout"' \
	"C a read timeout is classified as failure"
assert_contains "$CLIENT" 'how.startswith("oserror:")' \
	"C OS errors are classified as failure too"
assert_contains "$CLIENT" '"connect-error"' \
	"C the pre-existing failures were not lost"

stop_server
kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
