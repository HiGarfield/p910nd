#!/bin/sh
#
# PD-34  [medium]  Invalid printer arguments must be rejected, not ignored.
#
# usage() advertises an optional single [0|1|2].  main() only looked at the
# first character: "p910nd foo" silently served lp0, "p910nd 12" served
# lp1, and "p910nd 0 1" ignored the extra argument.  A typo therefore
# started a daemon on an unexpected port instead of reporting an error.
#
# A: three malformed invocations exit 1 with the synopsis (before the fix
#    each stayed in the foreground as a daemon until killed by timeout).
# B: control -- a valid "0" still starts and listens.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-34 argument-check

LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR"
CASE_BIN=$(build_variant test-args "-DLOCKFILE_DIR=\"$LOCKDIR\"")
assert_file_exists "$CASE_BIN" "the argument-check variant builds"

DEV="$CASE_DIR/lp0"
: >"$DEV"

check_rejected() {
	# check_rejected <label> [args...]
	_label=$1
	shift
	timeout 4 "$CASE_BIN" -d -i 127.0.0.1 -f "$DEV" "$@" \
		>"$CASE_DIR/$_label.out" 2>"$CASE_DIR/$_label.err"
	_rc=$?
	obs "$_label exit=$_rc stderr=$(tr '\n' '|' <"$CASE_DIR/$_label.err")"
	assert_eq "$_rc" "1" "$_label: rejected with the usage status"
	assert_contains "$CASE_DIR/$_label.err" "Usage:" \
		"$_label: prints the documented synopsis"
}

say "A: malformed arguments are refused"
check_rejected nonnumeric foo
check_rejected multidigit 12
check_rejected extra 0 1
check_rejected outofrange 9

assert_contains "$REPO_ROOT/p910nd.c" "if (argc > 1)" \
	"the source rejects more than one positional argument"

say "B: control -- printer 0 still starts normally"
daemon_start "$CASE_DIR/ok.daemon.log" "$CASE_BIN" -d -i 127.0.0.1 \
	-f "$DEV" 0
if [ -z "$DAEMON_PID" ]; then
	obs "B daemon failed to start: $(tr '\n' '|' <"$CASE_DIR/ok.daemon.log")"
	assert_true 1 "B printer 0 still starts"
else
	wait_for_port 127.0.0.1 "$TEST_PORT" 5
	assert_true 0 "B printer 0 still starts and listens"
fi
daemon_stop

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
