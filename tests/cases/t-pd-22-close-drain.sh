#!/bin/sh
#
# PD-22  [medium]  A clean close must drain the client's in-flight tail.
#
# set_socket_options() makes the accepted socket non-blocking.  Linux ignores
# SO_RCVTIMEO on a non-blocking descriptor, so the drain loop in
# close_connection() called read() once, got EAGAIN immediately and closed the
# socket while the peer still had bytes in flight.  close() on unread data
# sends RST, which makes CUPS treat a finished job as failed and print it
# again.
#
# JOB_MAX_DURATION=1 ends the copy loop at a known time, so a client can send
# a tail while the daemon is in the drain window.  strace -T records the
# 65536-byte read() of that loop, the only read of that size in the process
# (the copy loop reads 8192-byte buffers).

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-22 close-drain

LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR"
CASE_BIN=$(build_variant test-drain "-DLOCKFILE_DIR=\"$LOCKDIR\"" \
	-DJOB_MAX_DURATION=1)
assert_file_exists "$CASE_BIN" "the drain variant builds"
DEV="$CASE_DIR/lp0"
: >"$DEV"

if ! command -v strace >/dev/null 2>&1; then
	obs "strace is required by this case"
	assert_true 1 "strace is available"
	kill_stray
	exit 1
fi

daemon_start "$CASE_DIR/daemon.log" strace -f -T -e trace=read \
	-o "$CASE_DIR/strace.log" "$CASE_BIN" -d -i 127.0.0.1 -f "$DEV" 0
if [ -z "$DAEMON_PID" ]; then
	obs "daemon failed to start: $(tr '\n' '|' <"$CASE_DIR/daemon.log")"
	assert_true 1 "the daemon starts"
	kill_stray
	exit 1
fi
wait_for_port 127.0.0.1 "$TEST_PORT" 5 || obs "the port never came up"

"$PYTHON" - "$TEST_PORT" >"$CASE_DIR/client.out" 2>&1 <<'EOF'
import socket, sys, time
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), 5)
# copy_stream() leaves after JOB_MAX_DURATION (1s); send the tail while the
# daemon is draining the connection it has just sent FIN on.
time.sleep(1.3)
sent = 0
try:
    s.sendall(b"Z" * 4096)
    sent = 4096
except OSError:
    sent = -1
s.settimeout(12)
how = "none"
try:
    while True:
        if not s.recv(65536):
            how = "fin"
            break
except ConnectionResetError:
    how = "rst"
except socket.timeout:
    how = "timeout"
except OSError:
    how = "oserror"
print("RESULT how=%s sent=%d" % (how, sent))
s.close()
EOF
sleep 0.4
obs "client: $(tr '\n' '|' <"$CASE_DIR/client.out")"
c_how=$(sed -n 's/.*RESULT how=\([a-z-]*\).*/\1/p' "$CASE_DIR/client.out")
obs_kv "client termination" "$c_how"

"$PYTHON" - "$CASE_DIR/strace.log" >"$CASE_DIR/drain.out" 2>&1 <<'EOF'
import re, sys
lines = open(sys.argv[1], errors="replace").read().splitlines()
drained = [l for l in lines
           if re.search(r"read\(\d+, .*65536\)\s*=\s*4096\b", l)]
eagain = [l for l in lines
          if re.search(r"read\(\d+, .*65536\)\s*=\s*-1 EAGAIN", l)]
print("drained_tail_reads=%d" % len(drained))
print("immediate_eagain_reads=%d" % len(eagain))
for l in (drained + eagain)[:4]:
    print("trace: %s" % l.strip())
EOF
obs "strace: $(tr '\n' '|' <"$CASE_DIR/drain.out")"
drained=$(sed -n 's/^drained_tail_reads=\([0-9]*\)$/\1/p' "$CASE_DIR/drain.out")
eagain=$(sed -n 's/^immediate_eagain_reads=\([0-9]*\)$/\1/p' "$CASE_DIR/drain.out")

daemon_stop
obs "daemon log:"
sed 's/^/    /' "$CASE_DIR/daemon.log" | while read -r l; do obs "$l"; done

assert_eq "$c_how" "fin" "the finished job still ends with a clean FIN"
assert_ge "${drained:-0}" "1" \
	"the daemon drained the client's in-flight tail before closing"
assert_eq "${eagain:-1}" "0" \
	"the drain read waited instead of returning EAGAIN at once"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
