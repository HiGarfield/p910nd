#!/bin/sh
#
# PD-51  [medium]  The pty test device must close on a monotonic deadline.
#
# tests/tools/devnode.py timed --close-after / --hold-after with time.time()
# and measured drain durations with the wall clock.  A backwards clock step
# (NTP, VM resume, the fake-clock shim PD-07 loads) makes "now < deadline"
# true for hours: the pty master is never closed, so read() on the slave
# never fails with EIO and the bidirectional error cases can hang.  Drain
# event timestamps also jump backwards.
#
# A: with time.time() stepped back one hour, pty must still close on time.
# B: control -- the same invocation without the step.
# C: static -- no wall-clock deadline or elapsed measurement remains.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-51 devnode-clock

DEV="$TOOLS_DIR/devnode.py"

cat >"$CASE_DIR/sitecustomize.py" <<'EOF'
import time
_real = time.time
_n = [0]
def shifted():
    _n[0] += 1
    return _real() - (3600 if _n[0] >= 3 else 0)
time.time = shifted
EOF

say "A: wall clock stepped back one hour"
timeout 10 env PYTHONPATH="$CASE_DIR" "$PYTHON" "$DEV" pty \
	--close-after 0.2 --hold-after 0.2 >"$CASE_DIR/a.out" 2>&1
a_rc=$?
obs "A exit: $a_rc"
obs "A output: $(tr '\n' '|' <"$CASE_DIR/a.out")"
assert_eq "$a_rc" "0" \
	"A the pty helper exits on its own despite the wall clock step"
assert_contains "$CASE_DIR/a.out" "event=master-closed" \
	"A the master is closed on schedule, so the slave will report EIO"

say "B: control -- no clock step"
timeout 10 "$PYTHON" "$DEV" pty \
	--close-after 0.2 --hold-after 0.2 >"$CASE_DIR/b.out" 2>&1
b_rc=$?
obs "B exit: $b_rc"
obs "B output: $(tr '\n' '|' <"$CASE_DIR/b.out")"
assert_eq "$b_rc" "0" "B the helper works normally without the clock step"
assert_contains "$CASE_DIR/b.out" "event=master-closed" \
	"B and reports the same event"

say "C: static -- deadlines and durations are monotonic"
assert_eq "$(count_lines "$DEV" 'deadline = time.time()')" "0" \
	"C no wall-clock deadline expression remains"
assert_eq "$(count_lines "$DEV" 'deadline = time.monotonic()')" "2" \
	"C both pty deadlines use the monotonic clock"
assert_eq "$(count_lines "$DEV" 'time.time() - t0')" "0" \
	"C drain no longer measures elapsed time with the wall clock"
assert_contains "$DEV" "time.monotonic() - t0" \
	"C drain reports a monotonic elapsed time"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
