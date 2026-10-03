#!/bin/sh
#
# Static analysis baseline.
#
# This is not a defect case: it records what the compilers and linters say about
# the shipping source, so the report can state that the findings were logic
# defects rather than something a tool would have caught, and so any later change
# can prove it introduced no new diagnostics.
#
# The case passes as long as every tool ran *and* still reports nothing: the
# point of the baseline is that the number must stay at zero.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-00 static-analysis-baseline
SRC="$REPO_ROOT/p910nd.c"

record() {
	# record <label> <logfile> <status>
	obs_kv "$1 exit status" "$3"
	obs_kv "$1 diagnostics" "$(count_lines "$2" 'warning:\|error:')"
	obs "$1 output:"
	sed 's/^/    /' "$2" 2>/dev/null | head -40 | while read -r l; do obs "$l"; done
	DIAG_TOTAL=$(($(DIAG_TOTAL) + $(count_lines "$2" 'warning:\|error:')))
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
if command -v cppcheck >/dev/null 2>&1; then
	cppcheck --enable=warning,style,performance,portability --quiet \
		--std=c89 "$SRC" >"$CASE_DIR/cppcheck.log" 2>&1
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

obs ""
obs "total diagnostics across every tool: $DIAG_TOTAL"
assert_eq "$DIAG_TOTAL" "0" "the baseline stays at zero"
obs "regression rule: this number must not grow in any later change."

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
