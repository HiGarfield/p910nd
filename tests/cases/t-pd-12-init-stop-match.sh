#!/bin/sh
#
# PD-12  [中低]  The init script must be able to stop the daemon.
#
# main() rewrites its own argv[0] so that ps shows the printer port, which is a
# documented feature -- but it means the process is no longer findable under the
# name aux/p910nd.conf installs and aux/p910nd.init hands to killproc.  The
# rename is kept (it is the documented behaviour); the init script now stops the
# daemon through its pid file and only falls back to the name-based lookup.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-12 init-stop-match
CASE_BIN="$CASE_DIR/p910nd"

# ------------------------------------------------------- why the fix is needed
cp "$BUILD_DIR/test" "$CASE_BIN" || exit 1
obs_kv "binary invoked as" "$CASE_BIN"

daemon_start "$CASE_DIR/daemon.log" "$CASE_BIN" -d -i 127.0.0.1 -f "$CASE_DIR/lp0" 0
if [ -z "$DAEMON_PID" ]; then
	sed 's/^/    /' "$CASE_DIR/daemon.log" | while read -r l; do obs "$l"; done
	assert_true 0 "the daemon starts under its installed name"
	kill_stray
	exit 1
fi
: >"$CASE_DIR/lp0"
wait_for_port 127.0.0.1 "$TEST_PORT" 5

cmdline=$(tr '\0' ' ' <"/proc/$DAEMON_PID/cmdline")
obs_kv "/proc/$DAEMON_PID/cmdline" "$cmdline"
n_named=$(pgrep -f "^$CASE_BIN\$" 2>/dev/null | wc -l)
n_renamed=$(pgrep -f "p9100d" 2>/dev/null | wc -l)
obs_kv "pgrep -f '^$CASE_BIN\$' matches" "$n_named"
obs_kv "pgrep -f 'p9100d' matches" "$n_renamed"
daemon_stop

# The rename is intentional and stays; this is the reason the init script needs
# a different route.
assert_str_contains "$cmdline" "p9100d" "the process is still renamed to p9100d (documented)"
assert_eq "$n_named" "0" "and is no longer findable under the installed name"
assert_ge "$n_renamed" "1" "only under the new name"

# ------------------------------------------------------------- the fix itself
say "the init script stops the daemon through its pid file"
INIT="$REPO_ROOT/aux/p910nd.init"
stop_branch=$(sed -n '/^    stop)/,/^    ;;/p' "$INIT")
obs "stop branch:"
printf '%s\n' "$stop_branch" | sed 's/^/    /' | while read -r l; do obs "$l"; done

assert_contains "$INIT" "P910ND_PIDFILE" "the stop branch looks at a pid file"
assert_str_contains "$stop_branch" "/var/run/p910[0-9]d.pid" "namely the one the daemon writes"
assert_str_contains "$stop_branch" 'kill -TERM "$P910ND_PID"' "and signals that pid"
assert_str_contains "$stop_branch" 'killproc -TERM $P910ND_BIN' \
	"with the old name-based lookup kept as a fallback"
assert_str_contains "$stop_branch" '/proc/$p910nd_p/cmdline' \
	"a stale pid file is not acted on: the pid is checked before use"
assert_str_contains "$stop_branch" 'case "$p910nd_p" in' \
	"and the file content must be a number at all"

say "the script is syntactically valid"
# bash, not sh: the script uses array syntax that dash rejects, which predates
# this fix and is not something to change here.
if bash -n "$INIT" 2>"$CASE_DIR/syntax.log"; then
	obs "bash -n: OK"
	assert_true 0 "the init script parses"
else
	sed 's/^/    /' "$CASE_DIR/syntax.log" | while read -r l; do obs "$l"; done
	assert_true 1 "the init script parses"
fi

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
