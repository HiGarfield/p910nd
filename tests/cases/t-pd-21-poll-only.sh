#!/bin/sh
#
# PD-21  [中]  The I/O loop must be driven by poll() alone.
#
# The daemon used to wait for readiness on a fixed-size descriptor bit set,
# which cannot represent a descriptor at or above the compile-time width and so
# capped what a long-running daemon could serve.  The loop now waits with
# poll(), whose only ceiling is the process descriptor table (RLIMIT_NOFILE).
# This case keeps it that way:
#
#   * no call, type, macro or header of the old interface may reappear in
#     p910nd.c (checked with comments masked out);
#   * <poll.h> must stay included and all three wait sites must still be poll()
#     calls (bidirectional copy_stream, unidirectional copy_stream, flush);
#   * the man page, README and packaging files must not name the old interface;
#   * the built binary must reference poll() and not the old call.
#
# The scanner (tests/tools/static_checks.py) is excluded from the text scan
# because it can only look for the name by containing it; every shipped file
# is scanned.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-21 poll-only

"$PYTHON" "$TOOLS_DIR/static_checks.py" --check PD-21 >"$CASE_DIR/check.out" 2>&1
sed 's/^/    /' "$CASE_DIR/check.out" | while read -r l; do obs "$l"; done

assert_contains "$CASE_DIR/check.out" "no-banned-code  *OK" \
	"the source has no call, type, macro or header of the old interface"
assert_contains "$CASE_DIR/check.out" "poll-present  *OK" \
	"the source includes <poll.h> and waits with poll()"
assert_contains "$CASE_DIR/check.out" "no-banned-text  *OK" \
	"the man page and packaging files do not name the old interface"
assert_contains "$CASE_DIR/check.out" "binary-symbols  *OK" \
	"the built binary references poll and not the old call"
assert_contains "$CASE_DIR/check.out" "SUMMARY checks=4 ok=4 not-ok=0" \
	"every PD-21 assertion is OK"

say "wait sites in p910nd.c:"
grep -n 'poll(pfd\|poll(&pfd' "$REPO_ROOT/p910nd.c" |
	sed 's/^/    /' | while read -r l; do obs "$l"; say "    $l"; done
CALLS=$(grep -c 'poll(pfd\|poll(&pfd' "$REPO_ROOT/p910nd.c")
assert_eq "$CALLS" "3" "exactly three poll() call sites drive the I/O"

# The two copy_stream() loops wait on two descriptors each; flush_buffer()
# waits on one.  Record the timeouts so a rewrite that silently turns them into
# a busy loop is visible in the observation file.
obs "poll() timeouts (milliseconds):"
grep -n 'poll(pfd, 2, \|poll(&pfd, 1, ' "$REPO_ROOT/p910nd.c" |
	sed 's/^/    /' | while read -r l; do obs "$l"; done
assert_contains "$REPO_ROOT/p910nd.c" 'poll(pfd, 2, 100)' \
	"the bidirectional loop keeps its 100 ms pacing tick"
assert_contains "$REPO_ROOT/p910nd.c" 'poll(pfd, 2, 1000)' \
	"the unidirectional loop keeps its 1 s idle tick"
assert_contains "$REPO_ROOT/p910nd.c" 'poll(&pfd, 1, 100)' \
	"the flush retry keeps its 100 ms tick"
assert_contains "$REPO_ROOT/p910nd.c" 'poll(listenpfd' \
	"the accept loop waits on every listening address with poll"

# Link-time evidence: the symbol table must import poll and nothing of the old
# family.  static_checks.py reviews the same binary; recording it here keeps the
# observation file self-contained.
BIN="$BUILD_DIR/p910nd-baseline"
assert_file_exists "$BIN" "the baseline variant used for the symbol check"
if [ -x "$BIN" ] && command -v nm >/dev/null 2>&1; then
	nm -u "$BIN" >"$CASE_DIR/nm-undef.out" 2>&1
	obs_kv "undefined poll symbol count" \
		"$(count_lines "$CASE_DIR/nm-undef.out" 'poll')"
	assert_ge "$(count_lines "$CASE_DIR/nm-undef.out" 'poll')" "1" \
		"the binary imports poll()"
else
	obs "note: binary or nm unavailable, the symbol verdict comes from"
	obs "      static_checks.py only"
fi

say "the loop waits with poll(), which has no descriptor-number ceiling"
kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
