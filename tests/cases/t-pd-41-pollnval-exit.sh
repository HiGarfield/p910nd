#!/bin/sh
#
# PD-41  [high]  Dead listening sockets must not spin the daemon forever.
#
# The accept loop waits on every listening descriptor with poll().  When all
# of them come back with only POLLNVAL (closed descriptors), no listener is
# selected, so the old code slept 20 ms and went back to poll() forever: the
# process stayed alive, held the instance lock and the port, and could never
# serve another client.
#
# A: with a shim that returns POLLNVAL for every descriptor, the daemon must
#    stop by itself after a bounded retry and say why.
# B: control -- without the shim the same binary stays up and listens.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-41 pollnval-exit

LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR"
CASE_BIN=$(build_variant test-pollnval "-DLOCKFILE_DIR=\"$LOCKDIR\"")
assert_file_exists "$CASE_BIN" "the pollnval variant builds"

SHIM="$CASE_DIR/pollnval.so"
"$CC_BIN" -shared -fPIC -O1 -o "$SHIM" \
	"$TOOLS_DIR/pollnval_preload.c" -ldl >"$CASE_DIR/shim.build.log" 2>&1
shim_rc=$?
obs "shim build:"
sed 's/^/    /' "$CASE_DIR/shim.build.log" | while read -r l; do obs "$l"; done
assert_eq "$shim_rc" "0" "the POLLNVAL shim builds"

DEV="$CASE_DIR/lp0"
: >"$DEV"

say "A: every listener reports POLLNVAL"
# </dev/null matters: is_standalone() inspects fd 0, and a harness whose
# stdin happens to be a socket would take the inetd path instead of server().
timeout 10 env LD_PRELOAD="$SHIM" P910ND_POLLNVAL=1 \
	"$CASE_BIN" -d -i 127.0.0.1 -f "$DEV" 0 \
	>"$CASE_DIR/a.out" 2>&1 </dev/null
a_rc=$?
obs "A exit status: $a_rc"
obs "A output: $(tr '\n' '|' <"$CASE_DIR/a.out")"
assert_eq "$a_rc" "1" \
	"A the daemon exits on its own instead of being killed by timeout"
assert_contains "$CASE_DIR/a.out" "all listening sockets are invalid" \
	"A and logs the reason instead of spinning silently"

say "B: control -- no shim, the daemon keeps listening"
daemon_start "$CASE_DIR/b.daemon.log" "$CASE_BIN" -d -i 127.0.0.1 \
	-f "$DEV" 0
if [ -z "$DAEMON_PID" ]; then
	obs "B daemon failed to start: $(tr '\n' '|' <"$CASE_DIR/b.daemon.log")"
	assert_true 1 "B without POLLNVAL the daemon stays up"
else
	wait_for_port 127.0.0.1 "$TEST_PORT" 5
	assert_true 0 "B without POLLNVAL the daemon stays up and listens"
fi
daemon_stop

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
