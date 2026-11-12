#!/bin/sh
# case-timeout: 240
#
# PD-105  [medium]  A sensitivity check must not overwrite the last test run.
#
# tests/mutation-check.sh deliberately reverts three fixes and runs the
# matching case scripts from the LIVE tree.  Every case_init() truncates
# $OBS_DIR/<id>.txt and rebuilds $BUILD_DIR/cases/<id>/, so after a fully
# green `make test`, running the regression rule
#   sh tests/mutation-check.sh
# left PD-05/PD-07/PD-09 observations containing "ASSERT FAIL" lines from
# the intentional mutations.  CI archiving build/tests/ then reported
# failures that never happened, and the real evidence for the green run was
# gone.  Measured: the observation timestamps matched the mutation run, not
# the suite, with 1/1/2 spurious failure lines.
#
# A: the mutation check still detects all three reverted fixes.
# B: it leaves the live observations byte-for-byte unchanged.
# C: static -- mutated cases run from an isolated tree.
# D: control -- the live observations are genuinely clean to begin with.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-105 mutation-observations

SCRIPT="$TESTS_DIR/mutation-check.sh"
TREE="$CASE_DIR/tree"
mkdir -p "$TREE" || exit 1
# Copy the WORKING tree: the fix under test is not committed yet.  Doing the
# experiment inside a throw-away tree also keeps this case from touching the
# real build/tests/observations.
(cd "$REPO_ROOT" && tar cf - --exclude=.git --exclude=build .) |
	(cd "$TREE" && tar xf -)

PORT=$("$PYTHON" -c 'import socket
s = socket.socket(); s.bind(("127.0.0.1", 0))
print(s.getsockname()[1]); s.close()')
obs "isolated port: $PORT"

say "D: establish a clean observation baseline"
(cd "$TREE" && TEST_PORT="$PORT" sh tests/run-all.sh PD-05 PD-09) \
	>"$CASE_DIR/baseline.log" 2>&1
assert_eq "$?" "0" \
	"D the two cases pass in the isolated tree"
OBS="$TREE/build/tests/observations"
for id in PD-05 PD-09; do
	assert_file_exists "$OBS/$id.txt" \
		"D $id has an observation record"
	_n=$(count_lines "$OBS/$id.txt" 'ASSERT FAIL')
	assert_eq "$_n" "0" \
		"D $id starts with no failure lines"
done
sha256sum "$OBS/PD-05.txt" "$OBS/PD-09.txt" >"$CASE_DIR/before.sha"
obs "baseline hashes: $(tr '\n' '|' <"$CASE_DIR/before.sha")"

say "A/B: run the sensitivity check and re-check the evidence"
(cd "$TREE" && TEST_PORT="$PORT" sh tests/mutation-check.sh) \
	>"$CASE_DIR/mutation.log" 2>&1
MUT_RC=$?
obs "mutation exit: $MUT_RC"
sed 's/^/    /' "$CASE_DIR/mutation.log" | while read -r l; do obs "$l"; done
assert_eq "$MUT_RC" "0" \
	"A all three mutations are still detected"
assert_contains "$CASE_DIR/mutation.log" "all mutations were detected" \
	"A the usual success line is printed"
sha256sum -c "$CASE_DIR/before.sha" >"$CASE_DIR/hash.out" 2>&1
HASH_RC=$?
obs "hash check: $(tr '\n' '|' <"$CASE_DIR/hash.out")"
assert_eq "$HASH_RC" "0" \
	"B the live observation files are byte-for-byte unchanged"
for id in PD-05 PD-09; do
	_n=$(count_lines "$OBS/$id.txt" 'ASSERT FAIL')
	assert_eq "$_n" "0" \
		"B $id still has no failure lines after the mutations"
done

say "C: static -- the mutated cases are isolated"
"$PYTHON" - "$SCRIPT" >"$CASE_DIR/c.out" 2>&1 <<'EOF'
import sys
s = open(sys.argv[1], encoding="utf-8").read()
isolated = 'TREE="$WORK/tree"' in s and 'cd "$TREE"' in s
tar = "tar cf - --exclude=.git --exclude=build" in s
runs = s.count('cd "$TREE" && P910ND_SRC=')
live = 'sh "$CASES_DIR/t-pd-' in s
doc = "PD-105: run the mutated cases" in s
print("isolated=%s tar=%s isolated_runs=%d live_runs=%s doc=%s" %
      (isolated, tar, runs, live, doc))
sys.exit(0 if isolated and tar and runs == 3 and not live and doc else 1)
EOF
assert_eq "$?" "0" \
	"C all three mutated cases run in the isolated tree"
assert_contains "$CASE_DIR/c.out" \
	"isolated=True tar=True isolated_runs=3 live_runs=False doc=True" \
	"C every property was actually checked"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
