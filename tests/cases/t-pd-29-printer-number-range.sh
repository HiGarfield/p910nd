#!/bin/sh
#
# PD-29  [medium]  An out-of-range printer number must be rejected.
#
# usage() advertises [0|1|2], and the device template, lock file, pid file
# and BASEPORT+n all assume exactly those three values.  main() accepted
# any digit: "p910nd 3" started a daemon listening on 9103 for a printer
# that cannot exist, with a lock and pid file nobody expects.  Reject the
# digit at argument-check time instead of starting an unusable service.
#
# A: "3" exits immediately with the usage text (before the fix it stayed
#    in the foreground as a daemon until killed by timeout).
# B: control -- "0" still starts normally.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-29 printer-number-range

LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR"
CASE_BIN=$(build_variant test-badnum "-DLOCKFILE_DIR=\"$LOCKDIR\"")
assert_file_exists "$CASE_BIN" "the number-range variant builds"

DEV="$CASE_DIR/lp0"
: >"$DEV"

# ---------------------------------------------------------------- part A ----
say "A: an out-of-range printer number is rejected at argument check"
timeout 4 "$CASE_BIN" -d -i 127.0.0.1 -f "$DEV" 3 \
	>"$CASE_DIR/a.out" 2>"$CASE_DIR/a.err"
a_rc=$?
obs "A exit status: $a_rc"
obs "A stdout: $(tr '\n' '|' <"$CASE_DIR/a.out")"
obs "A stderr: $(tr '\n' '|' <"$CASE_DIR/a.err")"

assert_eq "$a_rc" "1" \
	"A the invalid number is refused instead of starting a daemon"
assert_contains "$CASE_DIR/a.err" "Usage:" "A with the documented usage text"

# ---------------------------------------------------------------- part B ----
say "B: control -- printer 0 still starts normally"
daemon_start "$CASE_DIR/b.daemon.log" "$CASE_BIN" -d -i 127.0.0.1 \
	-f "$DEV" 0
if [ -z "$DAEMON_PID" ]; then
	obs "B daemon failed to start: $(tr '\n' '|' <"$CASE_DIR/b.daemon.log")"
	assert_true 1 "B printer 0 still starts"
else
	wait_for_port 127.0.0.1 "$TEST_PORT" 5
	assert_true 0 "B printer 0 still starts and listens"
fi
daemon_stop

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
