#!/bin/sh
#
# PD-32  [medium]  The case index must never drift from the case files again.
#
# tests/README.md stopped at t-pd-21 while the suite had grown to t-pd-31,
# and its header still claimed "21 cases, about 250 seconds".  Nothing
# checked the index, so every new case silently made the documentation
# wrong.  This case enumerates the files and compares them with the index;
# adding a case now requires adding its row in the same commit.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-32 readme-index

README="$REPO_ROOT/tests/README.md"
n_cases=0
for id in $(case_ids "$CASES_DIR"); do
	n_cases=$((n_cases + 1))
	if grep -q "t-pd-$id-" "$README"; then
		obs "index: PD-$id present"
	else
		obs "index: PD-$id MISSING"
		assert_true 1 "PD-$id is listed in the README index"
	fi
done

index_rows=$(grep -c '^| `t-pd-[0-9][0-9]*-' "$README")
obs_kv "case files" "$n_cases"
obs_kv "README index rows" "$index_rows"
assert_eq "$index_rows" "$n_cases" \
	"the index lists exactly the cases that exist"
assert_ge "${n_cases:-0}" "30" "the whole suite is enumerated"
assert_not_contains "$README" "全部 21 个用例" \
	"the stale hard-coded case count is gone"
assert_contains "$README" "libwrap0-dev" \
	"the environment section names the libwrap development package"

# The defect report's live regression rule had the same problem: it still
# demanded "17/17 in about 187 seconds" after the suite reached 36 cases.
# Only the rule block is checked; the historical observations above it keep
# their original numbers on purpose.
REPORT="$REPO_ROOT/tests/DEFECT_REPORT.md"
sed -n '/^## 回归规则/,/^静态分析基线/p' "$REPORT" >"$CASE_DIR/rules.txt"
obs "regression rule:"
sed 's/^/    /' "$CASE_DIR/rules.txt" | while read -r l; do obs "$l"; done
assert_not_contains "$CASE_DIR/rules.txt" "17/17" \
	"the live rule no longer hard-codes the old case count"
assert_not_contains "$CASE_DIR/rules.txt" "约 187 秒" \
	"the live rule no longer hard-codes the old runtime"
assert_contains "$CASE_DIR/rules.txt" "make test" \
	"the rule still requires the full suite"
assert_contains "$CASE_DIR/rules.txt" "sh tests/mutation-check.sh" \
	"and the sensitivity check"
assert_contains "$CASE_DIR/rules.txt" "make && make CC=clang" \
	"and the warning-free dual build"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
