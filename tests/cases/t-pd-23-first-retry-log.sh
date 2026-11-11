#!/bin/sh
#
# PD-23  [medium]  The first printer-open failure must reach syslog.
#
# open_printer_retry() used a zero timestamp to mean "nothing logged yet".
# That held for gettimeofday() (epoch seconds are large), but mono_now()
# moved every deadline to CLOCK_MONOTONIC: on a machine booted less than
# OPEN_PRINTER_RETRY_LOG_EVERY (60s) ago the first failure was demoted to
# LOG_DEBUG, which dolog() drops, so the reason a job was waiting never
# appeared in syslog at all.
#
# tests/tools/retry_log_probe.c includes p910nd.c and overrides
# clock_gettime(), sleep() and vsyslog(): the clock starts at second 5, the
# retry loop ends after one pass, and the first visible log line is recorded.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-23 first-retry-log

PROBE="$CASE_DIR/retry_log_probe"
"$CC_BIN" -std=gnu89 -O1 -o "$PROBE" \
	"$TOOLS_DIR/retry_log_probe.c" -I"$REPO_ROOT" \
	>"$CASE_DIR/probe.build.log" 2>&1
build_rc=$?
obs "probe build:"
sed 's/^/    /' "$CASE_DIR/probe.build.log" | while read -r l; do obs "$l"; done
assert_eq "$build_rc" "0" "the first-failure log probe builds"
assert_file_exists "$PROBE" "the probe binary exists"

"$PROBE" >"$CASE_DIR/probe.out" 2>&1
probe_rc=$?
obs "probe output: $(tr '\n' '|' <"$CASE_DIR/probe.out")"
obs_kv "probe exit status" "$probe_rc"

assert_eq "$probe_rc" "0" \
	"the first open failure is reported at a priority syslog keeps"
assert_contains "$CASE_DIR/probe.out" "will keep retrying" \
	"the first line names the retry, not only the eventual give-up"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
