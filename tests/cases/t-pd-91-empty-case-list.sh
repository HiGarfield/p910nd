#!/bin/sh
# case-timeout: 150
#
# PD-91  [high]  A suite with zero cases must not report a green run.
#
# run-all.sh scans tests/cases with case_ids() when no case is named.  If
# that directory is empty, missing or renamed, case_ids() returns nothing:
# the loop runs ZERO assertions, FAILED and SKIPPED stay empty, and the
# driver exits 0.  Measured in an isolated tree:
#   passed : none / failed : none / skipped : none, driver_exit=0
# A rename or a broken checkout would therefore make `make test` and CI
# report success while testing nothing (same false-green class as PD-42
# and PD-99, but on the default no-argument path).
#
# A: an empty cases directory is a hard failure.
# B: control -- one real case in the same tree runs and passes.
# C: static -- the driver has the empty-list guard.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-91 empty-case-list

RUNALL="$TESTS_DIR/run-all.sh"
TREE="$CASE_DIR/tree"
# Only the files the nested driver needs; no git metadata required.
mkdir -p "$TREE/tests/cases" "$TREE/tests/tools" || exit 1
cp "$REPO_ROOT/p910nd.c" "$REPO_ROOT/Makefile" "$TREE/" &&
	cp "$RUNALL" "$TESTS_DIR/lib.sh" "$TESTS_DIR/variants.sh" \
		"$TREE/tests/" &&
	cp "$TOOLS_DIR/patch_source.py" "$TOOLS_DIR/faketime_preload.c" \
		"$TREE/tests/tools/" || {
	obs "FATAL: could not create the isolated checkout"
	assert_true 1 "the isolated checkout is created"
	exit 1
}

run_nested() {
	_label=$1
	(cd "$TREE" && TEST_PORT=19474 sh tests/run-all.sh) \
		>"$CASE_DIR/$_label.out" 2>&1
	NESTED_RC=$?
	obs "$_label nested exit: $NESTED_RC"
	obs "$_label output: $(tail -6 "$CASE_DIR/$_label.out" | tr '\n' '|')"
}

say "A: an empty cases directory is a hard failure"
run_nested empty
[ "$NESTED_RC" -ne 0 ]
assert_true "$?" "A the driver exits non-zero with zero cases"
assert_contains "$CASE_DIR/empty.out" "no test cases found" \
	"A and says why"
assert_not_contains "$CASE_DIR/empty.out" "== summary ==" \
	"A no green summary is printed"

say "B: control -- one real case still runs"
cat >"$TREE/tests/cases/t-pd-91-ok.sh" <<EOF
#!/bin/sh
: > "$TREE/tests/cases/ran.marker"
echo "PD-91 dummy case ran"
exit 0
EOF
chmod +x "$TREE/tests/cases/t-pd-91-ok.sh"
run_nested single
assert_eq "$NESTED_RC" "0" \
	"B a one-case run still succeeds"
assert_contains "$CASE_DIR/single.out" "PASS pd-91" \
	"B and the case is reported as passed"
assert_file_exists "$TREE/tests/cases/ran.marker" \
	"B the case body really executed"
assert_not_contains "$CASE_DIR/single.out" "no test cases found" \
	"B with no false empty-suite alarm"

say "C: static -- the empty-list guard is present"
# Literal substring: '[ ... ]' is a regex character class in grep.
"$PYTHON" - "$RUNALL" <<'EOF'
import sys
s = open(sys.argv[1], encoding="utf-8").read()
sys.exit(0 if 'if [ -z "$CASE_LIST" ]; then' in s else 1)
EOF
assert_eq "$?" "0" \
	"C the driver checks for an empty case list"
assert_contains "$RUNALL" "FATAL: no test cases found" \
	"C and reports it as fatal"
assert_contains "$RUNALL" "PD-91: an empty/missing/renamed cases directory" \
	"C the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
