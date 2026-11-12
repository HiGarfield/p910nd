#!/bin/sh
# case-timeout: 150
#
# PD-109  [low]  A drain that hits its time cap must say so.
#
# close_connection() used one break for BOTH exit caps:
#   if (now - start >= DRAIN_TIMEOUT || total >= DRAIN_MAX_BYTES) break;
# but only the byte cap logged a LOG_NOTICE afterwards.  Hitting the time
# cap also leaves unread bytes behind (close() can then send RST and lose
# in-flight printer status data), yet the operator saw nothing at all --
# indistinguishable from a clean end of job.  This was listed in earlier
# reviews as "no log on the DRAIN_TIMEOUT branch" and left unfixed because
# the branch looked unreachable; compiling with -DDRAIN_TIMEOUT=0 makes it
# deterministic and proves the diagnostic gap is real.
#
# A: a zero-second cap produces the timeout log during a real job.
# B: control -- the default cap does not produce it.
# C: static -- both caps are separate and both have diagnostics.
# D: the byte-cap diagnostic is unchanged (PD-22 regression guard).

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-109 drain-timeout-log

SRC="$REPO_ROOT/p910nd.c"
LOCK="$CASE_DIR/lock"
DEV="$CASE_DIR/lp0"
mkdir -p "$LOCK" || exit 1
: >"$DEV"

run_job() {
	_tag=$1
	_bin=$2
	: >"$DEV"
	daemon_start "$CASE_DIR/$_tag.daemon.log" "$_bin" \
		-d -i 127.0.0.1 -f "$DEV" 0
	if [ -z "$DAEMON_PID" ]; then
		obs "$_tag daemon failed: $(tr '\n' '|' <"$CASE_DIR/$_tag.daemon.log")"
		return 1
	fi
	wait_for_port 127.0.0.1 "$TEST_PORT" 5 || return 1
	"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" \
		--send-repeat 64 --half-close --expect-reply 0 \
		--read-timeout 3 >"$CASE_DIR/$_tag.client" 2>&1
	_n=0
	while [ "$_n" -lt 30 ] && ! grep -q 'drain timed out' \
		"$CASE_DIR/$_tag.daemon.log" 2>/dev/null; do
		sleep 0.1
		_n=$((_n + 1))
	done
	sleep 0.2
	obs "$_tag client: $(tr '\n' '|' <"$CASE_DIR/$_tag.client")"
	obs "$_tag log: $(tr '\n' '|' <"$CASE_DIR/$_tag.daemon.log")"
	daemon_stop
	kill_stray
	return 0
}

say "A: a zero cap logs the timeout"
BIN0=$(build_variant test-drain0 \
	"-DDRAIN_TIMEOUT=0" "-DLOCKFILE_DIR=\"$LOCK\"")
assert_file_exists "$BIN0" "the zero-cap variant builds"
run_job zero "$BIN0"
assert_contains "$CASE_DIR/zero.daemon.log" \
	"close: drain timed out after 0 seconds" \
	"A the time cap produces a LOG_NOTICE with its actual limit"
assert_contains "$CASE_DIR/zero.client" "RESULT how=" \
	"A and the job still completes from the client's point of view"

say "B: control -- the default cap stays silent"
BIN1=$(build_variant test-drain-default "-DLOCKFILE_DIR=\"$LOCK\"")
assert_file_exists "$BIN1" "the default-cap variant builds"
run_job normal "$BIN1"
assert_not_contains "$CASE_DIR/normal.daemon.log" "drain timed out" \
	"B a normal job never trips the time cap"
assert_contains "$CASE_DIR/normal.client" "RESULT how=" \
	"B and completes normally"

say "C: static -- separate exits, symmetric diagnostics"
"$PYTHON" - "$SRC" >"$CASE_DIR/c.out" 2>&1 <<'EOF'
import ast, sys
# C source: use a textual check confined to close_connection().
s = open(sys.argv[1], encoding="utf-8").read()
start = s.index("static void close_connection(int fd, int failure)")
end = s.index("\n}\n", start) + 3
body = s[start:end]
flag = "int timed_out = 0;" in body
time_branch = "timed_out = 1;" in body
time_log = "close: drain timed out after %d seconds" in body
byte_log = "bytes still unread after draining" in body
separate = body.find("if (now.tv_sec - start.tv_sec >= DRAIN_TIMEOUT)") \
    < body.find("if (total >= DRAIN_MAX_BYTES)")
print("flag=%s time_branch=%s time_log=%s byte_log=%s separate=%s" %
      (flag, time_branch, time_log, byte_log, separate))
sys.exit(0 if all((flag, time_branch, time_log, byte_log, separate)) else 1)
EOF
assert_eq "$?" "0" \
	"C the time cap has its own exit and its own diagnostic"
assert_contains "$CASE_DIR/c.out" \
	"flag=True time_branch=True time_log=True byte_log=True separate=True" \
	"C every property was checked"
assert_contains "$SRC" "PD-109: the byte cap used to be the only exit" \
	"C the reason is documented"

say "D: control -- the byte-cap path still logs"
assert_eq "$(count_lines "$SRC" 'bytes still unread after draining')" "1" \
	"D the PD-22 byte-cap diagnostic is untouched"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
