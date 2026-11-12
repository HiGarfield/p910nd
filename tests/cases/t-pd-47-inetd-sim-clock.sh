#!/bin/sh
#
# PD-47  [medium]  The inetd simulator must not measure deadlines with the
# wall clock.
#
# tests/tools/inetd_sim.py used time.time() for BOTH its read deadline and
# its child-reap deadline, while the sibling client.py deliberately uses
# time.monotonic().  A backwards clock step (NTP correction, VM resume, or
# the fake-clock shim PD-07 loads) makes "now < deadline" true for hours:
# the helper hangs instead of reporting a timeout, and a case that relies on
# it wedges until the harness kills it.
#
# A: start the helper with a sitecustomize that steps time.time() back an
#    hour after the first two calls.  Both deadlines must still fire.
# B: control -- the same run without the clock step.
# C: static -- no wall-clock deadline expression remains.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-47 inetd-sim-clock

SIM="$TOOLS_DIR/inetd_sim.py"

cat >"$CASE_DIR/sitecustomize.py" <<'EOF'
import time
_real = time.time
_n = [0]
def shifted():
    _n[0] += 1
    return _real() - (3600 if _n[0] >= 3 else 0)
time.time = shifted
EOF

# The fake inetd target keeps the accepted socket open and never replies, so
# the read deadline is what ends the wait.  It has no child processes and
# lives under build/tests so kill_stray() can clean up a leftover.
cat >"$CASE_DIR/hold.py" <<'EOF'
#!/usr/bin/env python3
import time
time.sleep(30)
EOF
chmod +x "$CASE_DIR/hold.py"

free_port() {
	"$PYTHON" -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()'
}

say "A: wall clock stepped back one hour"
# PYTHONPATH is set only for A; sitecustomize.py is loaded at interpreter
# start-up, so B runs the same helper with a normal clock.
A_PORT=$(free_port)
timeout 20 env PYTHONPATH="$CASE_DIR" "$PYTHON" "$SIM" \
	--port "$A_PORT" --target "$CASE_DIR/hold.py" \
	--expect-reply 1 --read-timeout 1 >"$CASE_DIR/a.out" 2>&1
a_rc=$?
obs "A exit status: $a_rc"
obs "A output: $(tr '\n' '|' <"$CASE_DIR/a.out")"
# PD-64: the probe deliberately times out, and inetd_sim.py now reports an
# unconfirmed job with exit 1.  What matters here is that it exits on its
# own (not 124 from `timeout`), so 1 is the expected healthy result.
assert_eq "$a_rc" "1" \
	"A the helper exits on its own despite the wall clock step"
assert_contains "$CASE_DIR/a.out" "RESULT how=timeout" \
	"A the read deadline still fires and is reported"
assert_contains "$CASE_DIR/a.out" "exit=killed-after-timeout" \
	"A the child-reap deadline still fires"
kill_stray

say "B: control -- no clock step"
B_PORT=$(free_port)
timeout 20 "$PYTHON" "$SIM" \
	--port "$B_PORT" --target "$CASE_DIR/hold.py" \
	--expect-reply 1 --read-timeout 1 >"$CASE_DIR/b.out" 2>&1
b_rc=$?
obs "B exit status: $b_rc"
obs "B output: $(tr '\n' '|' <"$CASE_DIR/b.out")"
assert_eq "$b_rc" "1" \
	"B the control also reports its expected timeout as exit 1 (PD-64)"
assert_contains "$CASE_DIR/b.out" "RESULT how=timeout" \
	"B the same timeout is reported"
kill_stray

say "C: static -- both deadlines use the monotonic clock"
assert_eq "$(count_lines "$SIM" 'deadline = time.time()')" "0" \
	"C no wall-clock deadline expression remains"
# PD-70 derives the read deadline from read_started = time.monotonic(), so
# only the child-reap deadline is spelled as "deadline = time.monotonic()".
assert_eq "$(count_lines "$SIM" 'deadline = time.monotonic()')" "1" \
	"C the child-reap deadline still uses the monotonic clock"
assert_contains "$SIM" 'read_started = time.monotonic()' \
	"C and the read deadline has a monotonic origin"
assert_contains "$SIM" 'deadline = read_started + args.read_timeout' \
	"C derived from that origin, never from time.time()"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
