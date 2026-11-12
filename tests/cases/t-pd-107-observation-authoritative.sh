#!/bin/sh
# case-timeout: 180
#
# PD-107  [high]  exit 0 must not override a failing assertion.
#
# run-all.sh judged a case solely by its exit status.  Every real case ends
# with `[ "$ASSERT_FAIL" -eq 0 ] || exit 1`, but that gate is easy to forget
# and, as PD-43/PD-105 showed, "$?" can be clobbered by a logging command so
# an assertion silently stops counting.  A synthetic case that writes
# "ASSERT FAIL" and then exits 0 was reported as `PASS pd-900` with a green
# suite (measured on the unfixed driver: exit 0, failed: none).
#
# A: such a case is reported as failed and the suite exits non-zero.
# B: control -- a clean case still passes.
# C: static -- the observation record is consulted on the exit-0 path.
# D: no real case writes a literal ASSERT FAIL into its observations.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-107 observation-authoritative

DRIVER="$TESTS_DIR/run-all.sh"
TREE="$CASE_DIR/tree"
mkdir -p "$TREE" || exit 1
(cd "$REPO_ROOT" && tar cf - --exclude=.git --exclude=build .) |
	(cd "$TREE" && tar xf -)

cat >"$TREE/tests/cases/t-pd-900-vacuous.sh" <<'EOF'
#!/bin/sh
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"
case_init PD-900 vacuous
assert_eq "1" "2" "an assertion that really fails"
exit 0
EOF
cat >"$TREE/tests/cases/t-pd-901-clean.sh" <<'EOF'
#!/bin/sh
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"
case_init PD-901 clean
assert_eq "1" "1" "an assertion that really passes"
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
EOF

PORT=$("$PYTHON" -c 'import socket
s = socket.socket(); s.bind(("127.0.0.1", 0))
print(s.getsockname()[1]); s.close()')

say "A: a green exit with a failing assertion is rejected"
(cd "$TREE" && TEST_PORT="$PORT" sh tests/run-all.sh PD-900) \
	>"$CASE_DIR/vacuous.out" 2>&1
VAC_RC=$?
obs "vacuous driver exit: $VAC_RC"
grep -E 'PASS pd-900|FAIL pd-900|failed :' "$CASE_DIR/vacuous.out" |
	while read -r l; do obs "vacuous: $l"; done
[ "$VAC_RC" -ne 0 ]
assert_true "$?" \
	"A the suite exits non-zero"
assert_contains "$CASE_DIR/vacuous.out" "FAIL pd-900" \
	"A the case is reported as failed"
assert_contains "$CASE_DIR/vacuous.out" "pd-900(vacuous-pass)" \
	"A with a reason in the summary"
assert_not_contains "$CASE_DIR/vacuous.out" "PASS pd-900" \
	"A and is never counted as passed"
assert_contains "$TREE/build/tests/observations/PD-900.txt" \
	"ASSERT FAIL  an assertion that really fails" \
	"A the underlying observation really did fail"

say "B: control -- a clean case still passes"
(cd "$TREE" && TEST_PORT="$PORT" sh tests/run-all.sh PD-901) \
	>"$CASE_DIR/clean.out" 2>&1
assert_eq "$?" "0" \
	"B the clean case exits 0"
assert_contains "$CASE_DIR/clean.out" "PASS pd-901" \
	"B and is reported as passing"
assert_not_contains "$CASE_DIR/clean.out" "vacuous-pass" \
	"B with no false accusation"

say "C: static -- exit 0 is cross-checked against the record"
assert_contains "$DRIVER" "grep -q '\^ASSERT FAIL'" \
	"C the driver scans the observation record"
assert_contains "$DRIVER" "vacuous-pass" \
	"C and names the failure mode"
assert_contains "$DRIVER" "PD-107: the observation record is authoritative" \
	"C the reason is documented"
"$PYTHON" - "$DRIVER" >"$CASE_DIR/c.out" 2>&1 <<'EOF'
import sys
s = open(sys.argv[1], encoding="utf-8").read()
gate = s.find('if [ "$status" -eq 0 ]; then')
check = s.find("grep -q '^ASSERT FAIL'")
fail = s.find('FAILED="$FAILED $lid(vacuous-pass)"')
print("gate=%d check=%d fail=%d ordered=%s" %
      (gate, check, fail, 0 <= gate < check < fail))
sys.exit(0 if 0 <= gate < check < fail else 1)
EOF
assert_eq "$?" "0" \
	"C the check is inside the exit-0 branch"

say "D: no real case emits a literal failure line"
"$PYTHON" - "$CASES_DIR" >"$CASE_DIR/d.out" 2>&1 <<'EOF'
import glob, os, sys
hits = []
for path in glob.glob(os.path.join(sys.argv[1], "t-pd-*.sh")):
    for n, line in enumerate(open(path, encoding="utf-8"), start=1):
        stripped = line.strip()
        if stripped.startswith('obs "ASSERT FAIL') or \
           stripped.startswith('printf') and 'ASSERT FAIL' in stripped:
            hits.append("%s:%d" % (os.path.basename(path), n))
print("cases=%d literal_failure_writers=%s" %
      (len(glob.glob(os.path.join(sys.argv[1], "t-pd-*.sh"))), hits))
sys.exit(0 if not hits else 1)
EOF
assert_eq "$?" "0" \
	"D the new guard cannot misfire on a real case"
assert_str_contains "$(cat "$CASE_DIR/d.out")" "literal_failure_writers=[]" \
	"D the scan covered the whole case set"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
