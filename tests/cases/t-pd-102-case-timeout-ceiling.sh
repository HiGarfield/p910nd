#!/bin/sh
#
# PD-102  [medium]  A case cannot claim an unbounded wall-clock budget.
#
# run-all.sh let any case declare "# case-timeout: N" and passed N straight to
# timeout(1).  There was no ceiling: a typo such as 999999 (or a stale legacy
# value) gave a hung case a wall clock of days and wedged the entire CI job
# with no diagnostic.  PD-58 deliberately allows larger budgets for the
# static-analysis cases, so simply removing the declaration is not an option.
#
# A: a synthetic case asking for 999999 is clamped to 10x the default and
#    says so, while a normal 120s declaration is untouched.
# B: static -- the ceiling, the clamp and the diagnostic are all present.
# C: control -- no real case declares more than the new ceiling.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-102 case-timeout-ceiling

DRIVER="$TESTS_DIR/run-all.sh"
TREE="$CASE_DIR/tree"
mkdir -p "$TREE" || exit 1
# Copy the WORKING tree, not HEAD: the fix under test is not committed yet.
(cd "$REPO_ROOT" && tar cf - --exclude=.git --exclude=build .) |
	(cd "$TREE" && tar xf -)

# Generated with printf, not a heredoc: a literal "# case-timeout: ..." line
# inside THIS case would be picked up by the real-declaration scan in part C.
printf '#!/bin/sh\n# case-timeout: %s\nexit 0\n' 999999 \
	>"$TREE/tests/cases/t-pd-900-clamp-probe.sh"
printf '#!/bin/sh\n# case-timeout: %s\nexit 0\n' 120 \
	>"$TREE/tests/cases/t-pd-901-normal-probe.sh"

PORT=$("$PYTHON" -c 'import socket
s = socket.socket(); s.bind(("127.0.0.1", 0))
print(s.getsockname()[1]); s.close()')
obs "isolated driver port: $PORT"

say "A: an absurd declaration is clamped, a normal one is not"
(cd "$TREE" && TEST_PORT="$PORT" sh tests/run-all.sh PD-900 PD-901) \
	>"$CASE_DIR/driver.out" 2>&1
DRIVER_RC=$?
obs "driver exit: $DRIVER_RC"
grep -E 'clamped|PASS pd-90|FAIL pd-90|FATAL' "$CASE_DIR/driver.out" |
	while read -r l; do obs "driver: $l"; done
assert_eq "$DRIVER_RC" "0" \
	"A the isolated driver still completes successfully"
"$PYTHON" - "$CASE_DIR/driver.out" >"$CASE_DIR/a.out" 2>&1 <<'EOF'
import re, sys
text = open(sys.argv[1], encoding="utf-8").read()
clamped = re.findall(r"pd-(\d+): case-timeout (\d+) clamped to (\d+)", text)
passed = re.findall(r"^PASS (pd-\d+)$", text, re.M)
print("clamped=%s passed=%s" % (clamped, passed))
ok = (len(clamped) == 1 and clamped[0][0] == "900"
      and clamped[0][1] == "999999" and clamped[0][2] == "450"
      and "pd-900" in passed and "pd-901" in passed)
sys.exit(0 if ok else 1)
EOF
assert_eq "$?" "0" \
	"A only pd-900 is clamped to 450 and both cases pass"
assert_str_contains "$(cat "$CASE_DIR/a.out")" \
	"clamped=[('900', '999999', '450')]" \
	"A the clamped declaration and its new limit were really checked"
assert_contains "$CASE_DIR/driver.out" "PASS pd-901" \
	"A the ordinary 120s declaration is not disturbed"

say "B: static -- the ceiling is wired into the driver"
assert_str_contains "$(cat "$DRIVER")" \
	'CASE_TIMEOUT_MAX=$((CASE_TIMEOUT * 10))' \
	"B the ceiling is ten times the default budget"
assert_contains "$DRIVER" 'clamped to $CASE_TIMEOUT_MAX' \
	"B the clamp prints a diagnostic with the effective limit"
assert_contains "$DRIVER" 'case_limit=$CASE_TIMEOUT_MAX' \
	"B and the clamped value is what timeout(1) receives"
assert_contains "$DRIVER" 'PD-102: "# case-timeout: N" used to be' \
	"B the reason is documented"

say "C: control -- no real case exceeds the ceiling"
"$PYTHON" - "$CASES_DIR" >"$CASE_DIR/c.out" 2>&1 <<'EOF'
import glob, os, re, sys
vals = []
for path in glob.glob(os.path.join(sys.argv[1], "t-pd-*.sh")):
    for line in open(path, encoding="utf-8"):
        m = re.match(r"# case-timeout: (\d+)$", line.strip())
        if m:
            vals.append((os.path.basename(path), int(m.group(1))))
ceiling = 45 * 10
over = [(n, v) for n, v in vals if v > ceiling]
print("declarations=%d max=%d ceiling=%d over=%s" %
      (len(vals), max(v for _, v in vals), ceiling, over))
sys.exit(0 if len(vals) >= 3 and not over else 1)
EOF
assert_eq "$?" "0" \
	"C every existing declaration still fits under the ceiling"
assert_str_contains "$(cat "$CASE_DIR/c.out")" "over=[]" \
	"C the maximum was compared against 450"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
