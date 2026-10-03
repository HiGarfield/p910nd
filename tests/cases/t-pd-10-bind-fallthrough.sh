#!/bin/sh
#
# PD-10  [中]  A failed bind must be reported as such.
#
# The getaddrinfo loop closed netfd on every failure but never recorded that
# nothing was bound, so when all candidate addresses failed the daemon fell into
# the accept loop with a stale descriptor and logged "accept: Bad file
# descriptor" -- an error with nothing to do with the real cause, which was the
# bind failure one line earlier.  It now says "no address worked" and leaves.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-10 bind-fallthrough
CASE_BIN="$BUILD_DIR/test"

say "occupy 127.0.0.1:$TEST_PORT"
"$PYTHON" - "$TEST_PORT" >"$CASE_DIR/occupier.out" 2>&1 <<'EOF' &
import socket, sys, time
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1])))
s.listen(1)
print("occupying", flush=True)
time.sleep(60)
EOF
OCC=$!
for _i in 1 2 3 4 5 6 7 8 9 10; do
	grep -q occupying "$CASE_DIR/occupier.out" 2>/dev/null && break
	sleep 0.2
done
obs "occupier: $(tr '\n' '|' <"$CASE_DIR/occupier.out")"
assert_contains "$CASE_DIR/occupier.out" "occupying" "fixture: the port is taken"

say "start a second daemon on the same address"
# stdin from /dev/null so is_standalone() takes the daemon path
strace -o "$CASE_DIR/a.trace" -e trace=socket,bind,listen,accept,close \
	"$CASE_BIN" -d -i 127.0.0.1 0 >"$CASE_DIR/a.daemon.log" 2>&1 </dev/null
status=$?
kill "$OCC" 2>/dev/null
wait "$OCC" 2>/dev/null

obs_kv "exit status" "$status"
obs "daemon log:"
sed 's/^/    /' "$CASE_DIR/a.daemon.log" | while read -r l; do obs "$l"; done
obs "relevant syscalls:"
grep -E 'socket\(AF_INET|bind\(|accept\(' "$CASE_DIR/a.trace" | sed 's/^/    /' |
	while read -r l; do obs "$l"; done

assert_contains "$CASE_DIR/a.daemon.log" "bind: Address already in use" \
	"the bind failure is still reported"
assert_contains "$CASE_DIR/a.daemon.log" "no address worked" \
	"and the daemon now says that nothing was bound"
assert_not_contains "$CASE_DIR/a.daemon.log" "accept: Bad file descriptor" \
	"it no longer enters the accept loop on a stale descriptor"
assert_not_contains "$CASE_DIR/a.trace" "accept(" \
	"accept(2) is not called at all any more"
assert_eq "$status" "1" "the process still exits non-zero"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
