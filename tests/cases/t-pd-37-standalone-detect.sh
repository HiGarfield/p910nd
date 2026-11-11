#!/bin/sh
#
# PD-37  [high]  A failed getsockname() must not hide an inetd handover.
#
# is_standalone() returns "standalone" for every getsockname(0) failure
# except ENOTSOCK.  If the (x)inetd socket reports EINVAL/EBADF/ENOTCONN,
# the daemon starts a listener on 9100+n instead of reading the connection
# that was already handed to it, and that job is lost.
#
# The shim makes getsockname(0) fail while fd 0 remains a real TCP socket.
# A repaired daemon still serves the job; an unrepaired one leaves the
# client waiting.
#
# A: inetd-style connection under the fault injection is served in full.
# B: static -- the decision falls back to the descriptor type.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-37 standalone-detect

INETD_PORT=${INETD_PORT:-19199}
LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR"
CASE_BIN=$(build_variant test-standalone "-DLOCKFILE_DIR=\"$LOCKDIR\"")
assert_file_exists "$CASE_BIN" "the standalone-detect variant builds"

SHIM="$CASE_DIR/getsockname.so"
"$CC_BIN" -shared -fPIC -O1 -o "$SHIM" \
	"$TOOLS_DIR/getsockname_preload.c" >"$CASE_DIR/shim.build.log" 2>&1
shim_rc=$?
obs "shim build:"
sed 's/^/    /' "$CASE_DIR/shim.build.log" | while read -r l; do obs "$l"; done
assert_eq "$shim_rc" "0" "the getsockname fault-injection shim builds"

DEV="$CASE_DIR/lp0"
: >"$DEV"

env LD_PRELOAD="$SHIM" P910ND_BREAK_GETSOCKNAME=1 \
	"$PYTHON" "$TOOLS_DIR/inetd_sim.py" --port "$INETD_PORT" \
	--target "$CASE_BIN" --send-repeat 512 --half-close \
	--read-timeout 10 --device "$DEV" -- -i 127.0.0.1 -f "$DEV" 0 \
	>"$CASE_DIR/inetd.out" 2>&1
obs "inetd sim:"
sed 's/^/    /' "$CASE_DIR/inetd.out" | while read -r l; do obs "$l"; done

assert_contains "$CASE_DIR/inetd.out" "RESULT how=fin" \
	"the handed-over connection is served despite getsockname failing"
assert_contains "$CASE_DIR/inetd.out" "device_bytes=512" \
	"and the whole job reaches the printer"
assert_not_contains "$CASE_DIR/inetd.out" "how=timeout" \
	"the client is not left waiting on a listener that never reads it"

"$PYTHON" - "$REPO_ROOT/p910nd.c" >"$CASE_DIR/scan.out" 2>&1 <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r"static int is_standalone\(.*?\n\}", s, re.S)
body = m.group(0) if m else ""
print("S_ISSOCK guard: %s" % ("OK" if "S_ISSOCK" in body else "DEFECT"))
sys.exit(0 if "S_ISSOCK" in body else 1)
EOF
assert_eq "$?" "0" \
	"the standalone decision falls back to the descriptor type"
assert_contains "$CASE_DIR/scan.out" "S_ISSOCK guard: OK" \
	"the fallback is present in is_standalone()"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
