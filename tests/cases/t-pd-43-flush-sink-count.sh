#!/bin/sh
#
# PD-43  [medium]  A wrapped ring must not be counted twice when discarded.
#
# flush_buffer() reports how many bytes could not be delivered.  When the
# printer's reply is a sink (the client disappeared), it consumes the ring
# with writeBuffer(), which can only remove ONE contiguous run per call.
# A ring wrapped around the end of the buffer therefore needs two passes,
# but the old loop added b->bytes to `discarded` before every pass: the
# tail was counted twice and the "reply: N bytes not delivered" log lied
# about how much data was lost.
#
# The probe seeds a wrapped ring (4 bytes at the tail, 3 at the head) and
# a plain one, and requires both to report exactly the 7 bytes present.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-43 flush-sink-count

PROBE="$CASE_DIR/flush_sink_probe"
"$CC_BIN" -std=gnu89 -O1 -o "$PROBE" \
	"$TOOLS_DIR/flush_sink_probe.c" -I"$REPO_ROOT" \
	>"$CASE_DIR/probe.build.log" 2>&1
build_rc=$?
obs "probe build:"
sed 's/^/    /' "$CASE_DIR/probe.build.log" | while read -r l; do obs "$l"; done
assert_eq "$build_rc" "0" "the wrapped-ring probe builds"

"$PROBE" >"$CASE_DIR/probe.out" 2>&1
probe_rc=$?
obs "probe output: $(tr '\n' '|' <"$CASE_DIR/probe.out")"
obs_kv "probe exit status" "$probe_rc"
assert_eq "$probe_rc" "0" \
	"a wrapped sink ring reports exactly the bytes it held"
assert_contains "$CASE_DIR/probe.out" "have=7 wrapped_lost=7 plain_lost=7" \
	"both the wrapped and the plain ring report 7, not 10"

# The existing single-pass sink case must still hold after the change.
PD36_PROBE="$CASE_DIR/flush_probe"
"$CC_BIN" -std=gnu89 -O1 -o "$PD36_PROBE" \
	"$TOOLS_DIR/flush_probe.c" -I"$REPO_ROOT" \
	>"$CASE_DIR/pd36.build.log" 2>&1
assert_eq "$?" "0" "the existing flush-result probe still builds"
"$PD36_PROBE" >"$CASE_DIR/pd36.out" 2>&1
pd36_rc=$?
# PD-106: save the probe's status before logging.  obs() is a printf, so
# "$?" here used to be 0 no matter how the probe exited -- an assertion that
# could never fail.
obs "PD-36 probe output: $(tr '\n' '|' <"$CASE_DIR/pd36.out")"
assert_eq "$pd36_rc" "0" \
	"the normal flush still reports 0 delivered-left and 7 discarded"
assert_contains "$CASE_DIR/pd36.out" "sink_left=7 delivered_left=0" \
	"and the byte counts are unchanged for the non-wrapped path"

assert_contains "$REPO_ROOT/p910nd.c" "discarded += (int)n" \
	"the sink counts bytes removed per pass, not the whole ring each time"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
