#!/bin/sh
#
# PD-103  [medium]  A bad simulator port is a harness error, not a job result.
#
# tests/tools/inetd_sim.py took --port as a bare int and called
# listener.bind((host, port)) without validating it.  -1, 65536 and 99999
# raised an uncaught OverflowError: a Python traceback on stderr, NO RESULT
# line on stdout, and exit 1.  PD-85 already classified an unusable --target
# and PD-89 an invalid --send-byte as connect-error:... with exit 2, precisely
# so a harness mistake cannot be mistaken for a failed print job.  Port 0 is
# worse: bind() succeeds on an ephemeral port the client can never reach.
#
# A: out-of-range ports produce a classified RESULT and no traceback.
# B: control -- a real port still drives a normal job.
# C: static -- the guard precedes bind() and is documented.
# D: client.py is intentionally unchanged: for a CLIENT a bad port is a
#    genuine connect error, not a configuration problem.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-103 inetd-sim-bad-port

SIM="$TOOLS_DIR/inetd_sim.py"
CLIENT="$TOOLS_DIR/client.py"
TARGET=/bin/true

say "A: invalid ports are classified, never raised"
for p in -1 0 65536 99999; do
	"$PYTHON" "$SIM" --port "$p" --target "$TARGET" \
		--read-timeout 1 >"$CASE_DIR/bad-$p.out" 2>&1
	_rc=$?
	obs "port $p rc=$_rc output: $(tr '\n' '|' <"$CASE_DIR/bad-$p.out")"
	assert_eq "$_rc" "2" \
		"A port $p exits with the harness-error status"
	assert_contains "$CASE_DIR/bad-$p.out" \
		"RESULT how=connect-error:bad-port:$p" \
		"A port $p is named in the RESULT line"
	assert_not_contains "$CASE_DIR/bad-$p.out" "Traceback" \
		"A port $p produces no Python traceback"
	assert_not_contains "$CASE_DIR/bad-$p.out" "OverflowError" \
		"A port $p produces no uncaught bind exception"
done

say "B: control -- a valid port still works"
PORT=$("$PYTHON" -c 'import socket
s = socket.socket(); s.bind(("127.0.0.1", 0))
print(s.getsockname()[1]); s.close()')
"$PYTHON" "$SIM" --port "$PORT" --target "$TARGET" \
	--read-timeout 3 >"$CASE_DIR/good.out" 2>&1
_good_rc=$?
obs "good port $PORT rc=$_good_rc output: $(tr '\n' '|' <"$CASE_DIR/good.out")"
assert_eq "$_good_rc" "0" \
	"B the control job completes successfully"
assert_contains "$CASE_DIR/good.out" "RESULT how=" \
	"B and emits its normal RESULT line"
assert_not_contains "$CASE_DIR/good.out" "bad-port" \
	"B with no bogus classification"

say "C: static -- the guard runs before bind()"
"$PYTHON" - "$SIM" >"$CASE_DIR/c.out" 2>&1 <<'EOF'
import sys
s = open(sys.argv[1], encoding="utf-8").read()
guard = "if args.port < 1 or args.port > 65535:" in s
classified = "RESULT how=connect-error:bad-port:%d received=0" in s
bind_at = s.find("listener.bind((args.host, args.port))")
guard_at = s.find("if args.port < 1 or args.port > 65535:")
print("guard=%s classified=%s before_bind=%s" %
      (guard, classified, 0 <= guard_at < bind_at))
sys.exit(0 if guard and classified and 0 <= guard_at < bind_at else 1)
EOF
assert_eq "$?" "0" \
	"C the range check and RESULT classification precede bind()"
assert_contains "$CASE_DIR/c.out" \
	"guard=True classified=True before_bind=True" \
	"C all three properties were checked"
assert_contains "$SIM" "PD-103: --port was an unchecked int" \
	"C the reason is documented"

say "D: client.py keeps its own semantics"
"$PYTHON" "$CLIENT" --port 99999 --expect-reply 1 \
	--read-timeout 1 >"$CASE_DIR/client.out" 2>&1
assert_eq "$?" "2" "D a client still reports a connect error with status 2"
assert_contains "$CASE_DIR/client.out" "RESULT how=connect-error" \
	"D and keeps the same RESULT contract"
assert_not_contains "$CLIENT" "bad-port" \
	"D without borrowing the simulator's classification"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
