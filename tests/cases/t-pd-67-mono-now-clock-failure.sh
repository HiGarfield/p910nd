#!/bin/sh
#
# PD-67  [medium]  mono_now() must never leave an undefined timeval.
#
# The wall-clock path called (void)gettimeofday(tv, 0) and returned without
# checking the result.  When both CLOCK_MONOTONIC and gettimeofday() fail,
# the caller's struct (uninitialised stack memory in every deadline path) is
# left untouched: measured with a poison value "rc=0 sec=12345 usec=678",
# which would make all "now - start" comparisons meaningless.  PD-40 already
# protected the monotonic branch; the fallback branch did not.
#
# A: both clocks fail -> a defined zero fallback.
# B: monotonic fails but wall works -> wall time still returned.
# C: monotonic works -> unchanged monotonic value.
# D: static -- the fallback is explicit and the sources agree.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-67 mono-now-clock-failure

PROBE="$CASE_DIR/mono_fail_probe"
"$CC_BIN" -std=gnu89 -O1 -o "$PROBE" \
	"$TOOLS_DIR/mono_fail_probe.c" -I"$REPO_ROOT" \
	>"$CASE_DIR/probe.build.log" 2>&1
build_rc=$?
obs "probe build:"
sed 's/^/    /' "$CASE_DIR/probe.build.log" | while read -r l; do obs "$l"; done
assert_eq "$build_rc" "0" "the clock-failure probe builds"

run_mode() {
	_mode=$1
	"$PROBE" "$_mode" >"$CASE_DIR/$_mode.out" 2>&1
	PROBE_RC=$?
	obs "$_mode: $(tr '\n' '|' <"$CASE_DIR/$_mode.out")"
	obs_kv "$_mode exit" "$PROBE_RC"
}

say "A: both clocks fail"
run_mode both-fail
assert_eq "$PROBE_RC" "0" \
	"A mono_now() produces a defined value instead of the poisoned one"
assert_contains "$CASE_DIR/both-fail.out" "sec=0 usec=0" \
	"A the zero-initialised last-good sample is used"

say "B: control -- monotonic fails, wall clock works"
run_mode wall
assert_eq "$PROBE_RC" "0" "B the documented wall-clock fallback still works"
assert_contains "$CASE_DIR/wall.out" "sec=1700000000" \
	"B and returns the wall value"

say "C: control -- monotonic clock works"
run_mode mono
assert_eq "$PROBE_RC" "0" "C the monotonic path is unchanged"
assert_contains "$CASE_DIR/mono.out" "rc=1 sec=5 usec=0" \
	"C and still reports the monotonic time base"

say "D: static -- the fallback is explicit"
assert_contains "$REPO_ROOT/p910nd.c" 'gettimeofday(tv, 0) != 0' \
	"D the wall-clock path checks gettimeofday()"
assert_contains "$REPO_ROOT/p910nd.c" 'static struct timeval last' \
	"D a zero-initialised last-good sample exists"
assert_contains "$REPO_ROOT/p910nd.c" 'last = \*tv' \
	"D successful samples are remembered"
assert_contains "$REPO_ROOT/p910nd.c" 'never hand the caller' \
	"D the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
