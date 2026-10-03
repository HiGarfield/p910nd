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
trap 'cleanup; kill_stray' EXIT INT TERM

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
	CASE_LIST=$(ls "$CASES_DIR" | sed -n 's/^\(t-pd-[0-9][0-9]\).*/\1/p' | sort -u)
fi

PASSED=""
FAILED=""
SKIPPED=""

for id in $CASE_LIST; do
	# accept PD-01, pd-01 or t-pd-01 on the command line and from the scan
	lid=$(printf '%s' "$id" | tr 'A-Z' 'a-z')
	lid=${lid#t-}
	case_file=$(ls "$CASES_DIR/t-$lid"-*.sh 2>/dev/null | head -1)
	if [ -z "$case_file" ]; then
		SKIPPED="$SKIPPED $lid(no-case-file)"
		say "?? $lid: no case file"
		continue
	fi
	kill_stray
	sleep 0.2
	if ! port_is_free "$TEST_PORT"; then
		SKIPPED="$SKIPPED $lid(port-$TEST_PORT-busy)"
		say "?? $lid: SKIPPED, TCP $TEST_PORT is already in use"
		continue
	fi
	out=$(timeout "$CASE_TIMEOUT" sh "$case_file" 2>&1)
	status=$?
	printf '%s\n' "$out"
	if [ "$status" -eq 0 ]; then
		PASSED="$PASSED $lid"
		say "PASS $lid"
	elif [ "$status" -eq 124 ]; then
		FAILED="$FAILED $lid(timeout)"
		say "FAIL $lid (timed out after ${CASE_TIMEOUT}s)"
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

[ -n "$FAILED" ] && exit 1
exit 0
