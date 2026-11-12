#!/bin/sh
#
# PD-96  [medium]  The listen backlog must be larger than the job cap.
#
# server() called listen(netfd, 30), but MAX_JOB_CHILDREN defaults to 64 and
# the accept loop deliberately leaves connections in the listen backlog once
# that cap is reached ("the client waits a little and is still served").
# With a queue of only 30, connections 31..64 never reach the accept queue:
# their SYNs are retransmitted or refused, so the documented concurrency
# level is impossible.  Measured on the unfixed build: strace shows
# listen(3, 30) = 0.
#
# The queue must be LARGER than the cap, not equal to it: PD-19 runs with
# MAX_JOB_CHILDREN=2 and requires six clients to wait in the backlog.  The
# fix therefore asks for SOMAXCONN (the kernel maximum) rather than a value
# derived from the cap.
#
# A: the default build asks for a backlog of at least MAX_JOB_CHILDREN.
# B: with MAX_JOB_CHILDREN=2 the backlog does NOT shrink (PD-19 semantics).
# C: static -- the old literal 30 is gone.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-96 listen-backlog-cap

SRC="$REPO_ROOT/p910nd.c"
LOCKDIR="$CASE_DIR/lock"
DEV="$CASE_DIR/lp0"
mkdir -p "$LOCKDIR" || exit 1
"$PYTHON" "$TOOLS_DIR/devnode.py" fifo "$DEV" >"$CASE_DIR/fifo.out" 2>&1
"$PYTHON" "$TOOLS_DIR/devnode.py" drain --fifo "$DEV" \
	--out "$CASE_DIR/stream.bin" >"$CASE_DIR/drain.out" 2>&1 &
sleep 0.5

# Start the daemon under strace and read the backlog the kernel was asked
# for.  strace execs the daemon in place, so DAEMON_PID/PGID from
# daemon_start still name the whole process group.
measure_backlog() {
	_tag=$1
	_bin=$2
	_want=$3
	rm -f "$CASE_DIR/trace-$_tag.log"
	daemon_start "$CASE_DIR/daemon-$_tag.log" \
		strace -f -e trace=listen -o "$CASE_DIR/trace-$_tag.log" \
		"$_bin" -d -f "$DEV" 0
	if [ -z "$DAEMON_PID" ]; then
		obs "$_tag daemon failed: $(tr '\n' '|' <"$CASE_DIR/daemon-$_tag.log")"
		return 1
	fi
	wait_for_port 127.0.0.1 "$TEST_PORT" 5 || return 1
	"$PYTHON" - "$CASE_DIR/trace-$_tag.log" "$_want" \
		>"$CASE_DIR/scan-$_tag.out" 2>&1 <<'EOF'
import re, sys
text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
want = int(sys.argv[2])
vals = [int(m.group(1)) for m in
        re.finditer(r"listen\(\s*\d+\s*,\s*(\d+)\s*\)", text)]
print("listen_calls=%d backlogs=%s min_want=%d" % (len(vals), vals, want))
# Every listener must leave room for at least the default cap, regardless
# of how small this build's MAX_JOB_CHILDREN is.
sys.exit(0 if vals and all(v >= want for v in vals) else 1)
EOF
	_scan_rc=$?
	obs "$_tag trace: $(tr '\n' '|' <"$CASE_DIR/trace-$_tag.log")"
	obs "$_tag scan: $(tr '\n' '|' <"$CASE_DIR/scan-$_tag.out")"
	daemon_stop
	kill_stray
	return "$_scan_rc"
}

say "A: the default build leaves room for the whole cap"
BIN=$(build_variant test-backlog "-DLOCKFILE_DIR=\"$LOCKDIR\"")
assert_file_exists "$BIN" "the default-cap variant builds"
measure_backlog default "$BIN" 64
assert_eq "$?" "0" \
	"A every listen() call requests a backlog of at least 64"
assert_eq "$(count_lines "$CASE_DIR/scan-default.out" 'listen_calls=')" "1" \
	"A the trace was actually parsed"

say "B: a small cap must not shrink the waiting queue"
BIN2=$(build_variant test-backlog2 \
	"-DLOCKFILE_DIR=\"$LOCKDIR\"" -DMAX_JOB_CHILDREN=2)
assert_file_exists "$BIN2" "the small-cap variant builds"
measure_backlog cap2 "$BIN2" 64
assert_eq "$?" "0" \
	"B with MAX_JOB_CHILDREN=2 the backlog still holds >= 64 entries"
assert_contains "$CASE_DIR/scan-cap2.out" "min_want=64" \
	"B the expected floor was checked"

say "C: static -- no hard-coded backlog remains"
assert_not_contains "$SRC" 'listen(netfd, 30)' \
	"C the literal 30 is gone"
assert_contains "$SRC" 'listen(netfd, SOMAXCONN)' \
	"C listen() asks for the kernel maximum"
assert_contains "$SRC" '#ifndef SOMAXCONN' \
	"C with a portable fallback for platforms without it"
assert_contains "$SRC" 'PD-96: the accept loop' \
	"C the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
