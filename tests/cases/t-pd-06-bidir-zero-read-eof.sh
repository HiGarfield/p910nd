#!/bin/sh
#
# PD-06  [中低]  The zero-read counter must not end a job that is still printing.
#
# /dev/lpX and usblp never signal EOF, so the only end-of-data signal the
# bidirectional loop has for the reply is "the printer returned 0 bytes N times
# in a row".  Applied unconditionally, that counter also fires in the middle of a
# job: a printer that pauses between two answer blocks looks exactly like one
# that has nothing left to say, and its remaining reply is dropped.
#
# It is now consulted only once the print direction is finished; before that the
# count is restarted and the bounded reply window stays the authority.
#
# A: a client that has not half-closed must not trigger the counter
# B: once the client half-closes the counter still ends the job
# C: a device that reports EAGAIN rather than 0 is unaffected (unchanged)

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-06 bidir-zero-read-eof
CASE_BIN="$BUILD_DIR/test"
assert_file_exists "$CASE_BIN" "the instrumented variant builds"

# ---------------------------------------------------------------- part A ----
# The client connects, sends nothing and does NOT half-close, so the print
# direction is still unfinished for the whole observation window.
say "A: client holds the connection without half-closing"
daemon_start "$CASE_DIR/a.daemon.log" "$CASE_BIN" -d -b -i 127.0.0.1 -f /dev/null 0
# wait_for_port opens a connection of its own, and that throwaway 0-byte job
# legitimately triggers the counter (it half-closes).  Let it finish, then take
# the count as the baseline so only the measured job can move it.
wait_for_port 127.0.0.1 "$TEST_PORT" 5
sleep 1.0
base=$(count_lines "$CASE_DIR/a.daemon.log" 'sent no data, stop reading from printer')
obs_kv "A baseline notices (from the port probe's own job)" "$base"
"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" --send-repeat 0 \
	--linger 2.5 --expect-reply 1 --read-timeout 20 --label A \
	>"$CASE_DIR/a.client" 2>&1 &
CLIENT=$!
sleep 1.5
early=$(count_lines "$CASE_DIR/a.daemon.log" 'sent no data, stop reading from printer')
obs_kv "A zero-read notices while the client is still connected" "$early (baseline $base)"
wait "$CLIENT"
obs "A client: $(tr '\n' '|' <"$CASE_DIR/a.client")"
obs "A daemon log:"
sed 's/^/    /' "$CASE_DIR/a.daemon.log" | while read -r l; do obs "$l"; done
daemon_stop
assert_eq "$early" "$base" \
	"A the counter must not fire while the print direction is unfinished"
assert_contains "$CASE_DIR/a.daemon.log" "no data transferred" \
	"A the job was ended by the idle timeout instead"

# ---------------------------------------------------------------- part B ----
say "B: once the client half-closes the counter is allowed to end the job"
daemon_start "$CASE_DIR/b.daemon.log" "$CASE_BIN" -d -b -i 127.0.0.1 -f /dev/null 0
wait_for_port 127.0.0.1 "$TEST_PORT" 5
_t0=$(date +%s.%N)
"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" --send-repeat 256 \
	--send-byte 0x5A --half-close --expect-reply 1 --read-timeout 20 --label B \
	>"$CASE_DIR/b.client" 2>&1
_t1=$(date +%s.%N)
daemon_stop
dur=$("$PYTHON" -c "print('%.2f' % ($_t1 - $_t0))")
obs_kv "B job duration (s)" "$dur"
obs "B daemon log:"
sed 's/^/    /' "$CASE_DIR/b.daemon.log" | while read -r l; do obs "$l"; done
assert_contains "$CASE_DIR/b.daemon.log" "sent no data, stop reading from printer" \
	"B the counter still does its job once the print direction is finished"
assert_f_lt "$dur" "3" "B and it ends the job quickly"

# ---------------------------------------------------------------- part C ----
say "C: a FIFO reports EAGAIN, so the counter never fires at all"
"$PYTHON" "$TOOLS_DIR/devnode.py" fifo "$CASE_DIR/lp0" >"$CASE_DIR/c.node" 2>&1
daemon_start "$CASE_DIR/c.daemon.log" "$CASE_BIN" -d -b -i 127.0.0.1 \
	-f "$CASE_DIR/lp0" 0
wait_for_port 127.0.0.1 "$TEST_PORT" 5
"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" --send-repeat 256 \
	--send-byte 0x5A --half-close --expect-reply 65536 --read-timeout 8 \
	--out "$CASE_DIR/c.reply" --label C >"$CASE_DIR/c.client" 2>&1 &
C_PID=$!
sleep 1
"$PYTHON" "$TOOLS_DIR/devnode.py" repl --fifo "$CASE_DIR/lp0" \
	--data 'HELLO-FROM-PRINTER' --delay 0.4 --repeat 3 \
	>"$CASE_DIR/c.repl" 2>&1
wait "$C_PID"
daemon_stop
kill_stray
rm -f "$CASE_DIR/lp0"
obs "C printer wrote: $(tr '\n' '|' <"$CASE_DIR/c.repl")"
obs_kv "C bytes the client got back" "$(wc -c <"$CASE_DIR/c.reply" | tr -d ' ')"
assert_eq "$(count_lines "$CASE_DIR/c.daemon.log" 'sent no data, stop reading')" "0" \
	"C an idle FIFO reports EAGAIN, so the zero-read counter never advances"
assert_ge "$(wc -c <"$CASE_DIR/c.reply" | tr -d ' ')" "1" \
	"C and the printer's reply is delivered"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
