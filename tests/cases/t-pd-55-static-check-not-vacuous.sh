#!/bin/sh
#
# PD-55  [medium]  A static check must be able to fail.
#
# check_pd01()'s "makefile-lockdir-optional" assertion reported OK in BOTH
# branches.  Uncommenting `override CFLAGS += -DLOCKFILE_DIR=...` in the
# Makefile -- which hard-codes a possibly unwritable lock directory for
# every build and breaks the lock-path cases -- would therefore still make
# PD-01's static check print OK and exit 0.  A check that cannot fail is
# worse than no check.
#
# A: with the shipped Makefile the check says OK, exit 0.
# B: with the override uncommented it must say DEFECT, exit 1.
# C: AST scan -- no if/else in static_checks.py reports OK on both sides.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-55 static-check-not-vacuous

SC="$TOOLS_DIR/static_checks.py"
FIX="$CASE_DIR/repo"
mkdir -p "$FIX"
cp "$REPO_ROOT/p910nd.c" "$REPO_ROOT/Makefile" "$FIX/"

run_check() {
	"$PYTHON" "$SC" --check PD-01 --repo "$FIX" \
		>"$CASE_DIR/$1.out" 2>&1
	CHECK_RC=$?
	obs "$1 output: $(tr '\n' '|' <"$CASE_DIR/$1.out")"
	obs_kv "$1 exit" "$CHECK_RC"
}

say "A: the shipped Makefile leaves the override commented out"
run_check control
assert_eq "$CHECK_RC" "0" "A the PD-01 static check passes on the real tree"
assert_contains "$CASE_DIR/control.out" "makefile-lockdir-optional" \
	"A the lockdir check really ran"
assert_not_contains "$CASE_DIR/control.out" "DEFECT" \
	"A and reports no defect"

say "B: uncomment the override -- the check must notice"
"$PYTHON" - "$FIX/Makefile" <<'EOF'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
old = "#override CFLAGS += -DLOCKFILE_DIR"
assert s.count(old) == 1, "PATCH_NEEDLE_MISSED"
open(p, "w", encoding="utf-8").write(s.replace(old, "override CFLAGS += -DLOCKFILE_DIR"))
EOF
assert_eq "$?" "0" "the mutation fixture was prepared"
run_check forced
assert_eq "$CHECK_RC" "1" \
	"B forcing -DLOCKFILE_DIR makes the static check fail"
assert_contains "$CASE_DIR/forced.out" "makefile-lockdir-optional" \
	"B the finding names the check"
assert_contains "$CASE_DIR/forced.out" "DEFECT" \
	"B and is reported as a defect, not OK"

say "C: AST -- no vacuous if/else remains"
"$PYTHON" - "$SC" >"$CASE_DIR/ast.out" 2>&1 <<'EOF'
import ast, sys
src = open(sys.argv[1], encoding="utf-8").read()
tree = ast.parse(src)

def verdicts(stmts):
    out = []
    mod = ast.Module(body=stmts, type_ignores=[])
    for n in ast.walk(mod):
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) \
                and n.func.attr == "add" and len(n.args) >= 3 \
                and isinstance(n.args[2], ast.Name):
            out.append(n.args[2].id)
    return out

bad = []
for n in ast.walk(tree):
    if isinstance(n, ast.If) and n.orelse:
        a, b = verdicts(n.body), verdicts(n.orelse)
        if a and b and set(a) == {"OK"} and set(b) == {"OK"}:
            bad.append(n.lineno)
print("vacuous_if_else_lines=%s" % bad)
sys.exit(0 if not bad else 1)
EOF
ast_rc=$?
obs "AST scan: $(tr '\n' '|' <"$CASE_DIR/ast.out")"
obs_kv "AST scan exit" "$ast_rc"
assert_eq "$ast_rc" "0" \
	"C no if/else in static_checks.py reports OK on both branches"
assert_contains "$CASE_DIR/ast.out" 'vacuous_if_else_lines=\[\]' \
	"C the scan found no vacuous assertion"

assert_contains "$SC" "the override must stay optional" \
	"C the forced-override branch now reports a defect"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
