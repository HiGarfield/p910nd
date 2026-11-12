#!/bin/sh
#
# PD-83  [medium]  client.py must not lose its RESULT when --out is unwritable.
#
# The --out reply file was written as the FIRST statement of a finally block.
# An unwritable path raised FileNotFoundError out of that block, so:
#   * stdout was empty (no RESULT how= line at all),
#   * exit 1 was indistinguishable from PD-48's "the print job failed",
#   * sock.close() was skipped and the connection leaked.
# A case asking for a reply file could therefore misreport a tooling error
# as a job outcome, or go green on a file that was never produced.
#
# A: an unwritable --out still prints RESULT, names the error, returns 2,
#    and does not raise a traceback.
# B: control -- a writable --out still writes the file and returns 0.
# C: static -- the socket is closed before the file is written, and the
#    write is guarded by an OSError handler with a distinct exit status.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-83 client-out-error

CLIENT="$TOOLS_DIR/client.py"
PORT=$("$PYTHON" -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()')
obs_kv "test port" "$PORT"

cat >"$CASE_DIR/server.py" <<'EOF'
import socket, sys, time
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1])))
s.listen(8)
s.settimeout(0.2)
end = time.monotonic() + 20.0
while time.monotonic() < end:
    try:
        c, _peer = s.accept()
    except socket.timeout:
        continue
    c.close()
EOF
"$PYTHON" "$CASE_DIR/server.py" "$PORT" >"$CASE_DIR/server.log" 2>&1 &
SERVER_PID=$!
wait_for_port 127.0.0.1 "$PORT" 5
assert_eq "$?" "0" "the control server is listening"

say "A: an unwritable reply path is reported, not raised"
"$PYTHON" "$CLIENT" --port "$PORT" --expect-reply 0 \
	--out "$CASE_DIR/no-such-dir/reply.bin" \
	>"$CASE_DIR/a.out" 2>"$CASE_DIR/a.err"
A_RC=$?
obs "A exit: $A_RC"
obs "A stdout: $(tr '\n' '|' <"$CASE_DIR/a.out")"
obs "A stderr: $(tr '\n' '|' <"$CASE_DIR/a.err")"
assert_eq "$A_RC" "2" \
	"A a tooling error gets its own exit status, not the job-failure status"
assert_contains "$CASE_DIR/a.out" "RESULT how=" \
	"A the machine-readable result is still printed"
assert_contains "$CASE_DIR/a.out" "cannot write --out" \
	"A and the real cause is named"
assert_not_contains "$CASE_DIR/a.err" "Traceback" \
	"A no uncaught exception escapes"

say "B: control -- a writable reply path behaves as before"
"$PYTHON" "$CLIENT" --port "$PORT" --expect-reply 0 \
	--out "$CASE_DIR/reply.bin" \
	>"$CASE_DIR/b.out" 2>"$CASE_DIR/b.err"
B_RC=$?
obs "B exit: $B_RC"
obs "B stdout: $(tr '\n' '|' <"$CASE_DIR/b.out")"
assert_eq "$B_RC" "0" \
	"B a no-read job with a good path still succeeds"
assert_file_exists "$CASE_DIR/reply.bin" \
	"B and the reply file is created"
assert_contains "$CASE_DIR/b.out" "RESULT how=" \
	"B with the usual RESULT line"
assert_not_contains "$CASE_DIR/b.out" "cannot write --out" \
	"B and no false error"
assert_not_contains "$CASE_DIR/b.err" "Traceback" \
	"B no traceback on the good path"

say "C: static -- close first, guarded write, distinct status"
"$PYTHON" - "$CLIENT" >"$CASE_DIR/c.out" 2>&1 <<'EOF'
import sys
s = open(sys.argv[1], encoding="utf-8").read()
close_at = s.find("sock.close()")
out_at = s.find('open(args.out, "wb")')
tail = s[out_at:out_at + 500] if out_at >= 0 else ""
guarded = "except OSError as exc" in tail
distinct = "if out_error is not None:" in s and "return 2" in s
order = 0 <= close_at < out_at
print("close_before_out=%s guarded=%s distinct_status=%s" %
      (order, guarded, distinct))
sys.exit(0 if order and guarded and distinct else 1)
EOF
assert_eq "$?" "0" \
	"C the socket is released first and the write is safely handled"
assert_contains "$CASE_DIR/c.out" "close_before_out=True guarded=True distinct_status=True" \
	"C all three properties were actually checked"
assert_contains "$CLIENT" "PD-83: close the socket FIRST" \
	"C the reason is documented"

kill -TERM "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null
kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
