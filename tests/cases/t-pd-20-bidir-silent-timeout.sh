#!/bin/sh
#
# PD-20  [高]  A bidirectional job that pauses must get SILENT_TIMEOUT too.
#
# The probe/real-job distinction (a client that never sent anything gets the
# short IDLE_TIMEOUT, one that has streamed data gets SILENT_TIMEOUT) was only
# ever wired into the unidirectional branch of copy_stream().  The bidirectional
# branch kept comparing against IDLE_TIMEOUT unconditionally, so a client that
# had already sent part of its job and then paused - CUPS building pages, a slow
# link, a client that sends a PJL header and then waits - had its job cut short.
# The printer kept whatever had arrived, the job was reported as failed and the
# client (CUPS) reprinted it: the same pages came out twice.
#
# With the scaled test profile IDLE_TIMEOUT is 3s and SILENT_TIMEOUT is 12s, so
# a 6s mid-job pause separates the two behaviours exactly.
#
# A: the job survives the pause and is delivered in full
# B: control - a client that never sends anything is still released promptly

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-20 bidir-silent-timeout

LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR"
CASE_BIN=$(build_variant test-bidir-silent "-DLOCKFILE_DIR=\"$LOCKDIR\"")
assert_file_exists "$CASE_BIN" "the variant builds"

# A regular file stands in for the printer: writes always succeed and reads hit
# EOF at once, so the only thing under test is the client's pause.
DEV="$CASE_DIR/lp0"
: >"$DEV"

daemon_start "$CASE_DIR/daemon.log" "$CASE_BIN" -b -d -i 127.0.0.1 -f "$DEV" 0
if [ -z "$DAEMON_PID" ]; then
	obs "daemon failed to start: $(tr '\n' '|' <"$CASE_DIR/daemon.log")"
	assert_true 1 "the daemon starts"
	kill_stray
	exit 1
fi
wait_for_port 127.0.0.1 "$TEST_PORT" 5 || obs "the port never came up"

# ---------------------------------------------------------------- part A ----
say "A: -b, client sends half the job, pauses 6s, sends the rest"
# A pause longer than IDLE_TIMEOUT (3s here) and shorter than SILENT_TIMEOUT
# (12s): a real job must survive it, only a probe may not.
"$PYTHON" - "$TEST_PORT" >"$CASE_DIR/a.client" 2>&1 <<'EOF'
import socket, sys, time
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), 5)
s.sendall(b"A" * 4096)
time.sleep(6)			# the legitimate mid-job pause
s.sendall(b"B" * 4096)
s.shutdown(socket.SHUT_WR)
s.settimeout(20)
how = "none"
received = 0
try:
    while True:
        buf = s.recv(65536)
        if not buf:
            how = "fin"
            break
        received += len(buf)
except ConnectionResetError:
    how = "rst"
except socket.timeout:
    how = "timeout"
print("RESULT how=%s sent=8192 received=%d" % (how, received))
sys.exit(1 if how == "rst" else 0)
EOF
a_status=$?

sleep 0.5
a_bytes=$(wc -c <"$DEV" | tr -d ' ')
a_how=$(sed -n 's/.*RESULT how=\([a-z-]*\).*/\1/p' "$CASE_DIR/a.client")
obs_kv "A termination" "$a_how"
obs_kv "A bytes the printer received" "$a_bytes"
obs "A client: $(tr '\n' '|' <"$CASE_DIR/a.client")"

assert_eq "$a_bytes" "8192" \
	"A the whole job reaches the printer: the pause was not mistaken for a dead client"
assert_not_contains "$CASE_DIR/a.client" "how=rst" \
	"A and the job is not reported as failed, so the client will not reprint it"
assert_not_contains "$CASE_DIR/daemon.log" "no data transferred for 3 seconds" \
	"A the short probe timeout did not fire on a client that had sent data"

# ---------------------------------------------------------------- part B ----
say "B: control -- a client that never sends anything is still released promptly"
: >"$DEV"
b0=$(date +%s.%N)
"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" --linger 1 \
	--expect-reply 1 --read-timeout 15 --label B >"$CASE_DIR/b.client" 2>&1
b1=$(date +%s.%N)
b_dur=$("$PYTHON" -c "print('%.2f' % ($b1 - $b0))")
b_how=$(sed -n 's/.*RESULT how=\([a-z-]*\).*/\1/p' "$CASE_DIR/b.client")
obs_kv "B client-visible duration (s)" "$b_dur"
obs_kv "B termination" "$b_how"
obs "B client: $(tr '\n' '|' <"$CASE_DIR/b.client")"

assert_eq "$b_how" "fin" "B a silent client is still released"
# The deadline is compared on whole seconds, so it can fire up to a second
# early: 2s is the floor for a 3s IDLE_TIMEOUT.
assert_f_ge "${b_dur:-0}" "2" "B after the probe timeout"
assert_f_lt "${b_dur:-99}" "12" "B and not after the long one, so the distinction survives"

daemon_stop
obs "daemon log:"
assert_contains "$CASE_DIR/daemon.log" "no data transferred for 3 seconds" \
	"B the probe timeout still fires for a client that never sent anything"
sed 's/^/    /' "$CASE_DIR/daemon.log" | while read -r l; do obs "$l"; done

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
