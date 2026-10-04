#!/bin/sh
#
# PD-17  [严重]  A second daemon instance must say so and exit, never queue.
#
# a11690e made take_lock() queue with F_SETLKW for *every* lock byte, and 5589df2
# (PD-01) removed the `#ifdef LOCKFILE_DIR` that had kept the instance lock out
# of the shipping build.  Together they made a second `p910nd 0` block for ever
# on the byte-0 lock:
#
#   * the daemonizing parent has already exited 0, so `service p910nd start`
#     reports success while nothing is listening;
#   * the blocked process quietly takes the port over the moment the first
#     instance stops, so a restart produces the wrong daemon.
#
# Byte 0 asks "am I the only p910nd for this printer?", which is the opposite
# question from byte 1's "may I use the printer now?" and needs the opposite
# answer.  Under (x)inetd byte 0 *is* the job lock, and there it still queues.
#
# A: a second instance reports the conflict and exits 1, promptly
# B: once the first instance is gone, a new one takes over and serves jobs

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-17 instance-lock-failfast

LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR"
CASE_BIN=$(build_variant test-instance "-DLOCKFILE_DIR=\"$LOCKDIR\"")
assert_file_exists "$CASE_BIN" "the variant builds"
DEV="$CASE_DIR/lp0"
: >"$DEV"

# ---------------------------------------------------------------- part A ----
say "A: with one instance running, a second one must refuse, not hang"
daemon_start "$CASE_DIR/a1.log" "$CASE_BIN" -d -i 127.0.0.1 -f "$DEV" 0
if [ -z "$DAEMON_PID" ]; then
	obs "first instance failed to start: $(tr '\n' '|' <"$CASE_DIR/a1.log")"
	assert_true 1 "the first instance starts"
	kill_stray
	exit 1
fi
wait_for_port 127.0.0.1 "$TEST_PORT" 5 || obs "A the port never came up"

a0=$(date +%s.%N)
# stdin is /dev/null so is_standalone() takes the daemon path whatever the
# harness inherited.  `timeout` is what turns "queued for ever" into a failure:
# the buggy build never returns and is killed with status 124.
timeout 15 "$CASE_BIN" -d -i 127.0.0.1 -f "$DEV" 0 \
	>"$CASE_DIR/a2.log" 2>&1 </dev/null
a_status=$?
a1=$(date +%s.%N)
a_dur=$("$PYTHON" -c "print('%.2f' % ($a1 - $a0))")

obs_kv "A second instance exit status" "$a_status"
obs_kv "A second instance duration (s)" "$a_dur"
obs "A second instance output:"
sed 's/^/    /' "$CASE_DIR/a2.log" | while read -r l; do obs "$l"; done

assert_eq "$a_status" "1" "A the second instance exits with a failure status"
assert_f_lt "${a_dur:-99}" "10" \
	"A and does so at once, instead of queueing behind the running one"
assert_contains "$CASE_DIR/a2.log" "another p910nd is already running" \
	"A it names the reason, so the operator is not left guessing"

daemon_stop
wait_for_gone 127.0.0.1 "$TEST_PORT" 10 || obs "A the port stayed up"

# ---------------------------------------------------------------- part B ----
say "B: with the first instance gone, a new one takes over and serves a job"
daemon_start "$CASE_DIR/b.log" "$CASE_BIN" -d -i 127.0.0.1 -f "$DEV" 0
if [ -n "$DAEMON_PID" ]; then
	wait_for_port 127.0.0.1 "$TEST_PORT" 5 || obs "B the port never came up"
	"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" --send-repeat 4096 \
		--send-byte 0x41 --half-close --expect-reply 1 --read-timeout 8 \
		--label B >"$CASE_DIR/b.client" 2>&1
else
	obs "B the new instance failed to start"
fi
daemon_stop
b_bytes=$(wc -c <"$DEV" | tr -d ' ')
obs_kv "B bytes that reached the printer" "$b_bytes"
obs "B client: $(tr '\n' '|' <"$CASE_DIR/b.client")"

assert_eq "$b_bytes" "4096" "B the takeover instance serves a job normally"
assert_not_contains "$CASE_DIR/b.client" "how=rst" \
	"B and reports it as printed, not as failed"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
