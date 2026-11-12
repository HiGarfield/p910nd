#!/bin/sh
#
# PD-54  [medium]  A complete reply must not be reported as "how=none".
#
# client.py's receive loop also ends when every --expect-reply byte has
# arrived while the peer still holds the connection open.  In that case
# how was never set: RESULT printed how=none, a termination mode that is
# not in the helper's documented set (fin/rst/eof/complete/no-read/timeout)
# and that no case can assert on.
#
# A: exactly the expected reply, peer still open -> how=complete, exit 0.
# B: a short reply -> how=timeout and (PD-48) a non-zero exit.
# C: a clean FIN -> how=fin, exit 0.
# D: static -- the mode is documented and assigned.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-54 client-complete-mode

CLIENT="$TOOLS_DIR/client.py"
SERVER="$CASE_DIR/server.py"
cat >"$SERVER" <<'EOF'
import socket, sys, time
port, count, hold = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", port))
s.listen(1)
print("ready", flush=True)
c, _ = s.accept()
if count:
    c.sendall(b"X" * count)
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

run_pair() {
	_label=$1
	_nbytes=$2
	_hold=$3
	_port=$(free_port)
	"$PYTHON" "$SERVER" "$_port" "$_nbytes" "$_hold" \
		>"$CASE_DIR/$_label.server" 2>&1 &
	SRV_PID=$!
	_i=0
	while [ "$_i" -lt 50 ]; do
		grep -q ready "$CASE_DIR/$_label.server" 2>/dev/null && break
		kill -0 "$SRV_PID" 2>/dev/null || break
		sleep 0.1
		_i=$((_i + 1))
	done
	"$PYTHON" "$CLIENT" --port "$_port" --expect-reply 8 \
		--read-timeout 1 --label "$_label" \
		>"$CASE_DIR/$_label.client" 2>&1
	CLIENT_RC=$?
	obs "$_label client: $(tr '\n' '|' <"$CASE_DIR/$_label.client")"
	obs_kv "$_label exit" "$CLIENT_RC"
	wait "$SRV_PID" 2>/dev/null
}

say "A: all eight expected bytes arrive, peer stays open"
run_pair complete 8 2
assert_eq "$CLIENT_RC" "0" "A a complete reply is a success"
assert_contains "$CASE_DIR/complete.client" "RESULT how=complete" \
	"A and is reported with a documented termination mode"
assert_not_contains "$CASE_DIR/complete.client" "how=none" \
	"A how=none is gone"

say "B: only four bytes arrive"
run_pair short 4 2
assert_eq "$CLIENT_RC" "1" "B an incomplete reply is still a failure"
assert_contains "$CASE_DIR/short.client" "RESULT how=timeout" \
	"B and is reported as a timeout, not a complete reply"

say "C: clean FIN with no data"
run_pair fin 0 0
assert_eq "$CLIENT_RC" "0" "C a clean FIN is still a success"
assert_contains "$CASE_DIR/fin.client" "RESULT how=fin" \
	"C and still reports fin"

say "D: static -- the new mode is explicit"
assert_contains "$CLIENT" "complete all --expect-reply bytes arrived" \
	"D the helper documents how=complete"
assert_contains "$CLIENT" 'how = ("no-read"' \
	"D and assigns a defined mode when the loop ends"
assert_contains "$CLIENT" "no-read  no reply was requested" \
	"D how=no-read is documented too"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
