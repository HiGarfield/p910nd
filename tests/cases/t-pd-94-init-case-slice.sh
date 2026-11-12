#!/bin/sh
#
# PD-94  [medium]  Static assertions must slice exactly the case branch.
#
# t-pd-12 and t-pd-68 extracted the init script's stop branch with
#   sed -n '/^    stop)/,/^    ;;/p'
# but every case terminator in aux/p910nd.init is indented with a TAB.
# The end pattern never matched, so sed printed from `stop)` all the way to
# EOF: 185 lines instead of the real 95, including restart, status,
# force-reload and reload.  Assertions about "the stop branch" could
# therefore be satisfied by text from a completely different branch -- the
# same false-green class as PD-55.
#
# A: execute each case's real sed expression and compare it with an exact
#    Python slice (same line count, no other case labels).
# B: static -- both cases use a whitespace-tolerant terminator.
# C: control -- the exact slice is non-empty and still holds the stop logic.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-94 init-case-slice

INIT="$REPO_ROOT/aux/p910nd.init"
C12="$CASES_DIR/t-pd-12-init-stop-match.sh"
C68="$CASES_DIR/t-pd-68-init-stop-no-pidfile.sh"

say "A: each case's sed expression matches the real stop branch"
"$PYTHON" - "$INIT" "$C12" "$C68" >"$CASE_DIR/a.out" 2>&1 <<'EOF'
import os, re, subprocess, sys
init, cases = sys.argv[1], sys.argv[2:]
s = open(init, encoding="utf-8").read()
start = s.index("    stop)")
m = re.search(r"^\s*;;\s*$", s[start:], re.M)
assert m, "no case terminator after stop)"
# sed's range INCLUDES the matching terminator line; m.end() covers it.
real = s[start:start + m.end()]
real_n = len(real.splitlines())
labels = re.compile(r"^    (restart|status|try-restart|force-reload|reload)\b")
bad = 0
for path in cases:
    text = open(path, encoding="utf-8").read()
    mm = re.search(r"sed -n '([^']+)' \"\$INIT\"", text)
    if not mm:
        print("%s no-sed-expression" % os.path.basename(path))
        bad += 1
        continue
    out = subprocess.run(["sed", "-n", mm.group(1), init],
                         capture_output=True, text=True).stdout
    n = len(out.splitlines())
    other = sum(1 for line in out.splitlines() if labels.match(line))
    print("%s lines=%d real=%d other_labels=%d" %
          (os.path.basename(path), n, real_n, other))
    if n != real_n or other != 0:
        bad += 1
print("real_lines=%d bad=%d" % (real_n, bad))
sys.exit(0 if real_n > 50 and bad == 0 else 1)
EOF
assert_eq "$?" "0" \
	"A both slices end at the real case terminator"
assert_contains "$CASE_DIR/a.out" "bad=0" \
	"A no slice overflowed into another branch"
"$PYTHON" - "$CASE_DIR/a.out" <<'EOF'
import re, sys
t = open(sys.argv[1], encoding="utf-8").read()
pairs = re.findall(r"lines=(\d+) real=(\d+) other_labels=(\d+)", t)
sys.exit(0 if len(pairs) == 2 and
         all(a == b and c == "0" for a, b, c in pairs) else 1)
EOF
assert_eq "$?" "0" \
	"A both case files were actually examined"

say "B: static -- whitespace-tolerant terminators"
"$PYTHON" - "$C12" "$C68" >"$CASE_DIR/b.out" 2>&1 <<'EOF'
import os, sys
bad = 0
for path in sys.argv[1:]:
    t = open(path, encoding="utf-8").read()
    ok = ("[[:space:]]*;;" in t and "/^    ;;/p" not in t
          and "PD-94" in t)
    print("%s clean=%s" % (os.path.basename(path), ok))
    bad += 0 if ok else 1
sys.exit(0 if bad == 0 else 1)
EOF
assert_eq "$?" "0" \
	"B both cases use a TAB-tolerant terminator and no space-only one"
assert_eq "$(count_lines "$CASE_DIR/b.out" 'clean=True')" "2" \
	"B both case files were actually checked"

say "C: control -- the exact slice still contains the stop logic"
stop_branch=$(sed -n '/^    stop)/,/^[[:space:]]*;;/p' "$INIT")
printf '%s\n' "$stop_branch" >"$CASE_DIR/branch.txt"
assert_str_contains "$stop_branch" "p910nd_running_pids" \
	"C the shared pid helper is in the slice"
assert_str_contains "$stop_branch" '"$P910ND_PIDDIR"/p910[0-9]d.pid' \
	"C and so is the configured pid directory"
assert_not_contains "$CASE_DIR/branch.txt" '^    restart)' \
	"C no later case branch leaked in"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
