#!/bin/sh
#
# PD-02  [高]  A missing lock directory must not stop the daemon.
#
# Before the fix, get_lock() did not create LOCKDIR, so on any distribution
# without /var/lock/subsys the daemon refused to start -- a failure mode that
# only became reachable once PD-01 was fixed.  get_lock() now creates the
# directory component by component and retries.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-02 lock-dir-missing

DEEP="$CASE_DIR/lock/deeply/nested"
if [ -d "$DEEP" ]; then obs "fixture is broken: $DEEP already exists"; exit 1; fi
obs "fixture: the lock directory does not exist yet"

# A pristine build pointed at a directory that does not exist.  The parent
# ($CASE_DIR/lock) does, so mkdir -p style creation is possible for an
# unprivileged process; the default /var/lock case needs root and is covered
# by part C.
bin=$(build_variant test-lockdir-mkdir "-DLOCKFILE_DIR=\"$DEEP\"")
assert_file_exists "$bin" "the instrumented variant builds"

say "start the daemon with a lock directory three levels deep"
daemon_start "$CASE_DIR/daemon.log" "$bin" -d -i 127.0.0.1 -f "$CASE_DIR/lp0" 0
sleep 0.5

if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
	obs "the daemon is running"
	assert_true 0 "the daemon starts although the lock directory was missing"
else
	obs "the daemon exited: $(tr '\n' '|' <"$CASE_DIR/daemon.log")"
	assert_true 1 "the daemon starts although the lock directory was missing"
fi
obs "daemon log:"
sed 's/^/    /' "$CASE_DIR/daemon.log" | while read -r l; do obs "$l"; done

assert_dir_exists "$DEEP" "the lock directory was created"
assert_file_exists "$DEEP/p9100d" "and so was the lock file inside it"
obs_kv "lock file mode" "$(stat -c '%a' "$DEEP/p9100d" 2>/dev/null)"
obs_kv "lock file owner" "$(stat -c '%u:%g' "$DEEP/p9100d" 2>/dev/null)"

# The daemon must actually be serving now, not merely alive.
: >"$CASE_DIR/lp0"
wait_for_port 127.0.0.1 "$TEST_PORT" 5
"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" --send-repeat 4096 \
	--send-byte 0x4A --half-close --expect-reply 1 --read-timeout 15 \
	--label MK >"$CASE_DIR/client.out" 2>&1
obs "client: $(tr '\n' '|' <"$CASE_DIR/client.out")"
assert_ge "$(wc -c <"$CASE_DIR/lp0" | tr -d ' ')" "4096" "and it serves a job"
daemon_stop

say "B: the directory is only created when it is missing"
daemon_start "$CASE_DIR/b.log" "$bin" -d -i 127.0.0.1 -f "$CASE_DIR/lp0" 0
wait_for_port 127.0.0.1 "$TEST_PORT" 5
daemon_stop
assert_eq "$(count_lines "$CASE_DIR/b.log" 'No such file')" "0" \
	"B a second start does not complain, the directory is already there"

say "C: a lock directory that cannot be created is still fatal, and says so"
# /proc cannot hold a new directory, so this is a case no umask can fix.
deny=$(build_variant test-lockdir-denied "-DLOCKFILE_DIR=\"/proc/p910nd-deny\"")
daemon_start "$CASE_DIR/c.log" "$deny" -d -i 127.0.0.1 -f "$CASE_DIR/lp0" 0
sleep 0.5
if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
	daemon_stop
	assert_true 1 "an uncreatable lock directory is refused"
else
	assert_true 0 "an uncreatable lock directory is refused"
fi
obs "C daemon log:"
sed 's/^/    /' "$CASE_DIR/c.log" | while read -r l; do obs "$l"; done
assert_contains "$CASE_DIR/c.log" "p910nd-deny/p9100d" "C the refusal names the lock file"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
