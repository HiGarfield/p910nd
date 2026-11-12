#!/bin/sh
#
# Driver for the p910nd defect verification cases.
#
#   make test          # or: sh tests/run-all.sh
#   sh tests/run-all.sh PD-01 PD-07        # only the named cases
#
# Everything the run produces lives under build/tests/ and is removed by
# `make clean`.  p910nd.c is never modified: the cases that need a different
# constant build a patched *copy* which is deleted as soon as it is compiled.

set -u

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
export REPO_ROOT
TESTS_DIR="$REPO_ROOT/tests"
. "$TESTS_DIR/lib.sh"

TEST_PORT=${TEST_PORT:-19100}
LOCK_DIR_OK="$BUILD_DIR/lock-ok"
LOCK_DIR_MISSING="$BUILD_DIR/lock-absent"
FAKETIME_SO="$BUILD_DIR/faketime.so"

mkdir -p "$BUILD_DIR" "$OBS_DIR" || exit 2
LOCK_DIR_OK="$BUILD_DIR/lock-ok"
LOCK_DIR_MISSING="$BUILD_DIR/lock-absent"
mkdir -p "$LOCK_DIR_OK"
rm -rf "$LOCK_DIR_MISSING"
export LOCK_DIR_OK LOCK_DIR_MISSING FAKETIME_SO TEST_PORT

# ---------------------------------------------------------------- cleanup ----

cleanup() {
	kill_stray
	# any source copy still lying around is a bug in the harness, not a fixture
	rm -rf "$BUILD_DIR"/src.* 2>/dev/null
}
# PD-74: a signal trap that only runs cleanup returns to the interrupted
# command, so SIGTERM/SIGINT from a cancelled CI job did NOT stop the driver:
# it kept building and running cases and could even exit 0.  Signal traps
# must exit explicitly; EXIT still runs cleanup on every path.
trap 'cleanup' EXIT
trap 'exit 143' INT TERM

# ------------------------------------------------------------------ build ----

say "== building variants =="

rc=0
build_baseline >/dev/null || rc=1
# Every functional variant needs a writable lock directory: since PD-01 the
# daemon always takes the instance lock and refuses to start without one,
# and the test runs unprivileged.
build_variant test "-DLOCKFILE_DIR=\"$LOCK_DIR_OK\"" >/dev/null || rc=1
build_variant test-lockdir "-DLOCKFILE_DIR=\"$LOCK_DIR_OK\""    >/dev/null || rc=1
build_variant test-lockdir-missing "-DLOCKFILE_DIR=\"$LOCK_DIR_MISSING\"" >/dev/null || rc=1
if [ "$rc" -ne 0 ]; then
	say "FATAL: could not build the variants, see $BUILD_DIR/*.build.log"
	exit 2
fi
if ! "$CC_BIN" -shared -fPIC -O1 -o "$FAKETIME_SO" \
	"$TOOLS_DIR/faketime_preload.c" -ldl >"$BUILD_DIR/faketime.build.log" 2>&1; then
	say "FATAL: could not build the fake clock shim:"
	cat "$BUILD_DIR/faketime.build.log"
	exit 2
fi
for v in p910nd-baseline test test-lockdir test-lockdir-missing; do
	say "   built $BUILD_DIR/$v"
done

# ------------------------------------------------------------------- run -----

if [ "$#" -gt 0 ]; then
	CASE_LIST="$*"
else
	CASE_LIST=$(case_ids "$CASES_DIR" | sed 's/^/t-pd-/')
fi

# PD-91: an empty/missing/renamed cases directory made case_ids() return
# nothing, so the loop ran zero assertions while FAILED and SKIPPED stayed
# empty and the driver exited 0 -- a completely green "make test" that
# tested nothing.  Same false-green class as PD-42/PD-99, but on the
# default no-argument path.
if [ -z "$CASE_LIST" ]; then
	say "FATAL: no test cases found in $CASES_DIR"
	exit 1
fi

PASSED=""
FAILED=""
SKIPPED=""

