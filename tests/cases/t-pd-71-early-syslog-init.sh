#!/bin/sh
#
# PD-71  [medium]  Early diagnostics must have a configured syslog identity.
#
# main() called openlog() only after getopt(), the printer-number checks and
# the argv[0] rewrite.  Under (x)inetd fd 2 is the client socket, so PD-66
# routes an invalid option and usage() through dolog() -> vsyslog(); -v does
# the same via show_version().  All of those paths call exit() before the
# openlog() call is reached: measured with the shim below, the old binary
# emitted two vsyslog lines and zero openlog lines, so the messages had the
# default ident, no LOG_PID and LOG_USER instead of LOG_LPR.
#
# A: an invalid option logs openlog first, with LOG_LPR and LOG_PID.
# B: -v does the same.
# C: static -- openlog() is called before getopt() and again for the port ident.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-71 early-syslog-init

CASE_BIN="$BUILD_DIR/test"
assert_file_exists "$CASE_BIN" "the shared test variant is available"

SHIM="$CASE_DIR/syslog_order.so"
"$CC_BIN" -shared -fPIC -O1 -std=gnu89 -o "$SHIM" \
	"$TOOLS_DIR/syslog_order_preload.c" -ldl \
	>"$CASE_DIR/shim.build.log" 2>&1
shim_rc=$?
obs "shim build:"
sed 's/^/    /' "$CASE_DIR/shim.build.log" | while read -r l; do obs "$l"; done
assert_eq "$shim_rc" "0" "the syslog order shim builds"

free_port() {
	"$PYTHON" -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()'
}

run_inetd() {
	_label=$1
	shift
	rm -f "$CASE_DIR/$_label.log"
	_port=$(free_port)
	P910ND_SYSLOG_LOG="$CASE_DIR/$_label.log" LD_PRELOAD="$SHIM" \
		timeout 15 "$PYTHON" "$TOOLS_DIR/inetd_sim.py" \
		--port "$_port" --target "$CASE_BIN" \
		--expect-reply 1 --read-timeout 2 -- "$@" \
		>"$CASE_DIR/$_label.out" 2>&1
	obs "$_label inetd exit: $?"
	obs "$_label output: $(tr '\n' '|' <"$CASE_DIR/$_label.out")"
	obs "$_label syslog trace: $(tr '\n' '|' <"$CASE_DIR/$_label.log" 2>/dev/null)"
}

check_trace() {
	_label=$1
	_log="$CASE_DIR/$_label.log"
	assert_file_exists "$_log" \
		"$_label the shim was loaded and syslog was used"
	assert_eq "$(sed -n 1p "$_log" 2>/dev/null)" "openlog 1 1" \
		"$_label the first syslog call is openlog(LOG_LPR|LOG_PID)"
	assert_contains "$_log" "^vsyslog" \
		"$_label and the diagnostic really reached syslog afterwards"
}

say "A: an invalid option under inetd"
run_inetd a -Z
check_trace a

say "B: -v under inetd"
run_inetd b -v
check_trace b

say "C: static -- openlog is initialised before argument parsing"
"$PYTHON" - "$REPO_ROOT/p910nd.c" >"$CASE_DIR/order.out" 2>&1 <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
main_start = s.index("int main(int argc, char *argv[])")
body = s[main_start:]
# Comments mention openlog() too; count only real, tab-indented call lines.
calls = [m.start() for m in re.finditer(r"^\topenlog\(", body, re.M)]
getopt_at = body.find("getopt(argc")
print("first_openlog=%d getopt=%d calls=%d" %
      (calls[0] if calls else -1, getopt_at, len(calls)))
ordered = (len(calls) == 2 and getopt_at >= 0 and
           calls[0] < getopt_at < calls[1])
sys.exit(0 if ordered else 1)
EOF
assert_eq "$?" "0" \
	"C openlog() precedes getopt() and the port-specific call is kept"
assert_contains "$CASE_DIR/order.out" "calls=2" \
	"C both the early and the renamed-ident call remain"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
