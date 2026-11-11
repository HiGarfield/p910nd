#!/bin/sh
#
# PD-36  [medium]  Undelivered bytes must not vanish at the final flush.
#
# flush_buffer() returned only b->bytes.  In sink mode (outfd < 0, set when
# the client disappears) writeBuffer() empties the ring, so the function
# reported "0 undelivered" even though every reply byte had been discarded.
# All three call sites ignored the result anyway, so a lost printer reply
# was never logged.
#
# The function now counts discarded bytes, both copy_stream() branches use
# its result in the success verdict, and the bidirectional branch logs a
# discarded reply.  The probe checks both sink and normal flushes; a source
# scan checks that every call site consumes the result.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-36 flush-result

PROBE="$CASE_DIR/flush_probe"
"$CC_BIN" -std=gnu89 -O1 -o "$PROBE" \
	"$TOOLS_DIR/flush_probe.c" -I"$REPO_ROOT" \
	>"$CASE_DIR/probe.build.log" 2>&1
build_rc=$?
obs "probe build:"
sed 's/^/    /' "$CASE_DIR/probe.build.log" | while read -r l; do obs "$l"; done
assert_eq "$build_rc" "0" "the flush-result probe builds"

"$PROBE" >"$CASE_DIR/probe.out" 2>&1
probe_rc=$?
obs "probe output: $(tr '\n' '|' <"$CASE_DIR/probe.out")"
obs_kv "probe exit status" "$probe_rc"
assert_eq "$probe_rc" "0" \
	"a sink reports its discarded bytes and a real write reports success"
assert_contains "$CASE_DIR/probe.out" "sink_left=7" \
	"the 7 discarded reply bytes are visible to the caller"
assert_contains "$CASE_DIR/probe.out" "delivered_left=0" \
	"a normal flush is still reported as fully delivered"

"$PYTHON" - "$REPO_ROOT/p910nd.c" >"$CASE_DIR/scan.out" 2>&1 <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r"static int copy_stream\(.*?\n\}", s, re.S)
body = m.group(0) if m else ""
calls = len(re.findall(r"\bflush_buffer\(", body))
assigned = len(re.findall(r"=\s*flush_buffer\(", body))
print("calls=%d assigned=%d" % (calls, assigned))
sys.exit(0 if calls == 3 and assigned == 3 else 1)
EOF
scan_rc=$?
obs "source scan: $(tr '\n' '|' <"$CASE_DIR/scan.out")"
assert_eq "$scan_rc" "0" \
	"all three copy_stream() flush calls consume the undelivered count"
assert_contains "$REPO_ROOT/p910nd.c" "not delivered to the client" \
	"a discarded printer reply is logged"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
