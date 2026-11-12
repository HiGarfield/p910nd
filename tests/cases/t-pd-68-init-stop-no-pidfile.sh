#!/bin/sh
#
# PD-68  [high]  stop must use the same process source as status.
#
# PD-59 made status find a live daemon by argv[0] even without a pid file
# (foreground -d, TESTING build, /var/run cleaned).  The stop branch still
# only read /var/run/p910[0-9]d.pid: for such a daemon P910ND_PIDS stayed
# empty, the fallback `killproc -TERM` matched the *installed* name (which
# main() renamed away), and stop returned success while the daemon lived.
# try-restart then tried to start a second instance.
#
# A: a live p9100d with no pid file must be signalled by stop.
# B: after stop, status no longer claims it is running.
# C: static -- stop calls the shared pid helper.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-68 init-stop-no-pidfile

INIT="$REPO_ROOT/aux/p910nd.init"
PIDDIR="$CASE_DIR/piddir"
BIN="$CASE_DIR/bin"
CONFIG="$CASE_DIR/p910nd.conf"
mkdir -p "$PIDDIR" "$BIN"
cp /bin/true "$BIN/p910nd" || true
chmod +x "$BIN/p910nd"
printf 'P910ND_NUM=""\nP910ND_OPTS=""\n' >"$CONFIG"
sed -e "s|^P910ND_BIN=/usr/sbin/p910nd|P910ND_BIN=$BIN/p910nd|" \
	-e "s|^P910ND_CONFIG=/etc/sysconfig/p910nd|P910ND_CONFIG=$CONFIG|" \
	"$INIT" >"$CASE_DIR/init-copy"
chmod +x "$CASE_DIR/init-copy"

# The stand-in daemon writes a marker on SIGTERM.  execv with a custom
# argv[0] gives it the p910<port>d name main() sets; the code is read before
# exec so the signal handler is installed in the new image.
cat >"$CASE_DIR/fake.py" <<'EOF'
import os, signal, time
marker = os.environ["P910ND_MARKER"]
def on_term(signum, frame):
    with open(marker, "w") as fh:
        fh.write("TERM\n")
    os._exit(0)
signal.signal(signal.SIGTERM, on_term)
while True:
    time.sleep(1)
EOF

start_fake() {
	_marker=$1
	_pidfile=$2
	(
		P910ND_MARKER="$_marker" "$PYTHON" -c '
import os, sys
os.execv(sys.executable,
         ["p9100d", "-c", open(sys.argv[1], encoding="utf-8").read()])
' "$CASE_DIR/fake.py" &
		_fake=$!
		printf '%s\n' "$_fake" >"$_pidfile"
		# Reap immediately so /proc/PID disappears as soon as it exits
		# and the init script's grace loop does not wait 40 seconds.
		wait "$_fake"
	) >/dev/null 2>&1 &
}

say "A: stop signals a daemon that has no pid file"
rm -f "$CASE_DIR/term.marker"
start_fake "$CASE_DIR/term.marker" "$CASE_DIR/fake.pid"
_n=0
FAKE_PID=
while [ "$_n" -lt 50 ]; do
	[ -s "$CASE_DIR/fake.pid" ] && FAKE_PID=$(cat "$CASE_DIR/fake.pid")
	if [ -n "$FAKE_PID" ]; then
		_argv0=$(tr '\0' '\n' <"/proc/$FAKE_PID/cmdline" 2>/dev/null | sed -n 1p)
		[ "$_argv0" = "p9100d" ] && break
	fi
	sleep 0.1
	_n=$((_n + 1))
done
obs_kv "fake daemon pid" "${FAKE_PID:-missing}"
obs_kv "fake daemon argv0" "${_argv0:-missing}"
obs_kv "pid files present" "$(ls "$PIDDIR" | wc -l | tr -d ' ')"
assert_eq "${_argv0:-missing}" "p9100d" \
	"the fake daemon has the renamed argv[0]"

P910ND_PIDDIR="$PIDDIR" bash "$CASE_DIR/init-copy" status \
	>"$CASE_DIR/status-before.out" 2>&1
assert_contains "$CASE_DIR/status-before.out" "running" \
	"status reports this daemon as running (the PD-59 behaviour)"

P910ND_PIDDIR="$PIDDIR" bash "$CASE_DIR/init-copy" stop \
	>"$CASE_DIR/stop.out" 2>&1
obs "stop output: $(tr '\n' '|' <"$CASE_DIR/stop.out")"
_m=0
while [ "$_m" -lt 50 ] && [ ! -f "$CASE_DIR/term.marker" ]; do
	sleep 0.1
	_m=$((_m + 1))
done
assert_file_exists "$CASE_DIR/term.marker" \
	"stop sends SIGTERM to the daemon it could not find in a pid file"

say "B: after stop, status no longer claims it is running"
P910ND_PIDDIR="$PIDDIR" bash "$CASE_DIR/init-copy" status \
	>"$CASE_DIR/status-after.out" 2>&1
status_rc=$?
obs "status-after exit: $status_rc"
obs "status-after output: $(tr '\n' '|' <"$CASE_DIR/status-after.out")"
[ "$status_rc" -ne 0 ]
_b_rc=$?
assert_true "$_b_rc" \
	"status and stop now agree that the daemon is gone"

say "C: static -- stop shares the status helper"
# PD-94: the case terminator uses a TAB; /^    ;;/ never matched and this
# slice included every later case branch.
stop_branch=$(sed -n '/^    stop)/,/^[[:space:]]*;;/p' "$INIT")
assert_str_contains "$stop_branch" "p910nd_running_pids" \
	"C the stop branch uses the same pid source as status"
assert_str_contains "$stop_branch" "PD-68: status()" \
	"C and documents why"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
