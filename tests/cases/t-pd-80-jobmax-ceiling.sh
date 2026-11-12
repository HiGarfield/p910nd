#!/bin/sh
#
# PD-80  [medium]  B-2's absolute job ceiling needs a real regression test.
#
# DEFECT_REPORT.md (B-2) says the "PRINTER_STALL_TIMEOUT=0 can hang forever"
# defect was fixed by adding JOB_MAX_DURATION as "an absolute ceiling that
# cannot be switched off", and lists NO test case for it.  In fact:
#   * the source comment still says "the only deadline that cannot be
#     switched off", while the default is 0, the guard is
#     `JOB_MAX_DURATION > 0`, and p910nd.8 documents "0 = no limit";
#   * no case ever exercises the ceiling, so removing it stays green.
# The misleading comment/report is dangerous: it tells an operator using
# PRINTER_STALL_TIMEOUT=0 that an unbounded job is protected.
#
# A: with JOB_MAX_DURATION=1 a silent connection ends via that ceiling,
#    before IDLE_TIMEOUT (3 s in the fast test profile) can fire.
# B: control -- with JOB_MAX_DURATION=0 the idle deadline fires instead.
# C: source, man page and report describe the same opt-in semantics.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-80 jobmax-ceiling

LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR" || exit 1
DEV="$CASE_DIR/lp0"
: >"$DEV"

# build_variant forwards "$@" as separate arguments: each -D must stand
# alone, otherwise gcc sees one giant macro definition.
JOBMAX_BIN=$(build_variant test-jobmax \
	"-DLOCKFILE_DIR=\"$LOCKDIR\"" "-DJOB_MAX_DURATION=1")
NOLIMIT_BIN=$(build_variant test-nolimit \
	"-DLOCKFILE_DIR=\"$LOCKDIR\"" "-DJOB_MAX_DURATION=0")
assert_file_exists "$JOBMAX_BIN" "the 1-second ceiling variant builds"
assert_file_exists "$NOLIMIT_BIN" "the no-ceiling control variant builds"

run_silent_job() {
	_label=$1
	_bin=$2
	daemon_start "$CASE_DIR/$_label.log" "$_bin" -d -i 127.0.0.1 \
		-f "$DEV" 0
	if [ -z "$DAEMON_PID" ]; then
		obs "$_label daemon failed: $(tr '\n' '|' <"$CASE_DIR/$_label.log")"
		assert_true 1 "$_label the daemon starts"
		return
	fi
	wait_for_port 127.0.0.1 "$TEST_PORT" 5
	assert_eq "$?" "0" "$_label the daemon listens"
	# Keep the connection open without sending a byte: no half-close, no
	# reply expected, so only a deadline can end the job.
	"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" \
		--linger 5 --expect-reply 0 \
		>"$CASE_DIR/$_label.client.out" 2>&1
	obs "$_label client: $(tr '\n' '|' <"$CASE_DIR/$_label.client.out")"
	daemon_stop
	obs "$_label daemon log: $(tr '\n' '|' <"$CASE_DIR/$_label.log")"
}

say "A: JOB_MAX_DURATION=1 ends the job before the idle deadline"
run_silent_job ceiling "$JOBMAX_BIN"
assert_contains "$CASE_DIR/ceiling.log" "job ran for" \
	"A the absolute ceiling ended the job"
assert_not_contains "$CASE_DIR/ceiling.log" "no data transferred" \
	"A the idle deadline did not get there first"

say "B: control -- JOB_MAX_DURATION=0 leaves the idle deadline in charge"
run_silent_job noceling "$NOLIMIT_BIN"
assert_contains "$CASE_DIR/noceling.log" "no data transferred" \
	"B with the ceiling disabled the idle deadline ends the job"
assert_not_contains "$CASE_DIR/noceling.log" "job ran for" \
	"B and no ceiling message is logged"

say "C: source, man page and report agree the ceiling is opt-in"
assert_eq "$(count_lines "$REPO_ROOT/p910nd.c" 'JOB_MAX_DURATION > 0 &&')" "2" \
	"C both copy loops guard the ceiling with > 0"
assert_not_contains "$REPO_ROOT/p910nd.c" "cannot be switched off" \
	"C no source comment claims the ceiling cannot be disabled"
assert_contains "$REPO_ROOT/p910nd.8" "0 = no limit" \
	"C the man page documents the disabled default"
assert_not_contains "$TESTS_DIR/DEFECT_REPORT.md" "不可关闭的绝对上界" \
	"C the report no longer makes that claim"
assert_contains "$TESTS_DIR/DEFECT_REPORT.md" "t-pd-80" \
	"C and points at this regression case"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
