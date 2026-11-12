#!/bin/sh
# case-timeout: 180
#
# PD-106  [high]  An assertion must read the status it claims to check.
#
# t-pd-43 ran its flush probe, then called obs() (a printf) and asserted on
# "$?".  obs() always succeeds, so "the normal flush still reports ..." was
# an assertion that could never fail no matter how the probe exited.  A
# static scan of all cases found exactly this one instance; PD-105's first
# draft had the same shape and was corrected before its commit.
#
# A: no case asserts on "$?" after a logging/assignment command.
# B: with a deliberately failing probe, PD-43's assertion must report FAIL.
# C: control -- with a healthy probe the same assertion reports PASS.
# D: static -- PD-43 stores the probe status in a named variable.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-106 assert-exit-code

CASE43="$CASES_DIR/t-pd-43-flush-sink-count.sh"
TREE="$CASE_DIR/tree"
mkdir -p "$TREE" || exit 1
(cd "$REPO_ROOT" && tar cf - --exclude=.git --exclude=build .) |
	(cd "$TREE" && tar xf -)
OBS="$TREE/build/tests/observations/PD-43.txt"
PROBE_C="$TREE/tests/tools/flush_probe.c"
cp "$PROBE_C" "$CASE_DIR/flush_probe.orig.c"

say "A: no assertion reads a status overwritten by logging"
"$PYTHON" - "$CASES_DIR" >"$CASE_DIR/a.out" 2>&1 <<'EOF'
import glob, os, re, sys
suspect = []
for path in sorted(glob.glob(os.path.join(sys.argv[1], "t-pd-*.sh"))):
    lines = open(path, encoding="utf-8").read().splitlines()
    for i, line in enumerate(lines):
        if not re.search(r'assert_(?:eq|true|ge|lt|f_ge|f_lt)\s+"\$\?"', line):
            continue
        j = i - 1
        while j >= 0 and (not lines[j].strip()
                          or lines[j].lstrip().startswith("#")
                          or lines[j].rstrip().endswith("\\")):
            j -= 1
        prev = lines[j].strip() if j >= 0 else ""
        if re.match(r"^(obs|say|obs_kv|printf|echo)\b", prev) or \
           re.match(r"^\w+=", prev):
            suspect.append("%s:%d" % (os.path.basename(path), i + 1))
print("cases=%d suspect=%s" %
      (len(glob.glob(os.path.join(sys.argv[1], "t-pd-*.sh"))), suspect))
sys.exit(0 if not suspect else 1)
EOF
assert_eq "$?" "0" \
	"A every \"\$?\" assertion follows the command it checks"
assert_str_contains "$(cat "$CASE_DIR/a.out")" "suspect=[]" \
	"A the scan really looked at the whole case set"

say "B: a failing probe must make that assertion fail"
printf 'int main(void) { return 1; }\n' >"$PROBE_C"
(cd "$TREE" && sh tests/cases/t-pd-43-flush-sink-count.sh) \
	>"$CASE_DIR/b-run.log" 2>&1
BAD_RC=$?
obs "failing-probe case exit: $BAD_RC"
obs "failing-probe assertion: $(grep 'the normal flush still reports' "$OBS" | head -1)"
[ "$BAD_RC" -ne 0 ]
assert_true "$?" \
	"B PD-43 fails when its probe fails"
assert_eq "$(count_lines "$OBS" 'ASSERT FAIL  the normal flush still reports')" "1" \
	"B and the probe-status assertion specifically reports FAIL"

say "C: control -- a healthy probe keeps PD-43 green"
cp "$CASE_DIR/flush_probe.orig.c" "$PROBE_C"
(cd "$TREE" && sh tests/cases/t-pd-43-flush-sink-count.sh) \
	>"$CASE_DIR/c-run.log" 2>&1
GOOD_RC=$?
obs "healthy case exit: $GOOD_RC"
assert_eq "$GOOD_RC" "0" \
	"C PD-43 passes with the real probe"
assert_eq "$(count_lines "$OBS" 'ASSERT PASS  the normal flush still reports')" "1" \
	"C and that same assertion reports PASS"
assert_eq "$(count_lines "$OBS" 'ASSERT FAIL')" "0" \
	"C with no assertion failing at all"

say "D: static -- the status is saved before logging"
assert_str_contains "$(cat "$CASE43")" 'pd36_rc=$?' \
	"D PD-43 saves the probe status in a named variable"
assert_str_contains "$(cat "$CASE43")" 'assert_eq "$pd36_rc" "0"' \
	"D and asserts on that saved value"
assert_contains "$CASE43" "PD-106: save the probe's status before logging" \
	"D the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
