#!/bin/sh
#
# PD-89  [medium]  inetd_sim.py must classify a bad --send-byte too.
#
# PD-88 guarded client.py's argument parsing, but inetd_sim.py still did
# `send_byte = int(args.send_byte, 0) & 0xFF` with no handler.  A
# non-numeric value raised ValueError before any socket work: stdout was
# empty (no RESULT line), stderr had a traceback, and exit 1 was
# indistinguishable from PD-64's "job not confirmed".  12 cases use this
# simulator, so a typo in a test fixture silently produced a job-failure
# shape instead of a clear tooling error.
#
# A: an invalid byte is reported with a RESULT line and status 2.
# B: control -- valid decimal, hex and out-of-range-masked values work.
# C: static -- both byte parsers are guarded.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-89 inetd-sim-bad-byte

SIM="$TOOLS_DIR/inetd_sim.py"

next_port() {
	"$PYTHON" -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()'
}

run_sim() {
	_label=$1
	_byte=$2
	_port=$(next_port)
	"$PYTHON" "$SIM" --port "$_port" --target /bin/true \
		--send-repeat 1 --send-byte "$_byte" \
		--expect-reply 0 --read-timeout 1 \
		>"$CASE_DIR/$_label.out" 2>"$CASE_DIR/$_label.err"
	SIM_RC=$?
	obs "$_label exit: $SIM_RC"
	obs "$_label stdout: $(tr '\n' '|' <"$CASE_DIR/$_label.out")"
	obs "$_label stderr: $(tr '\n' '|' <"$CASE_DIR/$_label.err")"
}

say "A: a non-numeric --send-byte is a tooling error"
run_sim bad notanumber
assert_eq "$SIM_RC" "2" \
	"A the tooling error gets its own status"
assert_contains "$CASE_DIR/bad.out" \
	"RESULT how=connect-error:bad-send-byte" \
	"A a machine-readable RESULT is still printed"
assert_contains "$CASE_DIR/bad.out" "bad --send-byte value" \
	"A the cause is named"
assert_not_contains "$CASE_DIR/bad.err" "Traceback" \
	"A no uncaught exception escapes"

say "B: control -- valid byte spellings still work"
run_sim hex 0x41
assert_eq "$SIM_RC" "0" \
	"B a hex byte still gives a successful result"
assert_contains "$CASE_DIR/hex.out" "client_sent=1" \
	"B and the byte is actually sent"
assert_not_contains "$CASE_DIR/hex.out" "bad-send-byte" \
	"B with no false argument error"
assert_not_contains "$CASE_DIR/hex.err" "Traceback" \
	"B no traceback on the good path"

run_sim masked 999
assert_eq "$SIM_RC" "0" \
	"B an out-of-range value is still masked to one byte"
assert_contains "$CASE_DIR/masked.out" "client_sent=1" \
	"B and one byte is sent"

say "C: static -- both byte parsers are guarded"
"$PYTHON" - "$SIM" "$TOOLS_DIR/client.py" >"$CASE_DIR/c.out" 2>&1 <<'EOF'
import sys
sim = open(sys.argv[1], encoding="utf-8").read()
cli = open(sys.argv[2], encoding="utf-8").read()
sim_guard = "except ValueError as exc" in sim and \
            "RESULT how=connect-error:bad-send-byte" in sim
cli_guard = "ValueError" in cli and "bad-send" in cli
print("sim_guard=%s client_guard=%s" % (sim_guard, cli_guard))
sys.exit(0 if sim_guard and cli_guard else 1)
EOF
assert_eq "$?" "0" \
	"C client.py and inetd_sim.py both classify bad byte arguments"
assert_contains "$CASE_DIR/c.out" "sim_guard=True client_guard=True" \
	"C both tools were actually checked"
assert_contains "$SIM" "PD-89: client.py got this guard in PD-88" \
	"C the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
