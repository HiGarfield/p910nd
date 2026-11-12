#!/bin/sh
#
# PD-58  [medium]  A slow static-analysis case must not be killed by the
# shared wall-clock budget.
#
# run-all.sh wrapped every case in the same CASE_TIMEOUT (45 s).  PD-00 runs
# gcc -fanalyzer, cppcheck and clang-tidy; on a loaded CI runner that can
# exceed the budget and was observed as "FAIL pd-00 (timed out after 45s)"
# -- a false failure with no source defect.  A case now declares
# "# case-timeout: N" to ask for a larger (never smaller) budget.
#
# The case builds a private copy of the harness with two synthetic cases
# that sleep longer than CASE_TIMEOUT=1:
#   A: one declaring a larger budget must PASS.
#   B: one without the declaration must still time out (default unchanged).
# C: static -- PD-00 declares 120 and the driver parses the declaration.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-58 case-timeout-budget

FIX="$CASE_DIR/harness"
mkdir -p "$FIX/tests/tools" "$FIX/tests/cases"
for f in Makefile p910nd.c tests/run-all.sh tests/lib.sh tests/variants.sh \
	tests/tools/patch_source.py tests/tools/faketime_preload.c; do
	mkdir -p "$FIX/$(dirname "$f")"
	cp "$REPO_ROOT/$f" "$FIX/$f"
done

cat >"$FIX/tests/cases/t-pd-58-extended.sh" <<'EOF'
#!/bin/sh
# case-timeout: 8
sleep 2
exit 0
EOF
cat >"$FIX/tests/cases/t-pd-59-default.sh" <<'EOF'
#!/bin/sh
sleep 2
exit 0
EOF

PORT=$((TEST_PORT + 1))

say "A: a case declaring a larger budget survives CASE_TIMEOUT=1"
TEST_PORT="$PORT" CASE_TIMEOUT=1 sh "$FIX/tests/run-all.sh" PD-58 \
	>"$CASE_DIR/extended.log" 2>&1
a_rc=$?
obs "A exit: $a_rc"
obs "A output: $(tr '\n' '|' <"$CASE_DIR/extended.log" | tail -c 400)"
assert_eq "$a_rc" "0" \
	"A the declared budget is applied instead of the 1 s default"
assert_contains "$CASE_DIR/extended.log" "PASS pd-58" \
	"A the extended case really ran to completion"

say "B: a case without a declaration still honours the default"
TEST_PORT="$PORT" CASE_TIMEOUT=1 sh "$FIX/tests/run-all.sh" PD-59 \
	>"$CASE_DIR/default.log" 2>&1
b_rc=$?
obs "B exit: $b_rc"
obs "B output: $(tr '\n' '|' <"$CASE_DIR/default.log" | tail -c 400)"
assert_eq "$b_rc" "1" \
	"B the default budget is unchanged (the case still times out)"
assert_contains "$CASE_DIR/default.log" "timed out after 1s" \
	"B and the failure names the default budget"

say "C: static -- the driver and PD-00 agree on the declaration"
assert_contains "$REPO_ROOT/tests/run-all.sh" 'case-timeout:' \
	"C the driver parses a per-case budget"
assert_contains "$REPO_ROOT/tests/run-all.sh" 'timeout "$case_limit"' \
	"C the parsed budget is what timeout actually receives"
assert_contains "$REPO_ROOT/tests/cases/t-pd-00-static-analysis-baseline.sh" \
	"# case-timeout: 120" \
	"C PD-00 declares a budget large enough for the analysers"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
