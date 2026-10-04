#!/bin/sh
#
# PD-18  [严重]  Waiting for the printer must be bounded.
#
# f0ba0f3 made every *temporary* open failure retry for as long as it takes and
# 00a41bd bounded only the permanent ones.  A device can report a temporary
# failure for ever - ENXIO from a FIFO with no reader or from a USB backend that
# never comes back, EBUSY from a driver that keeps the node - so the job waited
# for ever.  The job holds the byte-1 job lock for the whole wait, which is what
# turned one unavailable printer into "this printer prints nothing at all":
# every later job queued behind the first one.
#
# A: the wait ends, is reported, and the client is told the job failed
# B: the printer works again afterwards - the job lock was not left held

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-18 printer-wait-bounded

LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR"
# A short ceiling and a short retry interval keep the case inside its time
# budget; both are build-time knobs, so the mechanism under test is unchanged.
CASE_BIN=$(build_variant test-printerwait "-DLOCKFILE_DIR=\"$LOCKDIR\"" \
	-DOPEN_PRINTER_MAX_WAIT=4 -DOPEN_PRINTER_RETRY_INTERVAL=2)
assert_file_exists "$CASE_BIN" "the variant builds"

# A FIFO with no reader: open(O_WRONLY|O_NONBLOCK) fails with ENXIO, which the
# classifier calls temporary - exactly the failure that used to be retried for
# ever.
FIFO="$CASE_DIR/lp0"
"$PYTHON" "$TOOLS_DIR/devnode.py" fifo "$FIFO" >"$CASE_DIR/fifo.out" 2>&1
obs "fifo: $(tr '\n' '|' <"$CASE_DIR/fifo.out")"

daemon_start "$CASE_DIR/daemon.log" "$CASE_BIN" -d -i 127.0.0.1 -f "$FIFO" 0
if [ -z "$DAEMON_PID" ]; then
	obs "daemon failed to start: $(tr '\n' '|' <"$CASE_DIR/daemon.log")"
	assert_true 1 "the daemon starts"
	kill_stray
	exit 1
fi
wait_for_port 127.0.0.1 "$TEST_PORT" 5 || obs "the port never came up"

# ---------------------------------------------------------------- part A ----
say "A: the printer reports ENXIO for ever -- the wait must end"
a0=$(date +%s.%N)
"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" --send-repeat 2048 \
	--send-byte 0x41 --half-close --expect-reply 1 --read-timeout 20 \
	--label A >"$CASE_DIR/a.client" 2>&1
a1=$(date +%s.%N)
a_dur=$("$PYTHON" -c "print('%.2f' % ($a1 - $a0))")
a_how=$(sed -n 's/.*RESULT how=\([a-z-]*\).*/\1/p' "$CASE_DIR/a.client")

obs_kv "A client-visible duration (s)" "$a_dur"
obs_kv "A termination" "$a_how"
obs "A client: $(tr '\n' '|' <"$CASE_DIR/a.client")"

assert_eq "$a_how" "rst" "A the client is told the job failed, not left hanging"
assert_f_ge "${a_dur:-0}" "3" "A after roughly the configured ceiling"
assert_f_lt "${a_dur:-99}" "16" "A and well inside the client's own deadline"

sleep 0.5
obs "A daemon log:"
sed 's/^/    /' "$CASE_DIR/daemon.log" | while read -r l; do obs "$l"; done
assert_contains "$CASE_DIR/daemon.log" "job abandoned" \
	"A giving up is logged as a job failure"
assert_contains "$CASE_DIR/daemon.log" "No such device or address" \
	"A with the real errno of the device, not a generic message"

# A printer that stays unavailable must also stay quiet: one LOG_ERR per job per
# retry interval buries everything else in syslog.
obs_kv "A 'will keep retrying' lines" \
	"$(count_lines "$CASE_DIR/daemon.log" 'will keep retrying')"
assert_lt "$(count_lines "$CASE_DIR/daemon.log" 'will keep retrying')" "3" \
	"A the retry noise is throttled instead of one line per attempt"

# ---------------------------------------------------------------- part B ----
say "B: once a reader exists the printer works again -- the lock was released"
"$PYTHON" "$TOOLS_DIR/devnode.py" drain --fifo "$FIFO" \
	--out "$CASE_DIR/stream.bin" >"$CASE_DIR/drain.out" 2>&1 &
DRAIN=$!
sleep 0.5
"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" --send-repeat 2048 \
	--send-byte 0x42 --half-close --expect-reply 1 --read-timeout 10 \
	--label B >"$CASE_DIR/b.client" 2>&1
b_how=$(sed -n 's/.*RESULT how=\([a-z-]*\).*/\1/p' "$CASE_DIR/b.client")
sleep 0.5
daemon_stop
kill -TERM "$DRAIN" 2>/dev/null
wait "$DRAIN" 2>/dev/null
b_bytes=$(wc -c <"$CASE_DIR/stream.bin" 2>/dev/null | tr -d ' ')

obs_kv "B termination" "$b_how"
obs_kv "B bytes the printer received" "$b_bytes"
obs "B client: $(tr '\n' '|' <"$CASE_DIR/b.client")"

assert_eq "$b_bytes" "2048" \
	"B a later job is served: the printer lock was not left held"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