# PD-102: "# case-timeout: N" used to be completely unbounded.  A typo such
# as 999999 (or a stale copy of a legacy value) handed timeout(1) a wall clock
# of days: a hung case wedged the whole CI job with no diagnostic at all.
# Allow a case to ask for up to ten times the default budget -- the largest
# declaration in this suite is 150s with a 45s default -- and clamp anything
# beyond that, loudly.
CASE_TIMEOUT_MAX=$((CASE_TIMEOUT * 10))

for id in $CASE_LIST; do
	# accept PD-01, pd-01 or t-pd-01 on the command line and from the scan
	lid=$(printf '%s' "$id" | tr 'A-Z' 'a-z')
	lid=${lid#t-}
	case_file=$(ls "$CASES_DIR/t-$lid"-*.sh 2>/dev/null | head -1)
	if [ -z "$case_file" ]; then
		# A requested case that does not exist is a broken suite, not a
		# skip: counting it as skipped used to let `run-all.sh PD-99`
		# (and a renamed or lost case file) exit 0 without running a
		# single assertion.
		FAILED="$FAILED $lid(no-case-file)"
		say "?? $lid: no case file"
		continue
	fi
	# PD-77: case_ids() returns unique numeric ids and `ls | head -1` picks
	# ONE file for an id.  A second file with the same number was therefore
	# never executed while the driver still printed PASS for that id: a
	# silently dropped regression test.  Two files sharing an id is a
	# broken suite -- fail loudly instead of choosing one arbitrarily.
	n_matches=$(ls "$CASES_DIR/t-$lid"-*.sh 2>/dev/null | wc -l | tr -d ' ')
	if [ "$n_matches" -gt 1 ]; then
		FAILED="$FAILED $lid(duplicate-case-id)"
		say "?? $lid: DUPLICATE case id ($n_matches case files)"
		continue
	fi
	kill_stray
	sleep 0.2
	if ! port_is_free "$TEST_PORT"; then
		SKIPPED="$SKIPPED $lid(port-$TEST_PORT-busy)"
		say "?? $lid: SKIPPED, TCP $TEST_PORT is already in use"
		continue
	fi
	# PD-58: a case may ask for a larger wall-clock budget by declaring
	# "# case-timeout: N" (N > the default).  Static-analysis cases run
	# several external tools and can exceed CASE_TIMEOUT on a loaded CI
	# runner; being killed there is a false failure, not a defect.  The
	# declaration never shortens the default budget.
	case_limit=$CASE_TIMEOUT
	_extra=$(sed -n 's/^# case-timeout: \([0-9][0-9]*\)$/\1/p' \
		"$case_file" | head -1)
	if [ -n "$_extra" ] && [ "$_extra" -gt "$case_limit" ]; then
		if [ "$_extra" -gt "$CASE_TIMEOUT_MAX" ]; then
			say "?? $lid: case-timeout $_extra clamped to $CASE_TIMEOUT_MAX"
			case_limit=$CASE_TIMEOUT_MAX
		else
			case_limit=$_extra
		fi
	fi
	out=$(timeout "$case_limit" sh "$case_file" 2>&1)
	status=$?
	printf '%s\n' "$out"
	if [ "$status" -eq 0 ]; then
		PASSED="$PASSED $lid"
		say "PASS $lid"
	elif [ "$status" -eq 124 ]; then
		FAILED="$FAILED $lid(timeout)"
		say "FAIL $lid (timed out after ${case_limit}s)"
	else
		FAILED="$FAILED $lid(exit-$status)"
		say "FAIL $lid (exit $status)"
	fi
done

# --------------------------------------------------------------- summary ----

say ""
say "== summary =="
say "passed :${PASSED:- none}"
say "failed :${FAILED:- none}"
say "skipped:${SKIPPED:- none}"
say "observations: $OBS_DIR"

# PD-42: a case skipped because the test port is busy ran zero assertions.
# Exit 0 in that situation made `make test` (and CI) report a fully green
# suite without running a single test, so a stray listener on TEST_PORT
# silenced every regression check.  Skips are failures, just like missing
# case files; the message names the port so the cause is obvious.
if [ -n "$SKIPPED" ]; then
	say "FAIL: skipped cases ran no assertions (TCP $TEST_PORT is busy)"
fi
if [ -n "$FAILED" ] || [ -n "$SKIPPED" ]; then
	exit 1
fi
exit 0
