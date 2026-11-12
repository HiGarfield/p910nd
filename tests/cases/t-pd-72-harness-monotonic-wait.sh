#!/bin/sh
#
# PD-72  [high]  Bounded harness waits must not use the wall clock.
#
# wait_for_port() and wait_for_gone() computed their deadline with
# time.time().  A backwards wall-clock step (NTP correction, a VM resuming
# from suspend) made "now < deadline" true for hours, so the harness itself
# wedged: make test and CI hung with no failing assertion.  PD-07 fixed this
# in the daemon and PD-47 in inetd_sim.py; lib.sh was left behind.
#
# sitecustomize.py replaces time.time() so the FIRST call is real (the
# deadline) and every later call is one hour in the past, while
# time.monotonic() is untouched.  Reproduced on the old lib.sh: both waits
# had to be killed by timeout (exit 124).
#
# A: wait_for_port on a closed port still ends with its normal failure.
# B: wait_for_gone on a live listener still ends with its normal failure.
# C: control -- without the clock step both positive paths still succeed.
# D: static -- the waits use time.monotonic() and no time.time() remains.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-72 harness-monotonic-wait

LIB="$TESTS_DIR/lib.sh"
SITE="$CASE_DIR/site"
mkdir -p "$SITE"
cat >"$SITE/sitecustomize.py" <<'EOF'
import time
_real = time.time
_n = [0]
def _stepped():
    _n[0] += 1
    return _real() if _n[0] == 1 else _real() - 3600.0
time.time = _stepped
EOF

free_port() {
	"$PYTHON" -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()'
}

run_wait() {
	_label=$1
	_fn=$2
	_port=$3
	timeout 4 env PYTHONPATH="$SITE" REPO_ROOT="$REPO_ROOT" \
		sh -c ". \"$REPO_ROOT/tests/lib.sh\"; $_fn 127.0.0.1 $_port 0.3" \
		>"$CASE_DIR/$_label.out" 2>&1
	WAIT_RC=$?
	obs "$_label exit: $WAIT_RC"
	obs "$_label output: $(tr '\n' '|' <"$CASE_DIR/$_label.out")"
}

PORT_A=$(free_port)
PORT_B=$(free_port)

# A live listener for wait_for_gone.  It must ACCEPT: wait_for_gone opens a
# connection on every iteration, and a listener that never accepts fills its
# backlog, turning a subsequent connect into a timeout that would be mistaken
# for "the port is gone".  Not started through sitecustomize.
"$PYTHON" - "$PORT_B" >"$CASE_DIR/listener.log" 2>&1 <<'EOF' &
import socket, sys, time
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1])))
s.listen(128)
s.settimeout(0.05)
end = time.monotonic() + 30.0
while time.monotonic() < end:
    try:
        c, _peer = s.accept()
        c.close()
    except socket.timeout:
        pass
EOF
LISTENER_PID=$!
wait_for_port 127.0.0.1 "$PORT_B" 5
assert_eq "$?" "0" "the control listener is up"

say "A: a wall-clock step does not wedge wait_for_port"
run_wait a wait_for_port "$PORT_A"
assert_eq "$WAIT_RC" "1" \
	"A the closed port is reported as a normal failure, not a timeout kill"
assert_lt "$WAIT_RC" "124" \
	"A no timeout(1) rescue was needed"

say "B: a wall-clock step does not wedge wait_for_gone"
run_wait b wait_for_gone "$PORT_B"
assert_eq "$WAIT_RC" "1" \
	"B the live listener is reported as a normal failure"
assert_lt "$WAIT_RC" "124" \
	"B no timeout(1) rescue was needed"

say "C: control -- the positive paths still work without the clock step"
wait_for_port 127.0.0.1 "$PORT_B" 2
assert_eq "$?" "0" "C wait_for_port finds the listener"
wait_for_gone 127.0.0.1 "$PORT_A" 2
assert_eq "$?" "0" "C wait_for_gone sees the closed port"

say "D: static -- every bounded wait uses the monotonic clock"
assert_eq "$(count_lines "$LIB" 'time.monotonic() +')" "2" \
	"D both deadlines are derived from time.monotonic()"
assert_eq "$(count_lines "$LIB" 'while time.monotonic()')" "2" \
	"D and both loops compare against it"
assert_eq "$(count_lines "$LIB" 'time.time()')" "0" \
	"D no wall-clock deadline or loop condition remains"
assert_contains "$LIB" "PD-72: both bounded waits" \
	"D the reason is documented"

kill -TERM "$LISTENER_PID" 2>/dev/null
wait "$LISTENER_PID" 2>/dev/null
kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
