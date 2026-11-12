#!/bin/sh
#
# PD-65  [low]  The harness README's current-state claims drifted.
#
# tests/README.md still said "every case verifies one defect from
# DEFECT_REPORT.md" (that report covers 16 + 12 entries; the index has 60+
# cases including harness, CI and documentation regressions), and stated
# that patched variants always move BASEPORT to 19100 although TEST_PORT is
# overridable.  A maintainer following either statement could look in the
# wrong place or run cases on a colliding port.
#
# A: static -- both claims are corrected and the source agrees.
# B: behaviour -- patch_source.py really honours a non-default port.
# C: the index rule (PD-32) is still intact.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-65 harness-readme-current

README="$TESTS_DIR/README.md"
REPORT="$TESTS_DIR/DEFECT_REPORT.md"
VARIANTS="$TESTS_DIR/variants.sh"

say "A: stale claims are gone and the replacement matches the code"
assert_not_contains "$README" '每个用例验证 `DEFECT_REPORT.md` 中的一条缺陷' \
	"A the false one-defect-per-case claim is gone"
assert_contains "$README" "用例清单以本文件下方的索引为准" \
	"A the living index is named as the authority"
assert_contains "$README" "16 + 12" \
	"A the report's actual scope is stated"
assert_contains "$README" 'TEST_PORT=<port>' \
	"A the port override is documented"
assert_not_contains "$README" '`BASEPORT` 移到 19100' \
	"A the hard-coded 19100 wording is gone"
assert_contains "$VARIANTS" 'TEST_PORT=${TEST_PORT:-19100}' \
	"A variants.sh really defaults TEST_PORT to 19100"
assert_contains "$TOOLS_DIR/patch_source.py" 'BASEPORT {port}' \
	"A patch_source.py really substitutes the requested port"

say "B: behaviour -- a non-default port reaches the patched source"
cp "$REPO_ROOT/p910nd.c" "$CASE_DIR/patched.c"
"$PYTHON" "$TOOLS_DIR/patch_source.py" --profile ports --port 19394 \
	"$CASE_DIR/patched.c" >"$CASE_DIR/patch.log" 2>&1
patch_rc=$?
obs "patch output: $(tr '\n' '|' <"$CASE_DIR/patch.log")"
obs_kv "patch exit" "$patch_rc"
assert_eq "$patch_rc" "0" "B the ports profile applies cleanly"
assert_contains "$CASE_DIR/patched.c" "#define BASEPORT 19394" \
	"B BASEPORT follows the requested port"
assert_not_contains "$CASE_DIR/patched.c" "#define BASEPORT 9100" \
	"B the production port is gone from the test copy"

say "C: the index rule still governs the README"
assert_contains "$README" 't-pd-32' \
	"C the index check is referenced"
assert_eq "$(count_lines "$REPORT" 'No rule to make')" "0" \
	"C no leftover build error text leaked into the report"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
