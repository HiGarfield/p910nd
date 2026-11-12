#!/bin/sh
#
# PD-92  [medium]  stop must clean the pid directory status reads.
#
# p910nd_running_pids() (status, try-restart, stop's /proc fallback) reads
# "$P910ND_PIDDIR"/p910[0-9]d.pid, but the stop branch's own pid-file loop
# and its cleanup were hard-coded to /var/run.  With P910ND_PIDDIR set to a
# different directory -- a container layout, a vendor prefix, or the test
# hook used by PD-49/59/68 -- stop left stale pid files behind even though
# its own comment says they would make monitoring and the next start believe
# the daemon was still running.  Measured: stop exits 0 and p9100d.pid is
# still present in the custom dir.
#
# A: stale pid files in the custom directory are removed, unrelated files
#    are not.
# B: static -- every pid-file path in the stop branch uses the same hook.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-92 init-piddir-consistency

INIT="$REPO_ROOT/aux/p910nd.init"
PIDDIR="$CASE_DIR/piddir"
BIN="$CASE_DIR/bin"
CONFIG="$CASE_DIR/p910nd.conf"
mkdir -p "$PIDDIR" "$BIN" || exit 1
cp /bin/true "$BIN/p910nd" && chmod +x "$BIN/p910nd"
printf 'P910ND_NUM=""\nP910ND_OPTS=""\n' >"$CONFIG"
sed -e "s|^P910ND_BIN=/usr/sbin/p910nd|P910ND_BIN=$BIN/p910nd|" \
	-e "s|^P910ND_CONFIG=/etc/sysconfig/p910nd|P910ND_CONFIG=$CONFIG|" \
	"$INIT" >"$CASE_DIR/init-copy"
chmod +x "$CASE_DIR/init-copy"

say "A: stop removes stale pid files from the configured directory"
printf '99999999\n' >"$PIDDIR/p9100d.pid"
printf '99999998\n' >"$PIDDIR/p9101d.pid"
printf 'keep\n' >"$PIDDIR/unrelated.txt"
P910ND_PIDDIR="$PIDDIR" bash "$CASE_DIR/init-copy" stop \
	>"$CASE_DIR/stop.out" 2>&1
STOP_RC=$?
obs "stop exit: $STOP_RC"
obs "stop output: $(tr '\n' '|' <"$CASE_DIR/stop.out")"
obs "pid dir after stop: $(ls "$PIDDIR" | tr '\n' ' ')"
assert_eq "$STOP_RC" "0" \
	"A stopping an absent service still reports success"
[ ! -e "$PIDDIR/p9100d.pid" ]
assert_true "$?" "A the first stale pid file is removed"
[ ! -e "$PIDDIR/p9101d.pid" ]
assert_true "$?" "A and the second one too"
assert_file_exists "$PIDDIR/unrelated.txt" \
	"A unrelated files in the directory are not deleted"

say "B: static -- one pid directory for status and stop"
"$PYTHON" - "$INIT" >"$CASE_DIR/b.out" 2>&1 <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
start = s.index("    stop)")
# The case terminator is indented with a TAB, not four spaces.
end = re.search(r"^\s*;;\s*$", s[start:], re.M)
stop = s[start:start + end.start()] if end else s[start:]
want = '"$P910ND_PIDDIR"/p910[0-9]d.pid'
hard = "/var/run/p910[0-9]d.pid"
helper = 'for p910nd_pf in "$P910ND_PIDDIR"/p910[0-9]d.pid' in s
n = stop.count(want)
k = stop.count(hard)
print("stop_uses_hook=%d stop_hardcoded=%d helper_uses_hook=%s" %
      (n, k, helper))
sys.exit(0 if n == 2 and k == 0 and helper else 1)
EOF
assert_eq "$?" "0" \
	"B both the read loop and the cleanup use P910ND_PIDDIR"
assert_contains "$CASE_DIR/b.out" "stop_uses_hook=2 stop_hardcoded=0 helper_uses_hook=True" \
	"B every call site was actually checked"
assert_contains "$INIT" "PD-92: status()/p910nd_running_pids()" \
	"B the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
