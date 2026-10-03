#!/bin/sh
#
# PD-15  [低]  version[] and copyright[] must be const.
#
# The same file already fixed the identical hazard for the program name (D15:
# "the fallback name is a writable array, not a string literal"), but these two
# were left as writable storage with no reason to be.  -Wwrite-strings does not
# catch it -- it only const-ifies string literals, not initialised char[] arrays
# -- which is why this needed a person.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-15 version-const

"$PYTHON" "$TOOLS_DIR/static_checks.py" --check PD-15 >"$CASE_DIR/check.out" 2>&1
sed 's/^/    /' "$CASE_DIR/check.out" | while read -r l; do obs "$l"; done
assert_contains "$CASE_DIR/check.out" "version-arrays-const  *OK" \
	"version[] and copyright[] are const"

obs "declarations in question:"
grep -n 'static \(const \)\?char \(version\|copyright\|default_progname\)\[' \
	"$REPO_ROOT/p910nd.c" | sed 's/^/    /' | while read -r l; do obs "$l"; done
assert_contains "$REPO_ROOT/p910nd.c" "const char version" "version[] is const"
assert_contains "$REPO_ROOT/p910nd.c" "const char copyright" "copyright[] is const"

say "the toolchain is happy either way, which is why this needed a person"
"$CC_BIN" -std=c89 -pedantic -Wall -Wextra -Wwrite-strings -fsyntax-only \
	"$REPO_ROOT/p910nd.c" >"$CASE_DIR/syntax.out" 2>&1
obs_kv "gcc -Wwrite-strings -fsyntax-only diagnostics" \
	"$(count_lines "$CASE_DIR/syntax.out" 'warning:')"
obs "note: -Wwrite-strings only const-ifies literals; an initialised char[]"
obs "      array is unaffected either way, so the declaration itself is the test."

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
