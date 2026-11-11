#!/bin/sh
#
# PD-27  [high]  A transient accept() error must not stop the daemon.
#
# The accept loop handled EINTR, connection aborts and resource shortages,
# but every other errno fell through to a bare "break": the daemon left its
# loop and exited, so one ENOSR/ENETDOWN ended all printing on that host.
#
# The shim injects a single ENOSR on the first accept().  A repaired daemon
# keeps listening and serves the next real client; an unrepaired one is gone
# by then, so that client cannot even connect.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-27 accept-transient

LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR"
CASE_BIN=$(build_variant test-accept "-DLOCKFILE_DIR=\"$LOCKDIR\"")
assert_file_exists "$CASE_BIN" "the accept variant builds"

SHIM="$CASE_DIR/accept_errno.so"
"$CC_BIN" -shared -fPIC -O1 -o "$SHIM" \
	"$TOOLS_DIR/accept_errno_preload.c" -ldl \
	>"$CASE_DIR/shim.build.log" 2>&1
shim_rc=$?
obs "shim build:"
sed 's/^/    /' "$CASE_DIR/shim.build.log" | while read -r l; do obs "$l"; done
assert_eq "$shim_rc" "0" "the accept() fault-injection shim builds"

DEV="$CASE_DIR/lp0"
: >"$DEV"

# LD_PRELOAD is set only for the daemon process: the shell and the python
# helpers keep their normal accept(), so only the daemon sees the fault.
daemon_start "$CASE_DIR/daemon.log" env LD_PRELOAD="$SHIM" \
	P910ND_ACCEPT_FAILURES=1 "$CASE_BIN" -d -i 127.0.0.1 -f "$DEV" 0
if [ -z "$DAEMON_PID" ]; then
	obs "daemon failed to start: $(tr '\n' '|' <"$CASE_DIR/daemon.log")"
	assert_true 1 "the daemon starts"
	kill_stray
	exit 1
fi
wait_for_port 127.0.0.1 "$TEST_PORT" 5 || obs "the port never came up"

"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" --send-repeat 1024 \
	--half-close --expect-reply 1 --read-timeout 12 --label A \
	>"$CASE_DIR/client.out" 2>&1
c_how=$(sed -n 's/.*RESULT how=\([a-z-]*\).*/\1/p' "$CASE_DIR/client.out")
dev_bytes=$(wc -c <"$DEV" | tr -d ' ')
obs "client: $(tr '\n' '|' <"$CASE_DIR/client.out")"
obs_kv "client termination" "$c_how"
obs_kv "bytes the printer received" "$dev_bytes"

daemon_stop
obs "daemon log:"
sed 's/^/    /' "$CASE_DIR/daemon.log" | while read -r l; do obs "$l"; done

assert_eq "$c_how" "fin" \
	"the daemon still serves the next client after a transient accept error"
assert_eq "$dev_bytes" "1024" "and the whole job reaches the printer"
assert_contains "$CASE_DIR/daemon.log" "transient error, waiting" \
	"the transient error is logged and retried, not treated as fatal"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
