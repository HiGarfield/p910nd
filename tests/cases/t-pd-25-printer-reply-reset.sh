#!/bin/sh
#
# PD-25  [medium]  A reset printer reply must end that direction.
#
# readBuffer() only marked EOF for ECONNRESET/ENOTCONN when detectEof was
# set, i.e. only on the network->printer direction.  On the
# printer->network direction (detectEof=0 for character devices) the error
# was swallowed: no EOF, no READ_ERR, and the bidirectional loop kept
# polling a descriptor that could never produce data again until the whole
# PRINTER_REPLY_WINDOW (60s by default) had elapsed.
#
# tests/tools/read_error_probe.c builds a loopback TCP pair, resets the peer
# with SO_LINGER, and feeds that descriptor to readBuffer() as the return
# direction.  It exits 0 only when the buffer is marked finished.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-25 printer-reply-reset

PROBE="$CASE_DIR/read_error_probe"
"$CC_BIN" -std=gnu89 -O1 -o "$PROBE" \
	"$TOOLS_DIR/read_error_probe.c" -I"$REPO_ROOT" \
	>"$CASE_DIR/probe.build.log" 2>&1
build_rc=$?
obs "probe build:"
sed 's/^/    /' "$CASE_DIR/probe.build.log" | while read -r l; do obs "$l"; done
assert_eq "$build_rc" "0" "the printer-reply reset probe builds"
assert_file_exists "$PROBE" "the probe binary exists"

"$PROBE" >"$CASE_DIR/probe.out" 2>&1
probe_rc=$?
obs "probe output: $(tr '\n' '|' <"$CASE_DIR/probe.out")"
obs_kv "probe exit status" "$probe_rc"

assert_eq "$probe_rc" "0" \
	"a reset on the printer reply marks that direction finished"
assert_contains "$CASE_DIR/probe.out" "eof_read=1" \
	"and it is reported as EOF, not as a corrupt print job"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
