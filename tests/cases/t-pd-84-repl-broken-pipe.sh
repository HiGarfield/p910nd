#!/bin/sh
#
# PD-84  [medium]  devnode.py repl must not lie about a write that failed.
#
# do_repl() printed "event=write" BEFORE os.write(fd, payload) and had no
# exception handling or finally.  If the FIFO reader disappeared mid-run the
# second os.write() raised BrokenPipeError out of the helper: the log claimed
# a chunk had been delivered, no end event was printed, the FIFO descriptor
# leaked, and the case saw an unexplained traceback.  Measured with a reader
# that closes after 0.35 s and a 0.45 s repl delay: two event=write lines,
# exit 1, BrokenPipeError on stderr.
#
# A: a vanished reader is reported as a clean, complete failure.
# B: control -- a reader that stays gets three real writes and status 0.
# C: static -- the log follows the write, errors are caught, fd is released.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-84 repl-broken-pipe

DEV="$TOOLS_DIR/devnode.py"

run_repl() {
	_label=$1
	_lifetime=$2
	_fifo="$CASE_DIR/$_label.fifo"
	"$PYTHON" - "$_fifo" <<'EOF'
import os, sys
try:
    os.unlink(sys.argv[1])
except FileNotFoundError:
    pass
os.mkfifo(sys.argv[1], 0o600)
EOF
	# O_RDWR so the reader never sees EOF and does not block on open.
	"$PYTHON" - "$_fifo" "$_lifetime" >"$CASE_DIR/$_label.reader" 2>&1 <<'EOF' &
import os, sys, time
fd = os.open(sys.argv[1], os.O_RDWR)
time.sleep(float(sys.argv[2]))
os.close(fd)
EOF
	READER_PID=$!
	sleep 0.1
	"$PYTHON" "$DEV" repl --fifo "$_fifo" --data A \
		--delay 0.45 --repeat 3 \
		>"$CASE_DIR/$_label.out" 2>"$CASE_DIR/$_label.err"
	REPL_RC=$?
	wait "$READER_PID" 2>/dev/null
	obs "$_label exit: $REPL_RC"
	obs "$_label stdout: $(tr '\n' '|' <"$CASE_DIR/$_label.out")"
	obs "$_label stderr: $(tr '\n' '|' <"$CASE_DIR/$_label.err")"
}

say "A: the reader disappears before the second write"
run_repl broken 0.35
assert_eq "$REPL_RC" "1" \
	"A a lost reader is a distinct, reported failure"
assert_not_contains "$CASE_DIR/broken.err" "Traceback" \
	"A no uncaught exception escapes"
assert_contains "$CASE_DIR/broken.out" "event=write-error" \
	"A the broken pipe is named"
assert_contains "$CASE_DIR/broken.out" "event=repl-end written=1 requested=3" \
	"A the end event reports the one write that really landed"
assert_eq "$(count_lines "$CASE_DIR/broken.out" '^event=write t=')" "1" \
	"A the log no longer claims a write that failed"

say "B: control -- a persistent reader gets all three writes"
run_repl healthy 3.0
assert_eq "$REPL_RC" "0" \
	"B a complete run still succeeds"
assert_eq "$(count_lines "$CASE_DIR/healthy.out" '^event=write t=')" "3" \
	"B three real writes are logged"
assert_contains "$CASE_DIR/healthy.out" "event=repl-end written=3 requested=3" \
	"B the end event reports a complete run"
assert_not_contains "$CASE_DIR/healthy.out" "event=write-error" \
	"B with no false error"
assert_not_contains "$CASE_DIR/healthy.err" "Traceback" \
	"B and no traceback"

say "C: static -- log after write, guarded, descriptor released"
"$PYTHON" - "$DEV" >"$CASE_DIR/c.out" 2>&1 <<'EOF'
import sys
s = open(sys.argv[1], encoding="utf-8").read()
start = s.index("def do_repl(")
body = s[start:s.index("def main(", start)]
write_at = body.find("os.write(fd, payload)")
log_at = body.find('print("event=write t=')
guarded = "except OSError as exc" in body
fin = body.find("finally:")
closed = fin >= 0 and "os.close(fd)" in body[fin:]
end_event = "event=repl-end" in body
order = 0 <= write_at < log_at
print("log_after_write=%s guarded=%s finally_close=%s end_event=%s" %
      (order, guarded, closed, end_event))
sys.exit(0 if order and guarded and closed and end_event else 1)
EOF
assert_eq "$?" "0" \
	"C the write is logged only after it succeeds and cleanup is guaranteed"
assert_contains "$CASE_DIR/c.out" "log_after_write=True guarded=True finally_close=True end_event=True" \
	"C all four properties were actually checked"
assert_contains "$DEV" "PD-84: log a write only AFTER it succeeded" \
	"C the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
