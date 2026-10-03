#!/bin/sh
#
# PD-11  [低]  The pacing timestamp must be normalised with >=.
#
# When the current tv_usec was 900000 and the pace is 100000us the sum was
# exactly 1000000, the strict `>` did not fire, and tv_usec was left out of
# range (a legal timeval is 0..999999) without carrying into tv_sec.  The
# release then came one clock tick early.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-11 pace-normalize

say "A: the source uses the correct comparison"
assert_contains "$REPO_ROOT/p910nd.c" "if (then.tv_usec >= 1000000)" \
	"the normalisation fires when the sum is exactly 1000000"
assert_not_contains "$REPO_ROOT/p910nd.c" "if (then.tv_usec > 1000000)" \
	"the strict comparison is gone"
assert_contains "$REPO_ROOT/p910nd.c" "then.tv_usec -= 1000000" \
	"and the carry is still applied"

say "B: replay the arithmetic for every alignment"
if ! "$CC_BIN" -std=c89 -pedantic -Wall -Wextra -O0 -o "$CASE_DIR/pace_probe" \
	"$TOOLS_DIR/pace_probe.c" 2>"$CASE_DIR/pace.build.log"; then
	sed 's/^/    /' "$CASE_DIR/pace.build.log" | while read -r l; do obs "$l"; done
	assert_true 1 "the pacing probe compiles"
	kill_stray
	exit 1
fi
obs "the probe itself builds warning free as strict C89"
assert_eq "$(count_lines "$CASE_DIR/pace.build.log" 'warning:')" "0" \
	"the probe is C89 clean"

"$CASE_DIR/pace_probe" >"$CASE_DIR/pace.out" 2>&1
probe_rc=$?
sed 's/^/    /' "$CASE_DIR/pace.out" | while read -r l; do obs "$l"; done
obs_kv "probe exit status" "$probe_rc"

assert_contains "$CASE_DIR/pace.out" "VERDICT clean" \
	"the shipped normalisation is in range and on time at every alignment"
assert_not_contains "$CASE_DIR/pace.out" "ILLEGAL" \
	"no alignment leaves tv_usec outside 0..999999"
assert_not_contains "$CASE_DIR/pace.out" "EARLY" \
	"and none releases the pacing timer early"
assert_contains "$CASE_DIR/pace.out" "out-of-range=0 early=0 late=0" \
	"the summary agrees"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
