#!/bin/sh
#
# PD-08  [中]  The wait for the printer lock must be boundable, and must stay
# unbounded by default.
#
# a11690e turned "printer busy" from "refuse the job" into "queue the job", but
# the queue had no bound: F_SETLKW waits forever and only a signal can break it,
# so one wedged job blocked every later job for that printer permanently.  A
# non-zero LOCK_WAIT_TIMEOUT now bounds the wait with F_SETLK polling and
# reports the timeout to the client; the default is 0, i.e. exactly the old
# behaviour.
#
# A: LOCK_WAIT_TIMEOUT=0 (default) still waits forever
# B: LOCK_WAIT_TIMEOUT=2 gives up after two seconds and tells the client

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-08 job-lock-hol
CASE_BIN=$(build_variant test-joblock "-DLOCKFILE_DIR=\"$LOCK_DIR_OK\"")
assert_file_exists "$CASE_BIN" "the default variant builds"
bounded=$(build_variant test-joblock-timeout \
	"-DLOCKFILE_DIR=\"$CASE_DIR/lock\"" -DLOCK_WAIT_TIMEOUT=2)
assert_file_exists "$bounded" "the bounded variant builds"
LOCKFILE="$LOCK_DIR_OK/p9100d"
DEV="$CASE_DIR/lp0"

occupy_job_lock() {
	# occupy_job_lock <lockfile> <holder-out>
	mkdir -p "$(dirname "$1")"
	rm -f "$1"
	"$PYTHON" "$TOOLS_DIR/lockhold.py" hold --file "$1" --byte 1 \
		--seconds 40 >"$2" 2>&1 &
	HOLDER=$!
	for _i in 1 2 3 4 5 6 7 8 9 10; do
		grep -q LOCKED "$2" 2>/dev/null && break
		sleep 0.2
	done
	obs "lock holder: $(tr '\n' '|' <"$2")"
}

# ---------------------------------------------------------------- part A ----
say "A: default LOCK_WAIT_TIMEOUT=0 -- the wait is still unbounded"
: >"$DEV"
occupy_job_lock "$LOCKFILE" "$CASE_DIR/holder.out"
assert_contains "$CASE_DIR/holder.out" "byte=1" "A the fixture holds the job lock range"

strace -f -o "$CASE_DIR/a.trace" -e trace=fcntl,write \
	"$CASE_BIN" -d -i 127.0.0.1 -f "$DEV" 0 >"$CASE_DIR/a.daemon.log" 2>&1 &
DPID=$!
wait_for_port 127.0.0.1 "$TEST_PORT" 5
t0=$(date +%s.%N)
"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" --send-repeat 4096 \
	--send-byte 0x63 --half-close --expect-reply 1 --read-timeout 8 \
	--label HOL >"$CASE_DIR/a.client" 2>&1
t1=$(date +%s.%N)
elapsed=$("$PYTHON" -c "print('%.2f' % ($t1 - $t0))")
dev_bytes=$(wc -c <"$DEV" | tr -d ' ')

kill -TERM "$DPID" 2>/dev/null
sleep 0.3
kill "$HOLDER" 2>/dev/null
wait "$HOLDER" 2>/dev/null
kill_stray

obs_kv "A client-visible duration (s)" "$elapsed"
obs "A client: $(tr '\n' '|' <"$CASE_DIR/a.client")"
obs_kv "A bytes that reached the printer while the lock was held" "$dev_bytes"
obs_kv "A F_SETLKW calls on byte 1" \
	"$(count_lines "$CASE_DIR/a.trace" 'F_SETLKW, {l_type=F_WRLCK, l_whence=SEEK_SET, l_start=1')"
obs "A fcntl calls seen by the daemon:"
grep 'F_SETLK' "$CASE_DIR/a.trace" | sed 's/^/    /' | head -6 | while read -r l; do obs "$l"; done

assert_contains "$CASE_DIR/a.trace" "l_start=1" "A the job child does try to take the job lock"
assert_contains "$CASE_DIR/a.trace" "F_SETLKW" "A and blocks in the kernel, as before"
assert_eq "$dev_bytes" "0" "A not a single job byte reaches the printer"
assert_eq "$(sed -n 's/.*RESULT how=\([a-z-]*\).*/\1/p' "$CASE_DIR/a.client")" "timeout" \
	"A the client is left waiting, with no answer and no failure"

# ---------------------------------------------------------------- part B ----
say "B: LOCK_WAIT_TIMEOUT=2 -- the wait is bounded and reported"
: >"$CASE_DIR/lp0b"
occupy_job_lock "$CASE_DIR/lock/p9100d" "$CASE_DIR/holder-b.out"
assert_contains "$CASE_DIR/holder-b.out" "byte=1" "B the fixture holds the job lock range"

daemon_start "$CASE_DIR/b.daemon.log" "$bounded" -d -i 127.0.0.1 -f "$CASE_DIR/lp0b" 0
wait_for_port 127.0.0.1 "$TEST_PORT" 5
b0=$(date +%s.%N)
"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" --send-repeat 4096 \
	--send-byte 0x63 --half-close --expect-reply 1 --read-timeout 20 \
	--label BOUNDED >"$CASE_DIR/b.client" 2>&1
b1=$(date +%s.%N)
bdur=$("$PYTHON" -c "print('%.2f' % ($b1 - $b0))")
bbytes=$(wc -c <"$CASE_DIR/lp0b" | tr -d ' ')
daemon_stop
kill "$HOLDER" 2>/dev/null
wait "$HOLDER" 2>/dev/null
kill_stray

obs_kv "B client-visible duration (s)" "$bdur"
obs "B client: $(tr '\n' '|' <"$CASE_DIR/b.client")"
obs_kv "B bytes that reached the printer" "$bbytes"
obs "B daemon log:"
sed 's/^/    /' "$CASE_DIR/b.daemon.log" | while read -r l; do obs "$l"; done
b_how=$(sed -n 's/.*RESULT how=\([a-z-]*\).*/\1/p' "$CASE_DIR/b.client")

assert_eq "$b_how" "rst" "B the client is told the job failed instead of hanging"
assert_f_ge "${bdur:-0}" "2" "B after at least the configured two seconds"
assert_f_lt "${bdur:-99}" "8" "B and not much later than that"
assert_eq "$bbytes" "0" "B no partial job reached the printer"
assert_contains "$CASE_DIR/b.daemon.log" "still busy after 2 seconds" \
	"B the timeout is logged with its cause"
assert_contains "$CASE_DIR/b.daemon.log" "could not take the job lock" \
	"B and the refusal names the reason"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
