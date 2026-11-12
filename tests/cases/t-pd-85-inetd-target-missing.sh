#!/bin/sh
#
# PD-85  [high]  inetd_sim.py must not report success when the service never
# started.
#
# With a missing --target the forked child failed os.execv(), wrote
# "exec failed: ..." to fd 2 -- which under inetd IS the client socket --
# and exited 127.  Measured:
#   client_received=49, preview="exec failed: ...",
#   RESULT how=complete received=49 exit=127, helper exit 0.
# A case asking for a reply therefore saw a fabricated reply and a green
# result for a service that never ran (PD-66 fixed the same injection in the
# daemon, but the harness still did it).
#
# A: a missing target is rejected before forking, with status 2.
# B: a target that passes the check but fails execv (bad shebang) still
#    leaves the client stream clean, reports exit=127 and fails with 1.
# C: control -- a real target still reports a clean FIN and status 0.
# D: static -- pre-check, real-stderr diagnostics and the 127 verdict.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-85 inetd-target-missing

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
	_target=$2
	_port=$(next_port)
	"$PYTHON" "$SIM" --port "$_port" --target "$_target" \
		--expect-reply 8 --read-timeout 1 \
		>"$CASE_DIR/$_label.out" 2>"$CASE_DIR/$_label.err"
	SIM_RC=$?
	obs "$_label exit: $SIM_RC"
	obs "$_label stdout: $(tr '\n' '|' <"$CASE_DIR/$_label.out")"
	obs "$_label stderr: $(tr '\n' '|' <"$CASE_DIR/$_label.err")"
}

say "A: a target that does not exist is a harness error, not a job result"
run_sim missing "$CASE_DIR/no-such-p910nd"
assert_eq "$SIM_RC" "2" \
	"A the helper fails with the configuration-error status"
assert_contains "$CASE_DIR/missing.out" \
	"RESULT how=connect-error:target-not-executable" \
	"A the RESULT line names the real cause"
assert_contains "$CASE_DIR/missing.out" "received=0" \
	"A no fabricated reply bytes were delivered"
assert_not_contains "$CASE_DIR/missing.out" "exec failed" \
	"A no diagnostic was injected into the client stream"

say "B: execv still fails after the check (bad shebang)"
printf '#!/nonexistent/interpreter\n' >"$CASE_DIR/badsh"
chmod +x "$CASE_DIR/badsh"
run_sim badexec "$CASE_DIR/badsh"
assert_eq "$SIM_RC" "1" \
	"B exit=127 is a failed job, not a green result"
assert_contains "$CASE_DIR/badexec.out" "exit=127" \
	"B the child's exec-failure status is reported"
assert_contains "$CASE_DIR/badexec.out" "client_received=0" \
	"B the client received no bytes"
assert_not_contains "$CASE_DIR/badexec.out" "exec failed" \
	"B the diagnostic did not reach the client socket"
assert_contains "$CASE_DIR/badexec.err" "exec failed" \
	"B the diagnostic went to the real stderr instead"

say "C: control -- a working target still succeeds"
run_sim healthy /bin/true
assert_eq "$SIM_RC" "0" \
	"C a real executable still gives a successful result"
assert_not_contains "$CASE_DIR/healthy.out" "exec failed" \
	"C with no spurious diagnostic"
assert_not_contains "$CASE_DIR/healthy.out" "target-not-executable" \
	"C and no false configuration error"

say "D: static -- all three defences are in place"
"$PYTHON" - "$SIM" >"$CASE_DIR/d.out" 2>&1 <<'EOF'
import sys
s = open(sys.argv[1], encoding="utf-8").read()
precheck = "os.path.isfile(args.target) and os.access(args.target, os.X_OK)" in s
stderr = "saved_stderr = os.dup(2)" in s and \
         'os.write(saved_stderr, ("exec failed' in s
verdict = "if status == 127:" in s
no_inject = "os.write(2, (\"exec failed" not in s
print("precheck=%s real_stderr=%s verdict=%s no_inject=%s" %
      (precheck, stderr, verdict, no_inject))
sys.exit(0 if precheck and stderr and verdict and no_inject else 1)
EOF
assert_eq "$?" "0" \
	"D the pre-check, stderr routing and 127 verdict are all present"
assert_contains "$CASE_DIR/d.out" "precheck=True real_stderr=True verdict=True no_inject=True" \
	"D every property was actually checked"
assert_contains "$SIM" "PD-85: an unusable --target" \
	"D the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
