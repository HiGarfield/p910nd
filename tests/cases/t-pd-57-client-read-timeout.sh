#!/bin/sh
#
# PD-57  [medium]  A short --read-timeout must actually be short.
#
# client.py set sock.settimeout(1.0) once and only checked the monotonic
# deadline at the top of the receive loop.  A single recv() therefore
# blocked for a full second even when --read-timeout was 0.2: measured
# elapsed=1.003 (five times the requested limit), and every case using a
# short limit silently waited five times longer than intended.
#
# A: --read-timeout 0.1 against a silent server must end well under 0.6 s.
# B: --read-timeout 1.4 must still be honoured (not clamped to a tiny value).
# C: static -- the socket timeout is derived from the remaining deadline.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-57 client-read-timeout

CLIENT="$TOOLS_DIR/client.py"
SERVER="$CASE_DIR/server.py"
cat >"$SERVER" <<'EOF'
import socket, sys, time
port, hold = int(sys.argv[1]), float(sys.argv[2])
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", port))
s.listen(1)
print("ready", flush=True)
c, _ = s.accept()
time.sleep(hold)
c.close()
s.close()
EOF

free_port() {
	"$PYTHON" -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()'
}

run_client() {
	_label=$1
	_limit=$2
	_port=$(free_port)
	"$PYTHON" "$SERVER" "$_port" 3 >"$CASE_DIR/$_label.server" 2>&1 &
	SRV_PID=$!
	_i=0
	while [ "$_i" -lt 50 ]; do
		grep -q ready "$CASE_DIR/$_label.server" 2>/dev/null && break
		kill -0 "$SRV_PID" 2>/dev/null || break
		sleep 0.1
		_i=$((_i + 1))
	done
	"$PYTHON" "$CLIENT" --port "$_port" --expect-reply 1 \
		--read-timeout "$_limit" --label "$_label" \
		>"$CASE_DIR/$_label.client" 2>&1
	CLIENT_RC=$?
	ELAPSED=$(sed -n 's/.*elapsed=\([0-9.]*\).*/\1/p' \
		"$CASE_DIR/$_label.client")
	obs "$_label client: $(tr '\n' '|' <"$CASE_DIR/$_label.client")"
	obs_kv "$_label exit" "$CLIENT_RC"
	obs_kv "$_label elapsed" "${ELAPSED:-missing}"
	wait "$SRV_PID" 2>/dev/null
}

say "A: a 0.1 s read timeout must not take a full second"
run_client short 0.1
assert_eq "$CLIENT_RC" "1" "A the short read still ends as a timeout failure"
assert_contains "$CASE_DIR/short.client" "RESULT how=timeout" \
	"A and reports the timeout"
assert_f_lt "${ELAPSED:-99}" "0.6" \
	"A elapsed time is close to the requested 0.1 s, not 1.0 s"

say "B: a 1.4 s read timeout is still honoured"
run_client long 1.4
assert_eq "$CLIENT_RC" "1" "B the longer read also ends as a timeout failure"
assert_f_ge "${ELAPSED:-0}" "1.1" \
	"B it is not clamped to a tiny constant"

say "C: static -- the socket timeout follows the deadline"
assert_contains "$CLIENT" "deadline - time.monotonic()" \
	"C the recv timeout is derived from the remaining deadline"
assert_contains "$CLIENT" "sock.settimeout(min(" \
	"C and is capped at one second"
assert_contains "$CLIENT" "the socket timeout follows the deadline" \
	"C the option documents the new behaviour"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
