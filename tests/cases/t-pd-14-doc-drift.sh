#!/bin/sh
#
# PD-14  [低]  Documentation must match the program.
#
#   * all four files that state a version must state the same one
#   * p910nd.8 must not tell the administrator to install files that do not
#     exist (it used to name client.pl, banner.pl and p910nd.sh)
#   * every -D knob the source defines must be documented
#   * aux/p910nd.init must not call a command that does not exist

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-14 doc-drift

"$PYTHON" "$TOOLS_DIR/static_checks.py" --check PD-14 >"$CASE_DIR/check.out" 2>&1
sed 's/^/    /' "$CASE_DIR/check.out" | while read -r l; do obs "$l"; done

assert_contains "$CASE_DIR/check.out" "version-drift  *OK" "all four files agree on one version"
assert_contains "$CASE_DIR/check.out" "man-missing-refs  *OK" "the man page cites no missing files"
assert_contains "$CASE_DIR/check.out" "knobs-documented  *OK" "every build-time knob is documented"
assert_contains "$CASE_DIR/check.out" "init-checkproc  *OK" "the init script calls no phantom command"

obs "version strings as they appear:"
for f in p910nd.c p910nd.8 README.md aux/p910nd.spec; do
	obs "    $f: $(grep -m1 -iE 'version' "$REPO_ROOT/$f" | tr -s ' \t' ' ')"
done

obs "files the man page used to tell you to install:"
for f in client.pl banner.pl p910nd.sh; do
	if grep -q -- "$f" "$REPO_ROOT/p910nd.8"; then
		obs "    $f: STILL REFERENCED"
	else
		obs "    $f: no longer referenced"
	fi
done

say "build-time knobs: defined in p910nd.c, documented in p910nd.8"
missing=0
for knob in $(grep -o '^#ifndef[[:space:]]*[A-Z][A-Z0-9_]*' "$REPO_ROOT/p910nd.c" |
	awk '{print $2}' | sort -u); do
	# TESTING is the build-time switch the harness uses, not a tuning knob
	case "$knob" in TESTING) continue ;; esac
	if ! grep -q -- "$knob" "$REPO_ROOT/p910nd.8"; then
		obs "    $knob: MISSING from p910nd.8"
		missing=$((missing + 1))
	fi
done
assert_eq "$missing" "0" "no knob is undocumented"

obs "aux/p910nd.init checkproc implementations:"
grep -n 'checkproc()' "$REPO_ROOT/aux/p910nd.init" | sed 's/^/    /' |
	while read -r l; do obs "$l"; done
assert_not_contains "$REPO_ROOT/aux/p910nd.init" "return status " \
	"no branch calls a bare 'status'"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
