#!/bin/sh
#
# PD-104  [medium]  CASE_TIMEOUT must be validated before the driver works.
#
# run-all.sh used the CASE_TIMEOUT environment variable unchecked.  Zero is
# the dangerous case: GNU timeout(1) treats a duration of 0 as "disable the
# timeout", so every case silently became unbounded and one hung regression
# could wedge CI for ever.  A non-numeric value reached the PD-102 ceiling
# arithmetic $((CASE_TIMEOUT * 10)) and killed the driver with a bare
# "Illegal number: abc" only AFTER all four variants had been built -- a
# confusing failure with no hint at the offending variable.
#
# A: non-numeric values are refused before any build, with a clear message.
# B: zero (which disables timeout) is refused too.
# C: control -- the default still runs a case normally.
# D: static -- the guard precedes the variant build.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-104 case-timeout-validation

DRIVER="$TESTS_DIR/run-all.sh"
TREE="$CASE_DIR/tree"
mkdir -p "$TREE" || exit 1
# Copy the WORKING tree: the fix under test is not committed yet.  An isolated
# tree also keeps the nested driver's kill_stray() away from this case's own
# helper processes.
(cd "$REPO_ROOT" && tar cf - --exclude=.git --exclude=build .) |
	(cd "$TREE" && tar xf -)

run_driver() {
	_label=$1
	_value=$2
	(cd "$TREE" && CASE_TIMEOUT="$_value" timeout 90 \
		sh tests/run-all.sh PD-32) >"$CASE_DIR/$_label.out" 2>&1
	_rc=$?
	obs "$_label CASE_TIMEOUT=$_value rc=$_rc output: $(tr '\n' '|' <"$CASE_DIR/$_label.out")"
	eval "RC_$_label=$_rc"
}

say "A: a non-numeric budget is refused before any work"
run_driver alpha abc
run_driver beta -5
for tag in alpha beta; do
	eval "_rc=\$RC_$tag"
	assert_eq "$_rc" "2" \
		"A $tag exits with the configuration-error status"
	assert_contains "$CASE_DIR/$tag.out" \
		"FATAL: CASE_TIMEOUT must be a positive integer" \
		"A $tag names the offending variable"
	assert_not_contains "$CASE_DIR/$tag.out" "Illegal number" \
		"A $tag does not die inside shell arithmetic"
	assert_not_contains "$CASE_DIR/$tag.out" "== building variants ==" \
		"A $tag fails before building anything"
done

say "B: zero would disable timeout(1) entirely"
run_driver zero 0
eval '_rc=$RC_zero'
assert_eq "$_rc" "2" \
	"B CASE_TIMEOUT=0 is refused"
assert_contains "$CASE_DIR/zero.out" \
	"FATAL: CASE_TIMEOUT must be at least 1" \
	"B with a message explaining the lower bound"
assert_not_contains "$CASE_DIR/zero.out" "== building variants ==" \
	"B and before any build"

say "C: control -- the default budget still runs a case"
run_driver good 45
eval '_rc=$RC_good'
assert_eq "$_rc" "0" \
	"C CASE_TIMEOUT=45 completes successfully"
assert_contains "$CASE_DIR/good.out" "PASS pd-32" \
	"C and the selected case actually ran"
assert_contains "$CASE_DIR/good.out" "== building variants ==" \
	"C with the normal build path"

say "D: static -- the guard precedes the build"
"$PYTHON" - "$DRIVER" >"$CASE_DIR/d.out" 2>&1 <<'EOF'
import sys
s = open(sys.argv[1], encoding="utf-8").read()
case_at = s.find('case "$CASE_TIMEOUT" in')
lower_at = s.find('[ "$CASE_TIMEOUT" -lt 1 ]')
build_at = s.find("build_baseline")
doc = "PD-104: CASE_TIMEOUT feeds timeout(1)" in s
print("case=%d lower=%d build=%d doc=%s" %
      (case_at, lower_at, build_at, doc))
sys.exit(0 if 0 <= case_at < lower_at < build_at and doc else 1)
EOF
assert_eq "$?" "0" \
	"D both checks run before the variant build"
assert_contains "$CASE_DIR/d.out" "doc=True" \
	"D and the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
