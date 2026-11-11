#!/bin/sh
#
# PD-30  [medium]  Throttled retries must not imitate the LOG_ERR warning.
#
# open_printer_retry() emits at most one LOG_ERR per
# OPEN_PRINTER_RETRY_LOG_EVERY window; attempts inside the window were
# logged at LOG_DEBUG with the *same* "will keep retrying" text.  dolog()
# prints LOG_DEBUG to stdout under -d, so a throttled attempt looked exactly
# like a real warning: the throttle could not be observed, the debug log was
# flooded, and PD-18's log-line assertion became timing dependent (it failed
# whenever two jobs each retried twice).
#
# The throttled line now says "retrying (log throttled)".  The probe drives
# the whole retry loop with a deterministic clock, captures the -d output and
# counts both wordings.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-30 retry-throttle

PROBE="$CASE_DIR/retry_log_probe"
"$CC_BIN" -std=gnu89 -O1 -o "$PROBE" \
	"$TOOLS_DIR/retry_log_probe.c" -I"$REPO_ROOT" \
	>"$CASE_DIR/probe.build.log" 2>&1
build_rc=$?
obs "probe build:"
sed 's/^/    /' "$CASE_DIR/probe.build.log" | while read -r l; do obs "$l"; done
assert_eq "$build_rc" "0" "the retry-throttle probe builds"

"$PROBE" throttle "$CASE_DIR/daemon.log" \
	>"$CASE_DIR/probe.out" 2>"$CASE_DIR/probe.err"
probe_rc=$?
obs "probe: $(tr '\n' '|' <"$CASE_DIR/probe.err")"
obs_kv "probe exit status" "$probe_rc"
warned=$(sed -n 's/.*warned_lines=\([0-9]*\).*/\1/p' "$CASE_DIR/probe.err")
throttled=$(sed -n 's/.*throttled_debug_lines=\([0-9]*\).*/\1/p' \
	"$CASE_DIR/probe.err")

assert_eq "$probe_rc" "0" "the retry loop keeps only one unthrottled warning"
assert_eq "${warned:-?}" "1" \
	"exactly one LOG_ERR line appears inside the 30 second retry budget"
assert_ge "${throttled:-0}" "5" \
	"and -d still shows the remaining attempts, under a distinct wording"
assert_eq "$(count_lines "$REPO_ROOT/p910nd.c" 'LOG_DEBUG.*will keep retrying')" \
	"0" "a throttled debug line no longer imitates the real warning"

# The same binary also carries the PD-23 first-failure check: make sure the
# new mode did not change it.
"$PROBE" >"$CASE_DIR/probe23.out" 2>&1
assert_eq "$?" "0" "the PD-23 first-failure probe still passes"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
