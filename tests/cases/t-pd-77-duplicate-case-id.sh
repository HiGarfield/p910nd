#!/bin/sh
# case-timeout: 150
#
# PD-77  [high]  Two case files sharing one number must not be dropped.
#
# run-all.sh gets its ids from case_ids(), which sorts -u, and resolves each
# id with `ls "$CASES_DIR/t-$lid"-*.sh | head -1`.  With two files for the
# same number only the first was ever executed, yet the driver still printed
# "PASS <id>" and exited 0.  A newly added regression test could therefore
# be silently skipped forever (measured: t-pd-90-alpha.sh ran, beta never
# did, summary said "passed : pd-90").
#
# A: a duplicated id makes the driver fail loudly and run neither file.
# B: control -- a single file with that id still passes normally.
# C: static -- the duplicate check is present in the driver.
#
# The nested drivers run in an isolated copy of the tree so the real
# tests/cases directory is never polluted.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-77 duplicate-case-id

RUNALL="$TESTS_DIR/run-all.sh"
TREE="$CASE_DIR/tree"
# Only the files the nested driver actually needs, copied from the working
# tree: no .git dependency, so this works in a source tarball too.
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

mkcase() {
	_path=$1
	_marker=$2
	_tag=$3
	cat >"$_path" <<EOF
#!/bin/sh
printf '%s\\n' "$_tag" > "$_marker"
echo "$_tag ran"
exit 0
EOF
	chmod +x "$_path"
}

ALPHA="$CASE_DIR/alpha.marker"
BETA="$CASE_DIR/beta.marker"
CASES="$TREE/tests/cases"

run_nested() {
	_out=$1
	(cd "$TREE" && TEST_PORT=19491 sh tests/run-all.sh PD-90) \
		>"$_out" 2>&1
	NESTED_RC=$?
	obs "nested driver ($_out) exit: $NESTED_RC"
	obs "nested summary: $(sed -n '/== summary ==/,$p' "$_out" | tr '\n' '|')"
}

say "A: a duplicated case id is a hard failure"
mkcase "$CASES/t-pd-90-alpha.sh" "$ALPHA" ALPHA
mkcase "$CASES/t-pd-90-beta.sh" "$BETA" BETA
rm -f "$ALPHA" "$BETA"
run_nested "$CASE_DIR/dup.out"
[ "$NESTED_RC" -ne 0 ]
assert_true "$?" "A the driver exits non-zero on a duplicate id"
assert_contains "$CASE_DIR/dup.out" "DUPLICATE case id" \
	"A and names the problem"
assert_contains "$CASE_DIR/dup.out" "pd-90(duplicate-case-id)" \
	"A the failure is attributed to the duplicated id"
assert_not_contains "$CASE_DIR/dup.out" "PASS pd-90" \
	"A the id is not reported as passed"
if [ -e "$ALPHA" ] || [ -e "$BETA" ]; then
	assert_true 1 "A neither duplicate case file is executed"
else
	assert_true 0 "A neither duplicate case file is executed"
fi

say "B: control -- one file with that id still runs"
rm -f "$CASES/t-pd-90-beta.sh" "$ALPHA" "$BETA"
run_nested "$CASE_DIR/single.out"
assert_eq "$NESTED_RC" "0" "B the single-file run succeeds"
assert_contains "$CASE_DIR/single.out" "PASS pd-90" \
	"B and the case is reported as passed"
assert_not_contains "$CASE_DIR/single.out" "DUPLICATE case id" \
	"B with no false duplicate alarm"
assert_file_exists "$ALPHA" "B the case body really executed"
if [ -e "$BETA" ]; then
	assert_true 1 "B the removed file did not run"
else
	assert_true 0 "B the removed file did not run"
fi

say "C: static -- the driver checks the number of matching files"
assert_contains "$RUNALL" "duplicate-case-id" \
	"C the driver records a duplicate-id failure"
assert_contains "$RUNALL" 'n_matches=$(ls ' \
	"C and counts the files matching each id"
assert_contains "$RUNALL" "PD-77: case_ids()" \
	"C the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
