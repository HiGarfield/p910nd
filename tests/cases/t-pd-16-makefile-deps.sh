#!/bin/sh
#
# PD-16  [低]  The build must track header dependencies, and CI must run the
# tests.
#
# $(PROG) used to depend on p910nd.c only, with no -MMD, so editing any of the 26
# headers did not trigger a rebuild.  The 34 cross builds in CI compiled the
# binary and uploaded it as a release artefact without ever running a check.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-16 makefile-deps

"$PYTHON" "$TOOLS_DIR/static_checks.py" --check PD-16 >"$CASE_DIR/check.out" 2>&1
sed 's/^/    /' "$CASE_DIR/check.out" | while read -r l; do obs "$l"; done

assert_contains "$CASE_DIR/check.out" "header-deps  *OK" "generated header dependencies are in place"
assert_contains "$CASE_DIR/check.out" "clean-removes-d  *OK" "make clean removes the .d files"
assert_contains "$CASE_DIR/check.out" "make-test-target  *OK" "make test / make check exist"
assert_contains "$CASE_DIR/check.out" "ci-runs-tests  *OK" "CI runs the suite and the release waits for it"

obs "headers included by p910nd.c: $(grep -c '^#include' "$REPO_ROOT/p910nd.c")"
obs "prerequisites of \$(PROG): $(sed -n 's/^\$(PROG):[ \t]*//p' "$REPO_ROOT/Makefile")"
obs ".PHONY: $(sed -n 's/^\.PHONY:[ \t]*//p' "$REPO_ROOT/Makefile")"
obs "CI steps that run make: $(grep -c 'make ' "$REPO_ROOT/.github/workflows/CI.yml")"
obs "CI steps that run the suite: $(grep -cE 'make (test|check)|run-all\.sh' \
	"$REPO_ROOT/.github/workflows/CI.yml")"

# The mechanism has to actually work, so prove it in a scratch copy with a real
# local header: build, change the header, and make must plan a rebuild.
say "rebuild behaviour in a scratch copy of the build"
mkdir -p "$CASE_DIR/mk"
cp "$REPO_ROOT/Makefile" "$REPO_ROOT/p910nd.c" "$CASE_DIR/mk/" || exit 1
printf '#define SCRATCH_HDR 1\n' >"$CASE_DIR/mk/scratch.h"
sed -i '1i #include "scratch.h"' "$CASE_DIR/mk/p910nd.c"
# LC_ALL=C so make's own wording is English on any locale
if LC_ALL=C make -C "$CASE_DIR/mk" p910nd >"$CASE_DIR/mk/build.log" 2>&1; then
	obs "scratch build succeeded"
	obs "generated dependency file:"
	sed 's/^/    /' "$CASE_DIR/mk/p910nd.d" | while read -r l; do obs "$l"; done
	assert_contains "$CASE_DIR/mk/p910nd.d" "scratch.h" \
		"the header shows up in the generated .d file"
	obs "after touching the header, make says:"
	touch "$CASE_DIR/mk/scratch.h"
	LC_ALL=C make -C "$CASE_DIR/mk" -n p910nd >"$CASE_DIR/mk/dryrun.log" 2>&1
	sed 's/^/    /' "$CASE_DIR/mk/dryrun.log" | while read -r l; do obs "$l"; done
	assert_not_contains "$CASE_DIR/mk/dryrun.log" "up to date" \
		"make plans a rebuild after the header changed"
	assert_contains "$CASE_DIR/mk/dryrun.log" "p910nd.c" \
		"and the recipe still compiles only the source"
	assert_not_contains "$CASE_DIR/mk/dryrun.log" "scratch.h " \
		"the header is a prerequisite, not a compilation input"
else
	obs "scratch build failed:"
	sed 's/^/    /' "$CASE_DIR/mk/build.log" | while read -r l; do obs "$l"; done
	assert_true 1 "the scratch build succeeded"
fi

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
