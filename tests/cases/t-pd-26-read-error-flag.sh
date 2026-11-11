#!/bin/sh
#
# PD-26  [medium]  A vanished client must not erase the printer read error.
#
# The bidirectional loop used to answer a failed network write with
# printerToNetworkBuffer.err = 0, so the reply direction could keep running
# as a sink (C3).  That also cleared READ_ERR: if the printer had failed on
# the same round, the PD-05 read-error exit no longer fired and the job kept
# polling a dead descriptor until the whole PRINTER_REPLY_WINDOW elapsed.
#
# discard_printer_reply() now clears only WRITE_ERR.  The probe includes
# p910nd.c, sets both flags and checks exactly which one survives.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-26 read-error-flag

PROBE="$CASE_DIR/discard_probe"
"$CC_BIN" -std=gnu89 -O1 -o "$PROBE" \
	"$TOOLS_DIR/discard_probe.c" -I"$REPO_ROOT" \
	>"$CASE_DIR/probe.build.log" 2>&1
build_rc=$?
obs "probe build:"
sed 's/^/    /' "$CASE_DIR/probe.build.log" | while read -r l; do obs "$l"; done
assert_eq "$build_rc" "0" "the reply-discard probe builds"
assert_file_exists "$PROBE" "the probe binary exists"

"$PROBE" >"$CASE_DIR/probe.out" 2>&1
probe_rc=$?
obs "probe output: $(tr '\n' '|' <"$CASE_DIR/probe.out")"
obs_kv "probe exit status" "$probe_rc"

assert_eq "$probe_rc" "0" \
	"only WRITE_ERR is cleared when the reply becomes a sink"
assert_contains "$CASE_DIR/probe.out" "read_err_kept=1" \
	"the printer read error survives for the PD-05 exit"
assert_contains "$CASE_DIR/probe.out" "write_err_cleared=1" \
	"and the write error is cleared so the print direction continues"
assert_not_contains "$REPO_ROOT/p910nd.c" 'printerToNetworkBuffer.err = 0;' \
	"the copy loop no longer clears the whole error word"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
