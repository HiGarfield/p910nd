#!/bin/sh
#
# PD-90  [low]  A failed source copy must not leave a scratch directory.
#
# tests/variants.sh patched_source() created its src.XXXXXX directory and
# then did `cp "$P910ND_SRC" "$_dir/p910nd.c" || return 1`.  When the source
# copy was missing/unreadable (a stale mutation fixture, a typo in
# P910ND_SRC), the empty directory stayed behind.  run-all.sh only cleans
# src.* on EXIT of the WHOLE driver, so single-case runs accumulated them
# and the build tree was not left as clean as the harness claims.
# Measured: build_rc=1, src.* count 0 -> 1.
#
# A: a failing copy leaves the directory count unchanged.
# B: control -- a successful variant build also leaves no directory.
# C: static -- the failure branch removes the directory it created.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-90 variant-src-leak

VARIANTS="$TESTS_DIR/variants.sh"

count_src_dirs() {
	# shellcheck disable=SC2086
	ls -d "$BUILD_DIR"/src.* 2>/dev/null | wc -l | tr -d ' '
}

say "A: an unreadable P910ND_SRC leaves no scratch directory"
_before=$(count_src_dirs)
_saved=$P910ND_SRC
P910ND_SRC="$CASE_DIR/no-such-source.c"
_out=$(build_variant test-leakcheck 2>/dev/null)
_rc=$?
P910ND_SRC=$_saved
_after=$(count_src_dirs)
obs_kv "src dirs before failed build" "$_before"
obs_kv "src dirs after failed build" "$_after"
obs_kv "build_variant exit" "$_rc"
assert_eq "$_rc" "1" \
	"A the broken source is rejected instead of building a stale binary"
assert_eq "$_after" "$_before" \
	"A no scratch directory is left behind"
assert_eq "${_out:-}" "" \
	"A and no variant path is echoed for a failed build"

say "B: control -- a good build also leaves the tree clean"
_before=$(count_src_dirs)
_out=$(build_variant test-leakok 2>/dev/null)
_rc=$?
_after=$(count_src_dirs)
obs_kv "successful variant path" "${_out:-missing}"
obs_kv "src dirs after good build" "$_after"
assert_eq "$_rc" "0" \
	"B a normal variant still builds"
assert_file_exists "${_out:-$CASE_DIR/missing}" \
	"B and its binary exists"
assert_eq "$_after" "$_before" \
	"B with no scratch directory left over either"

say "C: static -- the failure path cleans up"
assert_contains "$VARIANTS" 'cp "$P910ND_SRC" "$_dir/p910nd.c" || { rm -rf "$_dir"; return 1; }' \
	"C the copy-failure branch removes the directory"
assert_contains "$VARIANTS" "PD-90: a missing/unreadable P910ND_SRC" \
	"C the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
