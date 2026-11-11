#!/bin/sh
#
# PD-31  [high]  CI must install every dependency the suite compiles against.
#
# PD-28 builds a USE_LIBWRAP binary: p910nd.c includes tcpd.h and links
# -lwrap, both provided by libwrap0-dev.  The CI job installed only strace,
# python3, cppcheck and clang-tidy, so on ubuntu-latest that case could not
# even build and `make test` failed there while passing on a developer box.
#
# The case checks the declared packages and then compiles the exact variant
# PD-28 needs, so a missing development package cannot hide behind the local
# machine already having it.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-31 ci-deps

CI="$REPO_ROOT/.github/workflows/CI.yml"
obs "CI install step:"
grep -n 'apt-get install' "$CI" | sed 's/^/    /' |
	while read -r l; do obs "$l"; done

assert_contains "$CI" "libwrap0-dev" \
	"CI installs the package that provides tcpd.h and libwrap.so"
assert_contains "$CI" "strace" "CI still installs strace"
assert_contains "$CI" "python3" "CI still installs python3"
assert_contains "$CI" "cppcheck" "CI still installs cppcheck"
assert_contains "$CI" "clang-tidy" "CI still installs clang-tidy"

# Integration half: the USE_LIBWRAP build PD-28 relies on must succeed.
LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR"
BIN=$(build_variant libwrap-ci "-DLOCKFILE_DIR=\"$LOCKDIR\"" \
	-DUSE_LIBWRAP -lwrap)
assert_file_exists "$BIN" \
	"the USE_LIBWRAP variant builds with the dependencies CI declares"
assert_contains "$REPO_ROOT/tests/cases/t-pd-28-inetd-libwrap.sh" \
	"-DUSE_LIBWRAP" "PD-28 really does need that package"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
