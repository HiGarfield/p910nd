#!/bin/sh
#
# PD-35  [high]  The daemon must listen on every address getaddrinfo gives.
#
# server() stopped at the first successful bind/listen.  With PF_UNSPEC and
# a wildcard bind, getaddrinfo() commonly returns AF_INET first, so only the
# IPv4 socket was ever listened on and every IPv6 client was refused, even
# though the code went out of its way to set up an IPv6 socket.
#
# The daemon is started without -i (wildcard) and one job is driven over each
# family.  Before the fix the IPv6 connection is refused; after it, both jobs
# reach the printer and both clients see a clean FIN.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-35 multi-address

LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR"
CASE_BIN=$(build_variant test-multiaddr "-DLOCKFILE_DIR=\"$LOCKDIR\"")
assert_file_exists "$CASE_BIN" "the multi-address variant builds"

DEV="$CASE_DIR/lp0"
# A FIFO with a permanent reader accumulates both jobs.  A regular file
# would be reopened at offset 0 by the second job and appear to contain
# only one of them.
"$PYTHON" "$TOOLS_DIR/devnode.py" fifo "$DEV" >"$CASE_DIR/fifo.out" 2>&1
obs "fifo: $(tr '\n' '|' <"$CASE_DIR/fifo.out")"
"$PYTHON" "$TOOLS_DIR/devnode.py" drain --fifo "$DEV" \
	--out "$CASE_DIR/stream.bin" >"$CASE_DIR/drain.out" 2>&1 &
DRAIN=$!
sleep 0.5

# No -i: the wildcard bind must cover both families.
daemon_start "$CASE_DIR/daemon.log" "$CASE_BIN" -d -f "$DEV" 0
if [ -z "$DAEMON_PID" ]; then
	obs "daemon failed to start: $(tr '\n' '|' <"$CASE_DIR/daemon.log")"
	assert_true 1 "the daemon starts"
	kill_stray
	exit 1
fi

"$PYTHON" - "$TEST_PORT" >"$CASE_DIR/client.out" 2>&1 <<'EOF'
import socket, sys, time
port = int(sys.argv[1])

deadline = time.monotonic() + 5.0
while time.monotonic() < deadline:
    try:
        probe = socket.create_connection(("127.0.0.1", port), 0.25)
        probe.close()
        break
    except OSError:
        time.sleep(0.05)


def job(family, host, label):
    how = "none"
    try:
        s = socket.socket(family, socket.SOCK_STREAM)
        s.settimeout(10.0)
        s.connect((host, port))
        s.sendall(b"X" * 512)
        s.shutdown(socket.SHUT_WR)
        while True:
            if not s.recv(4096):
                how = "fin"
                break
        s.close()
    except OSError as exc:
        how = "error:%s" % exc.__class__.__name__
    print("RESULT %s=%s" % (label, how))
    return how == "fin"


ok4 = job(socket.AF_INET, "127.0.0.1", "ipv4")
ok6 = job(socket.AF_INET6, "::1", "ipv6")
sys.exit(0 if ok4 and ok6 else 1)
EOF
client_rc=$?
obs "client: $(tr '\n' '|' <"$CASE_DIR/client.out")"
ipv4=$(sed -n 's/.*RESULT ipv4=\([a-z:]*\).*/\1/p' "$CASE_DIR/client.out")
ipv6=$(sed -n 's/.*RESULT ipv6=\([a-z:]*\).*/\1/p' "$CASE_DIR/client.out")
_n=0
while [ "$_n" -lt 20 ]; do
	dev_bytes=$(wc -c <"$CASE_DIR/stream.bin" 2>/dev/null | tr -d ' ')
	[ "${dev_bytes:-0}" -ge 1024 ] && break
	sleep 0.1
	_n=$((_n + 1))
done
obs_kv "IPv4 termination" "$ipv4"
obs_kv "IPv6 termination" "$ipv6"
obs_kv "bytes the printer received" "$dev_bytes"

daemon_stop
kill -TERM "$DRAIN" 2>/dev/null
wait "$DRAIN" 2>/dev/null
obs "daemon log:"
sed 's/^/    /' "$CASE_DIR/daemon.log" | while read -r l; do obs "$l"; done

assert_eq "$client_rc" "0" "both address families are served"
assert_eq "$ipv4" "fin" "the IPv4 job ends cleanly"
assert_eq "$ipv6" "fin" "the IPv6 job ends cleanly"
assert_eq "$dev_bytes" "1024" "both jobs reach the printer in full"
assert_contains "$CASE_DIR/daemon.log" "Connection from ::1" \
	"the daemon really accepted a connection on the IPv6 listener"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
