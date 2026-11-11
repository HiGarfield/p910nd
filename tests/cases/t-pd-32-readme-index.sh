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
for id in $(ls "$CASES_DIR" |
	sed -n 's/^t-pd-\([0-9][0-9]\)-.*\.sh$/\1/p' | sort -u); do
	n_cases=$((n_cases + 1))
	if grep -q "t-pd-$id-" "$README"; then
		obs "index: PD-$id present"
	else
		obs "index: PD-$id MISSING"
		assert_true 1 "PD-$id is listed in the README index"
	fi
done

index_rows=$(grep -c '^| `t-pd-[0-9][0-9]-' "$README")
obs_kv "case files" "$n_cases"
obs_kv "README index rows" "$index_rows"
assert_eq "$index_rows" "$n_cases" \
	"the index lists exactly the cases that exist"
assert_ge "${n_cases:-0}" "30" "the whole suite is enumerated"
assert_not_contains "$README" "全部 21 个用例" \
	"the stale hard-coded case count is gone"
assert_contains "$README" "libwrap0-dev" \
	"the environment section names the libwrap development package"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
