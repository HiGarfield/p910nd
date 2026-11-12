#!/bin/sh
#
# PD-53  [high]  kill_stray() must not SIGKILL unrelated processes.
#
# The harness sweeps /proc/*/cmdline for the literal build directory and
# kills every match.  Any process that merely MENTIONS that path -- an
# editor, another worktree's helper, or even kill_stray's own grep child --
# is killed, which can corrupt a test run without leaving a trace.
#
# A: a decoy whose argv contains the build path but no executable file
#    there must survive kill_stray().
# B: a real executable started from inside the build tree must still be
#    reaped, so the cleanup is not silently disabled.
# C: static -- the old substring grep is gone.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-53 kill-stray-scope

LIB="$TESTS_DIR/lib.sh"

# B's "leftover daemon": an executable file inside the build tree.
# A copied /bin/sleep is not portable: on a coreutils multicall host the
# renamed binary dispatches on argv[0] and dies with "unknown program".
# A shell wrapper would leave an orphaned `sleep 30` child behind when
# kill_stray() reaps the wrapper, so use one self-contained Python process
# whose argv still names an executable inside the build tree.
FAKE="$CASE_DIR/fake-daemon"
printf '#!%s\nimport time\ntime.sleep(30)\n' \
	"$(command -v "$PYTHON")" >"$FAKE"
chmod +x "$FAKE"

"$FAKE" 30 &
FAKE_PID=$!
# A's decoy: argv[1] mentions the build tree, but no such file exists.
"$PYTHON" -c 'import time
time.sleep(30)' "$BUILD_DIR/decoy-marker" &
DECOY_PID=$!

sleep 0.3
kill -0 "$FAKE_PID" 2>/dev/null
fake_up=$?
kill -0 "$DECOY_PID" 2>/dev/null
decoy_up=$?
obs_kv "before kill_stray: fake alive" "$fake_up"
obs_kv "before kill_stray: decoy alive" "$decoy_up"
assert_eq "$fake_up" "0" "both test processes are running"
assert_eq "$decoy_up" "0" "including the decoy"

kill_stray
wait "$FAKE_PID" 2>/dev/null
sleep 0.2

kill -0 "$DECOY_PID" 2>/dev/null
decoy_after=$?
kill -0 "$FAKE_PID" 2>/dev/null
fake_after=$?
obs_kv "after kill_stray: decoy alive" "$decoy_after"
obs_kv "after kill_stray: fake alive" "$fake_after"
assert_eq "$decoy_after" "0" \
	"A an unrelated process that only mentions the path survives"
[ "$fake_after" -ne 0 ]
_b_rc=$?
assert_true "$_b_rc" \
	"B a real executable from the build tree is still reaped"

say "C: static -- no substring kill remains"
assert_not_contains "$LIB" 'grep -q "$BUILD_DIR/"' \
	"C the old whole-command-line substring match is gone"
assert_contains "$LIB" "ARGV ENTRY" \
	"C the scope is documented"
assert_contains "$LIB" '\[ -f "$_arg" \]' \
	"C the match requires an executable regular file"

kill -TERM "$DECOY_PID" 2>/dev/null
wait "$DECOY_PID" 2>/dev/null
kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
