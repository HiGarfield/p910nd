#!/bin/sh
#
# PD-38  [medium]  An untracked job child must still count against the cap.
#
# record_child() returns 0 when calloc()/realloc() fails, but the child has
# already been forked and keeps running.  It used to increment
# inflight_children only on success, and sigchld_handler() decremented only
# when it found the pid in the table.  Such children were therefore invisible
# to MAX_JOB_CHILDREN in both directions: under OOM the daemon could go on
# forking without any bound, and the shutdown wait could never observe them.
#
# The probe forces the OOM path and reaps the untracked child; a source scan
# checks that both allocation-failure branches increment the counter.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-38 child-cap-oom

PROBE="$CASE_DIR/child_cap_probe"
"$CC_BIN" -std=gnu89 -O1 -o "$PROBE" \
	"$TOOLS_DIR/child_cap_probe.c" -I"$REPO_ROOT" \
	>"$CASE_DIR/probe.build.log" 2>&1
build_rc=$?
obs "probe build:"
sed 's/^/    /' "$CASE_DIR/probe.build.log" | while read -r l; do obs "$l"; done
assert_eq "$build_rc" "0" "the OOM accounting probe builds"

"$PROBE" >"$CASE_DIR/probe.out" 2>&1
probe_rc=$?
obs "probe output: $(tr '\n' '|' <"$CASE_DIR/probe.out")"
obs_kv "probe exit status" "$probe_rc"
assert_eq "$probe_rc" "0" \
	"an untracked child raises the count and is decremented on reap"
assert_contains "$CASE_DIR/probe.out" "after_record=1" \
	"record_child() counts the child even when allocation fails"
assert_contains "$CASE_DIR/probe.out" "after_reap=0" \
	"sigchld_handler() decrements for a pid absent from the table"

"$PYTHON" - "$REPO_ROOT/p910nd.c" >"$CASE_DIR/scan.out" 2>&1 <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r"static int record_child\(.*?\n\}", s, re.S)
body = m.group(0) if m else ""
n = body.count("inflight_children++")
print("record_child increments: %d" % n)
# Two success paths (free slot, grown table) plus the two OOM branches.
sys.exit(0 if n == 4 else 1)
EOF
assert_eq "$?" "0" \
	"both success paths and both allocation-failure paths increment the counter"
assert_contains "$REPO_ROOT/p910nd.c" \
	"Every reaped child was counted at fork time" \
	"the handler explains why it decrements unconditionally"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
