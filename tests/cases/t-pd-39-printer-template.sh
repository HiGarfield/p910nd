#!/bin/sh
#
# PD-39  [medium]  A malformed PRINTERFILE must not reach snprintf().
#
# open_printer() passed a single int to snprintf(PRINTERFILE, ...).  The
# shipped template has one %c, but a custom -DPRINTERFILE containing %s,
# two conversions or none at all made snprintf() read that int as a pointer
# or a missing argument - undefined behaviour at runtime.  The name buffer
# was also sized from the literal template, with no truncation check.
#
# The template is now validated at startup (unless -f overrides it), the
# buffer is fixed at 256 bytes and an over-long path is rejected.
#
# A: unit probe over legal and illegal templates.
# B: integration - a binary built with %s refuses to start with a clear
#    diagnostic instead of crashing or running with a broken device path.
# C: control - the default template still starts and listens.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-39 printer-template

PROBE="$CASE_DIR/template_probe"
"$CC_BIN" -std=gnu89 -O1 -o "$PROBE" \
	"$TOOLS_DIR/template_probe.c" -I"$REPO_ROOT" \
	>"$CASE_DIR/probe.build.log" 2>&1
build_rc=$?
obs "probe build:"
sed 's/^/    /' "$CASE_DIR/probe.build.log" | while read -r l; do obs "$l"; done
assert_eq "$build_rc" "0" "the template validator probe builds"

"$PROBE" >"$CASE_DIR/probe.out" 2>&1
probe_rc=$?
obs "probe output: $(tr '\n' '|' <"$CASE_DIR/probe.out")"
assert_eq "$probe_rc" "0" "only templates with exactly one %c are accepted"
assert_contains "$CASE_DIR/probe.out" "template_checks=1" \
	"legal and illegal templates are told apart"

assert_contains "$REPO_ROOT/p910nd.c" 'static char lpname\[256\]' \
	"the device path buffer no longer depends on the template length"
assert_contains "$REPO_ROOT/p910nd.c" "(size_t)n >= sizeof(lpname)" \
	"a truncated path is detected instead of being used"

# ---------------------------------------------------------------- part B ----
say "B: a binary built with an invalid template refuses to start"
LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR"
BAD=$(build_variant bad-template "-DLOCKFILE_DIR=\"$LOCKDIR\"" \
	'-DPRINTERFILE="/dev/lp%s"')
assert_file_exists "$BAD" "the invalid-template variant builds"
timeout 4 "$BAD" -d -i 127.0.0.1 0 >"$CASE_DIR/bad.out" 2>&1
bad_rc=$?
obs "B exit status: $bad_rc"
obs "B output: $(tr '\n' '|' <"$CASE_DIR/bad.out")"
assert_eq "$bad_rc" "1" \
	"B an invalid PRINTERFILE is refused before any device is opened"
assert_contains "$CASE_DIR/bad.out" "PRINTERFILE" \
	"B with a diagnostic that names the bad build-time setting"

# ---------------------------------------------------------------- part C ----
say "C: control -- the default template still starts"
DEV="$CASE_DIR/lp0"
: >"$DEV"
GOOD=$(build_variant test-goodtemplate "-DLOCKFILE_DIR=\"$LOCKDIR\"")
assert_file_exists "$GOOD" "the default-template variant builds"
daemon_start "$CASE_DIR/ok.daemon.log" "$GOOD" -d -i 127.0.0.1 -f "$DEV" 0
if [ -z "$DAEMON_PID" ]; then
	obs "C daemon failed to start: $(tr '\n' '|' <"$CASE_DIR/ok.daemon.log")"
	assert_true 1 "C the default template still starts"
else
	wait_for_port 127.0.0.1 "$TEST_PORT" 5
	assert_true 0 "C the default template still starts and listens"
fi
daemon_stop

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
