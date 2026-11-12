#!/bin/sh
#
# PD-56  [high]  A refused standalone connection must not look like a
# successful print job.
#
# With USE_LIBWRAP, one_job() rejects a connection with
# close_connection(0, 1) (RST, so CUPS retries), but server() used a bare
# close(fd).  On a socket with no unread data a bare close() sends a clean
# FIN: the client believes the job printed and never retries, even though
# tcpwrappers denied it.  Measured behaviour before the fix: RESULT how=fin.
#
# A: deny -> the client must see RST and a non-zero exit.
# B: allow (control) -> the job reaches the printer with a clean FIN.
# C: static -> both refusal paths use close_connection(fd, 1).

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-56 libwrap-reject-rst

LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR"
# the name contains "test", so build_variant() patches BASEPORT to TEST_PORT
BIN=$(build_variant test-libwrap-rst "-DLOCKFILE_DIR=\"$LOCKDIR\"" \
	-DUSE_LIBWRAP -lwrap)
assert_file_exists "$BIN" "the USE_LIBWRAP variant builds"

SHIM="$CASE_DIR/hosts_ctl.so"
"$CC_BIN" -shared -fPIC -O1 -o "$SHIM" \
	"$TOOLS_DIR/hosts_ctl_preload.c" >"$CASE_DIR/shim.build.log" 2>&1
shim_rc=$?
obs "shim build:"
sed 's/^/    /' "$CASE_DIR/shim.build.log" | while read -r l; do obs "$l"; done
assert_eq "$shim_rc" "0" "the hosts_ctl shim builds"

DEV="$CASE_DIR/lp0"
: >"$DEV"
LOG="$CASE_DIR/hosts.log"
: >"$LOG"
DAEMON_PROC=

start_daemon() {
	_label=$1
	_deny=$2
	LD_PRELOAD="$SHIM" P910ND_HOSTS_CTL_LOG="$LOG" \
		P910ND_HOSTS_CTL_DENY="$_deny" \
		"$BIN" -d -i 127.0.0.1 -f "$DEV" 0 \
		>"$CASE_DIR/$_label.daemon.log" 2>&1 </dev/null &
	DAEMON_PROC=$!
	wait_for_port 127.0.0.1 "$TEST_PORT" 5
}

stop_daemon() {
	[ -n "${DAEMON_PROC:-}" ] || return 0
	kill -TERM "$DAEMON_PROC" 2>/dev/null
	wait "$DAEMON_PROC" 2>/dev/null
	DAEMON_PROC=
}

say "A: hosts_ctl denies the connection"
start_daemon deny 1
"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" \
	--expect-reply 1 --read-timeout 5 --label DENY \
	>"$CASE_DIR/deny.client" 2>&1
deny_rc=$?
obs "A client: $(tr '\n' '|' <"$CASE_DIR/deny.client")"
obs_kv "A client exit" "$deny_rc"
deny_how=$(sed -n 's/.*RESULT how=\([a-z-]*\).*/\1/p' "$CASE_DIR/deny.client")
obs_kv "A how" "$deny_how"
assert_eq "$deny_how" "rst" \
	"A a refused standalone connection is reported with RST"
assert_eq "$deny_rc" "1" \
	"A and the client exits non-zero instead of assuming success"
assert_eq "$(wc -c <"$DEV" | tr -d ' ')" "0" \
	"A no data reached the printer"
stop_daemon

say "B: control -- hosts_ctl allows the connection"
start_daemon allow 0
"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" \
	--send-repeat 16 --half-close --expect-reply 1 --read-timeout 5 \
	--label ALLOW >"$CASE_DIR/allow.client" 2>&1
allow_rc=$?
obs "B client: $(tr '\n' '|' <"$CASE_DIR/allow.client")"
obs_kv "B client exit" "$allow_rc"
allow_how=$(sed -n 's/.*RESULT how=\([a-z-]*\).*/\1/p' "$CASE_DIR/allow.client")
assert_eq "$allow_how" "fin" \
	"B an allowed job still ends with a clean FIN"
assert_eq "$allow_rc" "0" "B and exits successfully"
assert_ge "$(wc -c <"$DEV" | tr -d ' ')" "16" \
	"B the allowed job reaches the printer in full"
stop_daemon

say "C: static -- both refusal paths use the failed-job close"
"$PYTHON" - "$REPO_ROOT/p910nd.c" >"$CASE_DIR/scan.out" 2>&1 <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
needle = '"Connection from %s port %hu rejected\\n"'
blocks = [s[m.end():m.end() + 500] for m in re.finditer(re.escape(needle), s)]
print("refusal_blocks=%d" % len(blocks))
for i, b in enumerate(blocks):
    tail = b.split("continue;")[0]
    print("block%d rst=%s bare_close=%s" %
          (i, "close_connection(0, 1)" in tail or "close_connection(fd, 1)" in tail,
           re.search(r"\bclose\(fd\);", tail) is not None))
ok = len(blocks) == 2 and all(
    ("close_connection(0, 1)" in b.split("continue;")[0] or
     "close_connection(fd, 1)" in b.split("continue;")[0]) and
    not re.search(r"\bclose\(fd\);", b.split("continue;")[0])
    for b in blocks)
sys.exit(0 if ok else 1)
EOF
scan_rc=$?
obs "C scan: $(tr '\n' '|' <"$CASE_DIR/scan.out")"
obs_kv "C scan exit" "$scan_rc"
assert_eq "$scan_rc" "0" \
	"C server() and one_job() both reject with close_connection(..., 1)"
assert_contains "$REPO_ROOT/p910nd.c" "PD-56: a bare close()" \
	"C the standalone refusal is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
