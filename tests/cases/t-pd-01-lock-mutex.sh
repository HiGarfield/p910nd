#!/bin/sh
#
# PD-01  [严重]  The printer lock must be taken in EVERY build.
#
# Before the fix, get_lock() sat inside `#ifdef LOCKFILE_DIR` and no build
# defined that macro, so lockfd stayed -1, lock_printer_job() returned success
# without locking anything, and two concurrent jobs interleaved on one printer.
# The archived pre-fix evidence (0 lock syscalls vs 1, and 73 byte-stream
# transitions vs 1) is quoted in DEFECT_REPORT.md; this case asserts the fixed
# behaviour so a regression is caught.
#
# A: the shipping build, with no -D at all, must reach the lock
# B: two concurrent jobs must be serialised

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-01 lock-mutex
DEFAULT_LOCK=/var/lock/subsys/p9100d

# ---------------------------------------------------------------- part A ----
# The bind address cannot be resolved, so the process walks the whole start-up
# path -- which is where the lock is taken -- and then exits.  No TCP port is
# touched.  As a non-root user the lock cannot actually be *created* under
# /var/lock, so what is asserted here is that the code path is no longer
# compiled out: the attempt must be visible.

say "A: shipping build, no -D, strace of the start-up path"
# stdin comes from /dev/null so is_standalone() sees a non-socket and takes the
# daemon path whatever the harness inherited.
strace -f -o "$CASE_DIR/a.trace" -e trace=openat,open,fcntl,mkdir \
	"$BUILD_DIR/p910nd-baseline" -d -i 300.300.300.300 0 \
	>"$CASE_DIR/a.out" 2>&1 </dev/null
a_status=$?
obs_kv "A exit status" "$a_status"
obs "A stdout/stderr: $(tr '\n' '|' <"$CASE_DIR/a.out")"
obs_kv "A attempts to open the default lock path" \
	"$(count_lines "$CASE_DIR/a.trace" "$DEFAULT_LOCK")"
obs_kv "A mkdir attempts" "$(count_lines "$CASE_DIR/a.trace" 'mkdir(')"
obs_kv "A record locks taken" "$(count_lines "$CASE_DIR/a.trace" 'F_SETLK')"
obs "A lock-related syscalls:"
grep -E "openat\(.*p9100d|mkdir\(|F_SETLK" "$CASE_DIR/a.trace" | sed 's/^/    /' | head -8 |
	while read -r l; do obs "$l"; done

assert_ge "$(count_lines "$CASE_DIR/a.trace" "$DEFAULT_LOCK")" "1" \
	"A the shipping build does try to open the lock file"
assert_contains "$CASE_DIR/a.out" "Permission denied" \
	"A /var/lock/subsys exists but is not writable here, so the failure is EACCES"
assert_eq "$a_status" "1" \
	"A a daemon that cannot take its lock refuses to start instead of serving unprotected"

say "A': static confirmation"
"$PYTHON" "$TOOLS_DIR/static_checks.py" --check PD-01 >"$CASE_DIR/a.static" 2>&1
sed 's/^/    /' "$CASE_DIR/a.static" | while read -r l; do obs "$l"; done
assert_contains "$CASE_DIR/a.static" "get_lock-unguarded  *OK" "A' get_lock() is not guarded any more"
assert_contains "$CASE_DIR/a.static" "job-lock-dead-code *OK" "A' the job lock can no longer be a no-op"

# ---------------------------------------------------------------- part B ----
# Two clients, one device: each sends a distinguishable byte slowly enough that
# both jobs are in flight together.  With the lock in place the device sees one
# job and then the other; before the fix the byte stream interleaved (73
# transitions in the recorded run).

run_concurrency() {
	# run_concurrency <label> <variant>
	_label=$1
	_variant=$2
	_dir="$CASE_DIR/b_$_label"
	mkdir -p "$_dir"

	"$PYTHON" "$TOOLS_DIR/devnode.py" fifo "$_dir/lp0" >"$_dir/node.out" 2>&1
	"$PYTHON" "$TOOLS_DIR/devnode.py" drain --fifo "$_dir/lp0" \
		--out "$_dir/stream.bin" >"$_dir/drain.out" 2>&1 &
	_drain_pid=$!
	sleep 0.3

	daemon_start "$_dir/daemon.log" "$BUILD_DIR/$_variant" \
		-d -i 127.0.0.1 -f "$_dir/lp0" 0
	if [ -z "$DAEMON_PID" ]; then
		obs "$_label: daemon failed to start: $(tr '\n' '|' <"$_dir/daemon.log")"
		kill "$_drain_pid" 2>/dev/null
		return 1
	fi
	wait_for_port 127.0.0.1 "$TEST_PORT" 5 || obs "$_label: port never came up"

	"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" \
		--send-repeat 300000 --send-byte 0xAA --chunk 8192 --chunk-delay 0.01 \
		--half-close --label "A-$_label" >"$_dir/clientA.out" 2>&1 &
	_ca=$!
	"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" \
		--send-repeat 300000 --send-byte 0xBB --chunk 8192 --chunk-delay 0.01 \
		--half-close --label "B-$_label" >"$_dir/clientB.out" 2>&1 &
	_cb=$!
	wait "$_ca" "$_cb"

	sleep 1
	daemon_stop
	sleep 0.3
	kill -TERM "$_drain_pid" 2>/dev/null
	wait "$_drain_pid" 2>/dev/null

	cat "$_dir/clientA.out" "$_dir/clientB.out" | sed 's/^/    /' | while read -r l; do obs "$l"; done
	obs "$_label daemon log:"
	sed 's/^/    /' "$_dir/daemon.log" | while read -r l; do obs "$l"; done
	_stats=$("$PYTHON" "$TOOLS_DIR/stream_transitions.py" "$_dir/stream.bin")
	obs "$_label: byte-stream transitions/total = $_stats"
	set -- $_stats
	eval "CONC_$_label=\$1"
	eval "CONC_BYTES_$_label=\$2"
	rm -f "$_dir/lp0"
}

say "B: two concurrent clients, job lock in place"
run_concurrency locked test-lockdir || obs "B run failed"

obs_kv "B stream transitions" "${CONC_locked:-?}"
obs_kv "B stream bytes" "${CONC_BYTES_locked:-?}"

assert_eq "${CONC_BYTES_locked:-0}" "600000" "B both jobs were delivered in full"
assert_lt "${CONC_locked:-999}" "3" "B and the device saw them one after the other, not interleaved"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
