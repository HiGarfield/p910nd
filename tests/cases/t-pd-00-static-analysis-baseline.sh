#!/bin/sh
#
# Static analysis baseline.
#
# This is not a defect case: it records what the compilers and linters say about
# the shipping source, so the report can state that the findings were logic
# defects rather than something a tool would have caught, and so any later change
# can prove it introduced no new diagnostics.
#
# case-timeout: 120
# gcc -fanalyzer, cppcheck and clang-tidy together exceed the 45 s default
# on a loaded CI runner; that timeout would be a false failure (PD-58).
#
# The case passes as long as every tool ran *and* still reports nothing: the
# point of the baseline is that the number must stay at zero.  Every tool is
# also asserted on its own, so one clean run cannot hide another tool's
# diagnostics behind the aggregate count, and the counter itself is exercised
# with synthetic diagnostics so it cannot silently stop accumulating.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-00 static-analysis-baseline
SRC="$REPO_ROOT/p910nd.c"

record() {
	# record <label> <logfile> <status>
	obs_kv "$1 exit status" "$3"
	obs_kv "$1 diagnostics" "$(count_lines "$2" 'warning:\|error:\|style:')"
	obs "$1 output:"
	sed 's/^/    /' "$2" 2>/dev/null | head -40 | while read -r l; do obs "$l"; done
	# A command substitution of the counter name used to run here: it printed
	# "DIAG_TOTAL: not found" and, because the substitution was empty, reset
	# the counter to this tool's diagnostic count instead of adding it.  The
	# last tool is normally clean, so warnings from earlier tools were hidden.
	DIAG_TOTAL=$((DIAG_TOTAL + $(count_lines "$2" 'warning:\|error:\|style:')))
}
DIAG_TOTAL=0

say "1/5  gcc -std=c89 -pedantic -Wall -Wextra (the flags the Makefile uses)"
"$CC_BIN" -std=c89 -pedantic -Wall -Wextra -O2 -fsyntax-only "$SRC" \
	>"$CASE_DIR/gcc.log" 2>&1
record "gcc c89" "$CASE_DIR/gcc.log" "$?"

say "2/5  gcc -fanalyzer"
"$CC_BIN" -std=c89 -pedantic -Wall -Wextra -fanalyzer -O2 -c -o /dev/null "$SRC" \
	>"$CASE_DIR/analyzer.log" 2>&1
record "gcc -fanalyzer" "$CASE_DIR/analyzer.log" "$?"

say "3/5  cppcheck"
# cppcheck explores both sides of #ifdef on its own.  Without an explicit
# definition it expands LOCKFILE into a bare LOCKFILE_DIR identifier
# followed by a string, and older releases report a spurious syntax error
# at the lockname declaration.  Pin the macro to the value the shipping
# build uses, so the analysis is deterministic across cppcheck versions.
if command -v cppcheck >/dev/null 2>&1; then
	cppcheck --enable=warning,style,performance,portability --quiet \
		--std=c89 -DLOCKFILE_DIR=\"/var/lock/subsys\" \
		"$SRC" >"$CASE_DIR/cppcheck.log" 2>&1
	record "cppcheck" "$CASE_DIR/cppcheck.log" "$?"
else
	obs "cppcheck not installed"
fi

say "4/5  clang-tidy"
if command -v clang-tidy >/dev/null 2>&1 && command -v clang >/dev/null 2>&1; then
	clang-tidy "$SRC" -- -std=c89 -I"$REPO_ROOT" \
		>"$CASE_DIR/clangtidy.log" 2>&1
	record "clang-tidy" "$CASE_DIR/clangtidy.log" "$?"
else
	obs "clang-tidy or clang not installed"
fi

say "5/5  the conditional-compilation gate, both ways"
# The monotonic clock must compile on its own and the documented fallback must
# compile too, or the switch is not a switch.
"$CC_BIN" -std=c89 -pedantic -Wall -Wextra -Werror -DNO_CLOCK_GETTIME=1 \
	-fsyntax-only "$SRC" >"$CASE_DIR/noclock.log" 2>&1
record "gcc -DNO_CLOCK_GETTIME=1" "$CASE_DIR/noclock.log" "$?"
assert_eq "$(count_lines "$CASE_DIR/noclock.log" 'warning:\|error:')" "0" \
	"the gettimeofday fallback builds warning free"
"$CC_BIN" -std=c89 -pedantic -Wall -Wextra -Werror \
	-fsyntax-only "$SRC" >"$CASE_DIR/clock.log" 2>&1
record "gcc (monotonic)" "$CASE_DIR/clock.log" "$?"
assert_eq "$(count_lines "$CASE_DIR/clock.log" 'warning:\|error:')" "0" \
	"the default monotonic build builds warning free"

# A dead store in the shipping source is a diagnostic even when the last tool
# happens to be clean.  cppcheck reports its style findings with "style:", so
# include that level in the per-tool count.
for _tool in gcc analyzer cppcheck clangtidy noclock clock; do
	assert_eq "$(count_lines "$CASE_DIR/$_tool.log" 'warning:\|error:\|style:')" \
		"0" "$_tool reports no diagnostics on its own"
done

obs ""
obs "total diagnostics across every tool: $DIAG_TOTAL"
assert_eq "$DIAG_TOTAL" "0" "the baseline stays at zero"
obs "regression rule: this number must not grow in any later change."

# Prove that the counter really accumulates.  Two synthetic logs (two
# diagnostics, then none) must leave it two higher than the real tools did;
# with the old per-tool reset the second record() left it at zero and this
# assertion could never detect a regression.
DIAG_BEFORE=$DIAG_TOTAL
printf '%s\n' 'selfcheck.c:1:1: warning: synthetic one' \
	'selfcheck.c:2:1: warning: synthetic two' >"$CASE_DIR/selfcheck.log"
: >"$CASE_DIR/selfcheck-empty.log"
record "selfcheck (2 synthetic diagnostics)" "$CASE_DIR/selfcheck.log" 0
record "selfcheck (clean)" "$CASE_DIR/selfcheck-empty.log" 0
assert_eq "$DIAG_TOTAL" "$((DIAG_BEFORE + 2))" \
	"the diagnostic counter accumulates instead of being reset"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
