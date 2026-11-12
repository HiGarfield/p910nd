#!/bin/sh
#
# PD-88  [medium]  client.py must classify a bad --send as a tooling error.
#
# The payload was built with
#   args.send.encode("utf-8").decode("unicode_escape").encode("latin-1")
# and --send-byte with int(text, 0), with no exception handling.  A legal
# python escape such as \u4e2d decodes to a code point above 255, and a
# non-numeric --send-byte raises ValueError.  Both escaped main() before any
# socket work: stdout had no RESULT line, stderr had a traceback, and exit 1
# was indistinguishable from PD-48's "the print job failed".  PD-83 fixed the
# same failure shape for --out; the argument parsing was left behind.
#
# A: an out-of-range escape is reported with a RESULT line and status 2.
# B: a non-numeric --send-byte behaves the same way.
# C: control -- a valid escape is not misclassified.
# D: static -- both constructions are guarded.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-88 client-bad-send

CLIENT="$TOOLS_DIR/client.py"
PORT=$("$PYTHON" -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()')
obs_kv "closed port used" "$PORT"

run_client() {
	_label=$1
	shift
	"$PYTHON" "$CLIENT" --port "$PORT" --expect-reply 0 "$@" \
		>"$CASE_DIR/$_label.out" 2>"$CASE_DIR/$_label.err"
	CLIENT_RC=$?
	obs "$_label exit: $CLIENT_RC"
	obs "$_label stdout: $(tr '\n' '|' <"$CASE_DIR/$_label.out")"
	obs "$_label stderr: $(tr '\n' '|' <"$CASE_DIR/$_label.err")"
}

say "A: a \\u escape outside latin-1 is a tooling error"
run_client escape --send '\u4e2d'
assert_eq "$CLIENT_RC" "2" \
	"A the tooling error gets its own status"
assert_contains "$CASE_DIR/escape.out" "RESULT how=connect-error:bad-send" \
	"A a machine-readable RESULT is still printed"
assert_contains "$CASE_DIR/escape.out" "bad send argument" \
	"A the cause is named"
assert_not_contains "$CASE_DIR/escape.err" "Traceback" \
	"A no uncaught exception escapes"

say "B: a non-numeric --send-byte behaves the same way"
run_client byte --send-repeat 4 --send-byte notanumber
assert_eq "$CLIENT_RC" "2" \
	"B the tooling error gets its own status"
assert_contains "$CASE_DIR/byte.out" "RESULT how=connect-error:bad-send" \
	"B a machine-readable RESULT is still printed"
assert_not_contains "$CASE_DIR/byte.err" "Traceback" \
	"B no uncaught exception escapes"

say "C: control -- a valid escape is not misclassified"
run_client valid --send '\x41\x42'
assert_eq "$CLIENT_RC" "2" \
	"C a connection failure is still reported"
assert_contains "$CASE_DIR/valid.out" "RESULT how=connect-error" \
	"C with the usual connect-error RESULT"
assert_not_contains "$CASE_DIR/valid.out" "bad-send" \
	"C and no false argument error"
assert_not_contains "$CASE_DIR/valid.err" "Traceback" \
	"C no traceback on the good path"

say "D: static -- payload construction is guarded"
"$PYTHON" - "$CLIENT" >"$CASE_DIR/d.out" 2>&1 <<'EOF'
import sys
s = open(sys.argv[1], encoding="utf-8").read()
start = s.find('    payload = b""')
end = s.find('    # monotonic:', start)
body = s[start:end] if start >= 0 and end > start else ""
guarded = "except (UnicodeEncodeError, UnicodeDecodeError, ValueError)" in body
result = "RESULT how=connect-error:bad-send" in body
both = "decode(\"unicode_escape\")" in body and "parse_byte(args.send_byte)" in body
print("guarded=%s result=%s both_inputs=%s" % (guarded, result, both))
sys.exit(0 if guarded and result and both else 1)
EOF
assert_eq "$?" "0" \
	"D both argument paths are inside the handler"
assert_contains "$CASE_DIR/d.out" "guarded=True result=True both_inputs=True" \
	"D every property was actually checked"
assert_contains "$CLIENT" "PD-88: --send accepts python escapes" \
	"D the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
