#!/bin/sh
#
# PD-52  [medium]  The lock holder must release on a monotonic deadline.
#
# tests/tools/lockhold.py hold --seconds N used time.time() for its release
# deadline.  A backwards wall-clock step (NTP, VM resume, the fake-clock
# shim PD-07 loads) makes "now >= deadline" false for hours, so the byte-1
# job lock is never released and PD-08 / PD-19 (and every later job) hang.
#
# A: with time.time() stepped back one hour the holder must still exit and
#    release the lock after one second.
# B: control -- the same holder without the clock step.
# C: static -- the deadline uses time.monotonic().

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-52 lockhold-clock

HOLD="$TOOLS_DIR/lockhold.py"

cat >"$CASE_DIR/sitecustomize.py" <<'EOF'
import time
_real = time.time
_n = [0]
def shifted():
    _n[0] += 1
    return _real() - (3600 if _n[0] >= 3 else 0)
time.time = shifted
EOF

# True only when byte 1 of the lock file can be locked non-blockingly.
lock_is_free() {
	"$PYTHON" -c 'import fcntl, os, sys
fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT, 0o600)
try:
    fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB, 1, 1, os.SEEK_SET)
except OSError:
    sys.exit(1)
os.close(fd)
sys.exit(0)' "$1"
}

run_holder() {
	_which=$1
	_file="$CASE_DIR/$_which.lock"
	: >"$_file"
	if [ "$_which" = a ]; then
		timeout 10 env PYTHONPATH="$CASE_DIR" "$PYTHON" "$HOLD" hold \
			--file "$_file" --byte 1 --seconds 1 \
			>"$CASE_DIR/$_which.out" 2>&1
	else
		timeout 10 "$PYTHON" "$HOLD" hold \
			--file "$_file" --byte 1 --seconds 1 \
			>"$CASE_DIR/$_which.out" 2>&1
	fi
	HOLD_RC=$?
	obs "$_which output: $(tr '\n' '|' <"$CASE_DIR/$_which.out")"
	obs_kv "$_which exit" "$HOLD_RC"
}

say "A: wall clock stepped back one hour"
run_holder a
assert_eq "$HOLD_RC" "0" \
	"A the holder exits after one second despite the wall clock step"
assert_contains "$CASE_DIR/a.out" "LOCKED" \
	"A it really acquired byte 1 before waiting"
lock_is_free "$CASE_DIR/a.lock"
assert_eq "$?" "0" \
	"A and released the lock, so the next job is not stuck"

say "B: control -- no clock step"
run_holder b
assert_eq "$HOLD_RC" "0" "B the holder works normally without the clock step"
lock_is_free "$CASE_DIR/b.lock"
assert_eq "$?" "0" "B and releases the lock there too"

say "C: static -- the release deadline is monotonic"
assert_eq "$(count_lines "$HOLD" 'time.time()')" "0" \
	"C no wall-clock deadline expression remains"
assert_contains "$HOLD" "deadline = time.monotonic()" \
	"C the deadline uses the monotonic clock"
assert_contains "$HOLD" "time.monotonic() >= deadline" \
	"C and so does the loop condition"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
