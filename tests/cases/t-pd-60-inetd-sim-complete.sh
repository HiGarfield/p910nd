#!/bin/sh
#
# PD-60  [medium]  A complete reply is not a timeout.
#
# tests/tools/inetd_sim.py's receive loop ends when either the monotonic
# deadline expires OR len(got) reaches --expect-reply.  The latter left how
# as "open", which the code then blindly rewrote to "timeout": measured
# "RESULT how=timeout received=8" for a perfect 8-byte reply.  Cases that
# assert on how= (PD-37 checks it is not timeout) can fail on a healthy job.
# client.py fixed the same class of bug in PD-54.
#
# A: exactly the expected reply, peer still open -> how=complete.
# B: a short reply -> how=timeout.
# C: a clean FIN -> how=fin.
# D: no reply requested -> how=no-read.
# E: static -- the modes are documented and assigned.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-60 inetd-sim-complete

SIM="$TOOLS_DIR/inetd_sim.py"
SVC="$CASE_DIR/svc.py"
cat >"$SVC" <<'EOF'
#!/usr/bin/env python3
import os, sys, time
count, hold = int(sys.argv[1]), float(sys.argv[2])
if count:
    os.write(1, b"X" * count)
time.sleep(hold)
EOF
chmod +x "$SVC"

free_port() {
	"$PYTHON" -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()'
}

run_sim() {
	_label=$1
	_expect=$2
	_nbytes=$3
	_hold=$4
	_port=$(free_port)
	timeout 15 "$PYTHON" "$SIM" --port "$_port" --target "$SVC" \
		--expect-reply "$_expect" --read-timeout 1 \
		-- "$_nbytes" "$_hold" >"$CASE_DIR/$_label.out" 2>&1
	SIM_RC=$?
	obs "$_label output: $(tr '\n' '|' <"$CASE_DIR/$_label.out")"
	obs_kv "$_label exit" "$SIM_RC"
}

say "A: all eight expected bytes arrive, peer stays open"
run_sim complete 8 8 1
assert_eq "$SIM_RC" "0" "A the simulator exits normally"
assert_contains "$CASE_DIR/complete.out" "RESULT how=complete" \
	"A a complete reply is reported as complete"
assert_contains "$CASE_DIR/complete.out" "received=8" \
	"A and all eight bytes are accounted for"
assert_not_contains "$CASE_DIR/complete.out" "how=timeout" \
	"A it is no longer misreported as a timeout"

say "B: only four bytes arrive"
run_sim short 8 4 1
assert_contains "$CASE_DIR/short.out" "RESULT how=timeout" \
	"B a genuinely incomplete reply is still a timeout"

say "C: clean FIN with no data"
run_sim fin 8 0 0
assert_contains "$CASE_DIR/fin.out" "RESULT how=fin" \
	"C a clean FIN is still reported as fin"

say "D: no reply bytes requested"
run_sim noread 0 0 0
assert_contains "$CASE_DIR/noread.out" "RESULT how=no-read" \
	"D --expect-reply 0 is reported explicitly, not as timeout"

say "E: static -- the new modes are explicit"
assert_contains "$SIM" "complete (every expected byte arrived" \
	"E the helper documents how=complete"
assert_contains "$SIM" 'how = "complete"' \
	"E and assigns it"
assert_contains "$SIM" 'how = "no-read"' \
	"E how=no-read is assigned too"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
