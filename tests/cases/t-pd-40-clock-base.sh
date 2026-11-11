#!/bin/sh
#
# PD-40  [high]  A transient clock_gettime() failure must not change the time base.
#
# mono_now() used CLOCK_MONOTONIC (seconds since boot) on every call and fell
# back to gettimeofday() (seconds since the epoch) whenever that call failed.
# One transient failure therefore made a "now - start" difference jump by
# decades: every idle/stall/reply/flush deadline fired at once on the forward
# jump, and the reverse order (wall clock first, monotonic later) made them
# negative so jobs hung forever.
#
# The probe fails clock_gettime() after the first successful call and checks
# that the returned values stay on one base and keep advancing.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-40 clock-base

PROBE="$CASE_DIR/clock_probe"
"$CC_BIN" -std=gnu89 -O1 -o "$PROBE" \
	"$TOOLS_DIR/clock_probe.c" -I"$REPO_ROOT" \
	>"$CASE_DIR/probe.build.log" 2>&1
build_rc=$?
obs "probe build:"
sed 's/^/    /' "$CASE_DIR/probe.build.log" | while read -r l; do obs "$l"; done
assert_eq "$build_rc" "0" "the clock-base probe builds"

"$PROBE" >"$CASE_DIR/probe.out" 2>&1
probe_rc=$?
obs "probe output: $(tr '\n' '|' <"$CASE_DIR/probe.out")"
obs_kv "probe exit status" "$probe_rc"
assert_eq "$probe_rc" "0" \
	"mono_now() keeps one time base across a clock_gettime() failure"
assert_contains "$CASE_DIR/probe.out" "step1=3" \
	"the first fallback sample advanced by the elapsed wall seconds"
assert_contains "$CASE_DIR/probe.out" "step2=7" \
	"and later samples keep advancing instead of freezing"
assert_contains "$REPO_ROOT/p910nd.c" "wall_offset" \
	"the source keeps an explicit wall-minus-monotonic offset"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
