#!/bin/sh
#
# PD-81  [medium]  B-3's recoverable stall budget has no regression guard.
#
# DEFECT_REPORT.md B-3 reports "stall_extended never resets, so the
# recoverable budget is granted only once; the abandonment log always lies
# about 240 s" as fixed, but its case column is "—" and no case mentions
# stall_extended at all.  Deleting a reset would keep the whole suite green.
#
# A: both copy loops declare, reset and re-grant the extension, and both
#    abandonment logs print the measured stall instead of a fixed number.
# B: self-mutation -- removing one reset must make the A checks fail, so
#    the guard is proven non-vacuous (PD-55).
# C: the report points at this case instead of claiming no case exists.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-81 stall-budget-reset

SRC="$REPO_ROOT/p910nd.c"
REPORT="$TESTS_DIR/DEFECT_REPORT.md"
CHECK="$CASE_DIR/check.py"

cat >"$CHECK" <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
decls = len(re.findall(r'int stall_extended\s*=', s))
resets = len(re.findall(r'stall_extended = 0;', s))
ext = len(re.findall(r'if \(!stall_extended\)', s))
measured = len(re.findall(r'stalled, networkToPrinterBuffer\.bytes\);', s))
fixed = len(re.findall(r'abandoning job.*240', s))
print("decls=%d resets=%d extensions=%d measured=%d fixed=%d" %
      (decls, resets, ext, measured, fixed))
sys.exit(0 if decls == 2 and resets >= 4 and ext == 2 and
         measured == 2 and fixed == 0 else 1)
EOF

say "A: the recoverable budget resets in both copy loops"
"$PYTHON" "$CHECK" "$SRC" >"$CASE_DIR/a.out" 2>&1
a_rc=$?
obs "source scan: $(cat "$CASE_DIR/a.out")"
assert_eq "$a_rc" "0" "A both loops reset and re-grant the extension"
assert_contains "$CASE_DIR/a.out" "resets=4" \
	"A four reset sites exist (progress and empty buffer, twice)"
assert_contains "$CASE_DIR/a.out" "measured=2" \
	"A both abandonment logs report the measured stall"

say "B: self-mutation proves the guard is not vacuous"
"$PYTHON" - "$SRC" "$CASE_DIR/mutated.c" <<'EOF'
import sys
s = open(sys.argv[1], encoding="utf-8").read()
needle = "stall_extended = 0;"
assert s.count(needle) >= 4, "the source no longer has four resets"
open(sys.argv[2], "w", encoding="utf-8").write(
    s.replace(needle, "; /* reset removed */", 1))
EOF
"$PYTHON" "$CHECK" "$CASE_DIR/mutated.c" >"$CASE_DIR/b.out" 2>&1
b_rc=$?
obs "mutated scan: $(cat "$CASE_DIR/b.out")"
assert_eq "$b_rc" "1" \
	"B dropping one reset makes the check fail"
assert_contains "$CASE_DIR/b.out" "resets=3" \
	"B the scan really counted the missing reset"

say "C: the report records the regression case"
assert_not_contains "$REPORT" '| B-3 | `stall_extended` 随进度复位；日志改打实测秒数 | — |' \
	"C B-3 no longer claims to have no case"
assert_contains "$REPORT" "t-pd-81" \
	"C and names this case"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
