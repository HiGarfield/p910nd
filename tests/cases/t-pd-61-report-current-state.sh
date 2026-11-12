#!/bin/sh
#
# PD-61  [medium]  The defect report's present-tense claims must match the
# code.
#
# DEFECT_REPORT.md was written at the 16-defect baseline but kept asserting
# current behaviour in the present tense after later fixes landed:
#   * "`make test`（17 个用例 ... 约 187 秒）现在 ..." -- 60+ cases today
#   * "mono_now() 从不失败：... 内部退回 gettimeofday()" -- PD-40 latched
#     the clock source and converts wall time into the monotonic base
#   * "全部 36 处计时点" -- a hard-coded count that drifts on every change
#   * "lpname[13] vs 9" -- lpname is 256 bytes and snprintf is checked
# A stale report misleads anyone debugging a timeout, a truncation or a
# buffer-limit question.
#
# A: no stale present-tense statements remain.
# B: the current facts are stated and agree with p910nd.c.
# C: the live regression-rule block is untouched.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-61 report-current-state

REPORT="$TESTS_DIR/DEFECT_REPORT.md"
SRC="$REPO_ROOT/p910nd.c"

say "A: stale present-tense claims are gone"
assert_not_contains "$REPORT" "17 个用例，连跑两轮全绿" \
	"A the old case count is no longer presented as current"
assert_not_contains "$REPORT" "mono_now() 从不失败" \
	"A the obsolete unconditional gettimeofday fallback is gone"
assert_not_contains "$REPORT" "全部 36 处计时点" \
	"A the hard-coded timing-site count is gone"
assert_not_contains "$REPORT" "lpname[13]" \
	"A the obsolete 13-byte device path buffer is gone"

say "B: current facts are documented and match the source"
assert_contains "$REPORT" "锁定时钟源" \
	"B the report describes the latched clock source"
assert_contains "$REPORT" "wall - monotonic" \
	"B and the offset used on a transient monotonic failure"
assert_contains "$SRC" "wall_offset" \
	"B p910nd.c really implements that offset"
assert_contains "$REPORT" 'lpname\[256\]' \
	"B the report states the current device path buffer"
assert_contains "$SRC" 'static char lpname\[256\]' \
	"B and the source agrees"
assert_contains "$REPORT" "PD-43" \
	"B the sink-buffer double-count fix is reflected"
assert_contains "$SRC" "discarded += (int)n" \
	"B and its per-pass accounting is in the source"
assert_contains "$REPORT" "用例清单见 `tests/README.md`" \
	"B the case count is delegated to the living index"

say "C: the live regression rule still exists"
sed -n '/^## 回归规则/,/^静态分析基线/p' "$REPORT" >"$CASE_DIR/rules.txt"
obs "rules:"
sed 's/^/    /' "$CASE_DIR/rules.txt" | while read -r l; do obs "$l"; done
assert_contains "$CASE_DIR/rules.txt" "make test" \
	"C the rule still requires the full suite"
assert_contains "$CASE_DIR/rules.txt" "sh tests/mutation-check.sh" \
	"C and the sensitivity check"
assert_contains "$CASE_DIR/rules.txt" "make && make CC=clang" \
	"C and the warning-free dual build"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
