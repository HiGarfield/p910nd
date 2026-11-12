#!/bin/sh
#
# PD-70  [medium]  inetd_sim.py must honour short read deadlines.
#
# The helper set client.settimeout(0.5) once and checked the monotonic
# deadline only at the top of the receive loop, so --read-timeout 0.1 still
# blocked for 0.5 s (client.py fixed the same class in PD-57).  The RESULT
# line had no elapsed field, so the overshoot was invisible in observations.
#
# A: 0.1 s request ends in under 0.4 s and reports timeout.
# B: 1.2 s request is still honoured (not clamped).
# C: a complete reply keeps how=complete and exit 0.
# D: static -- the timeout follows the deadline and the elapsed is logged.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-70 inetd-sim-read-timeout

SIM="$TOOLS_DIR/inetd_sim.py"
cat >"$CASE_DIR/hold.py" <<'EOF'
#!/usr/bin/env python3
import time
time.sleep(2)
EOF
cat >"$CASE_DIR/reply.py" <<'EOF'
#!/usr/bin/env python3
import os, time
os.write(1, b"12345678")
time.sleep(0.2)
EOF
chmod +x "$CASE_DIR"/*.py

free_port() {
	"$PYTHON" -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()'
}

run_sim() {
	_label=$1
	_target=$2
	_expect=$3
	_limit=$4
	_port=$(free_port)
	timeout 15 "$PYTHON" "$SIM" --port "$_port" \
		--target "$CASE_DIR/$_target" --expect-reply "$_expect" \
		--read-timeout "$_limit" >"$CASE_DIR/$_label.out" 2>&1
	SIM_RC=$?
	ELAPSED=$(sed -n 's/.*elapsed=\([0-9.]*\).*/\1/p' "$CASE_DIR/$_label.out")
	obs "$_label output: $(tr '\n' '|' <"$CASE_DIR/$_label.out")"
	obs_kv "$_label exit" "$SIM_RC"
	obs_kv "$_label elapsed" "${ELAPSED:-missing}"
}

say "A: a 0.1 s deadline must not take half a second"
run_sim short hold.py 1 0.1
assert_eq "$SIM_RC" "1" "A an unconfirmed job is still a failure (PD-64)"
assert_contains "$CASE_DIR/short.out" "RESULT how=timeout" \
	"A and reports the timeout"
assert_f_lt "${ELAPSED:-99}" "0.4" \
	"A the read phase ends near 0.1 s, not 0.5 s"

say "B: a 1.2 s deadline is still honoured"
run_sim long hold.py 1 1.2
assert_f_ge "${ELAPSED:-0}" "1.0" \
	"B the deadline is not clamped to a tiny constant"

say "C: a complete reply is unaffected"
run_sim complete reply.py 8 1
assert_eq "$SIM_RC" "0" "C a complete reply still succeeds"
assert_contains "$CASE_DIR/complete.out" "RESULT how=complete" \
	"C and still reports how=complete"

say "D: static -- the deadline drives the socket timeout"
assert_contains "$SIM" "deadline - time.monotonic()" \
	"D recv timeout is derived from the remaining deadline"
assert_contains "$SIM" "client.settimeout(min(" \
	"D and capped at half a second"
assert_contains "$SIM" "elapsed=%.3f" \
	"D the RESULT line exposes the measured read duration"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
