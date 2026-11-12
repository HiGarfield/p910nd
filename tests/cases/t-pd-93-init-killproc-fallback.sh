#!/bin/sh
#
# PD-93  [medium]  Stopping an absent service must not print option errors.
#
# When no daemon was found, the stop branch still called
# `killproc -TERM "$P910ND_BIN"` as a name-based fallback.  On hosts with
# /lib/lsb/init-functions that helper is a shell function whose options are
# parsed by getopts, so the leading signal produced four messages:
#   illegal option -- T / -- E / -- R / -- M
# on an otherwise successful `stop`.  The SUSE /sbin/killproc binary that
# accepts that syntax does not exist there.  Since PD-68, stop already uses
# p910nd_running_pids() (pid files AND argv[0]), so an empty pid list means
# the service is genuinely down and LSB requires a silent success.
#
# A: stop with no daemon is clean and reports success.
# B: stop with only a stale pid file is clean too and removes it.
# C: static -- the incompatible fallback is gone from real code lines.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-93 init-killproc-fallback

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

check_clean_stop() {
	_label=$1
	P910ND_PIDDIR="$PIDDIR" bash "$CASE_DIR/init-copy" stop \
		>"$CASE_DIR/$_label.out" 2>&1
	STOP_RC=$?
	obs "$_label exit: $STOP_RC"
	obs "$_label output: $(tr '\n' '|' <"$CASE_DIR/$_label.out")"
	assert_eq "$STOP_RC" "0" \
		"$_label stop still reports success"
	assert_contains "$CASE_DIR/$_label.out" "Shutting down p910nd" \
		"$_label the usual start message is printed"
	assert_not_contains "$CASE_DIR/$_label.out" "illegal option" \
		"$_label no option-parser errors"
	assert_not_contains "$CASE_DIR/$_label.out" "No such file or directory" \
		"$_label and no missing-helper errors"
}

say "A: stopping a service that is not running is silent"
check_clean_stop absent

say "B: a stale pid file does not reintroduce the fallback"
printf '99999999\n' >"$PIDDIR/p9100d.pid"
check_clean_stop stale
[ ! -e "$PIDDIR/p9100d.pid" ]
assert_true "$?" "B the stale pid file is still cleaned up (PD-92)"

say "C: static -- the incompatible call is gone"
"$PYTHON" - "$INIT" >"$CASE_DIR/c.out" 2>&1 <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
start = s.index("    stop)")
m = re.search(r"^\s*;;\s*$", s[start:], re.M)
branch = s[start:start + m.start()] if m else s[start:]
code = "\n".join(l for l in branch.splitlines()
                 if not l.lstrip().startswith("#"))
bad = code.count("killproc -TERM")
print("bare_fallback=%d documented=%s" %
      (bad, "PD-93: p910nd_running_pids()" in branch))
sys.exit(0 if bad == 0 and "PD-93: p910nd_running_pids()" in branch else 1)
EOF
assert_eq "$?" "0" \
	"C no real code line invokes the incompatible helper"
assert_contains "$CASE_DIR/c.out" "bare_fallback=0 documented=True" \
	"C the scan really checked code and rationale"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
