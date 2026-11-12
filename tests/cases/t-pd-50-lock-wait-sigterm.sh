#!/bin/sh
#
# PD-50  [high]  A pending SIGTERM must abort a blocking lock wait.
#
# take_lock() only looked at got_term after fcntl(F_SETLKW) returned EINTR.
# If SIGTERM arrived BEFORE the call (a shutdown while an inetd job was
# starting), the flag was already set but no further signal interrupted the
# kernel wait: the job sat on the instance lock until the holder released it
# -- forever if that holder is wedged.  byte 0 is taken with wait_seconds < 0
# by one_job(), so this is the normal inetd shutdown path.
#
# The probe has a child hold byte 0, sets got_term in the parent, and calls
# take_lock(fd, 0, -1).  SIGALRM bounds the wait to three seconds.
# A control run without got_term must still block, proving the lock really
# was held and the result is not a vacuous pass.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-50 lock-wait-sigterm

PROBE="$CASE_DIR/take_lock_probe"
"$CC_BIN" -std=gnu89 -O1 -o "$PROBE" \
	"$TOOLS_DIR/take_lock_probe.c" -I"$REPO_ROOT" \
	>"$CASE_DIR/probe.build.log" 2>&1
build_rc=$?
obs "probe build:"
sed 's/^/    /' "$CASE_DIR/probe.build.log" | while read -r l; do obs "$l"; done
assert_eq "$build_rc" "0" "the lock-wait probe builds"

say "A: got_term already set before the unbounded wait"
timeout 12 "$PROBE" "$CASE_DIR/lock-a" >"$CASE_DIR/a.out" 2>&1
a_rc=$?
obs "A output: $(tr '\n' '|' <"$CASE_DIR/a.out")"
obs_kv "A exit" "$a_rc"
assert_eq "$a_rc" "0" \
	"A take_lock() returns at once instead of blocking on the held lock"
assert_contains "$CASE_DIR/a.out" "take_lock rc=0 held=1 got_term=1" \
	"A the lock really was held and the flag really was set"

say "B: control -- without got_term the same wait must still block"
timeout 12 "$PROBE" "$CASE_DIR/lock-b" no-term >"$CASE_DIR/b.out" 2>&1
b_rc=$?
obs "B output: $(tr '\n' '|' <"$CASE_DIR/b.out")"
obs_kv "B exit" "$b_rc"
assert_eq "$b_rc" "2" \
	"B SIGALRM still fires when there is no terminate flag"

say "C: static -- the flag is checked before the kernel wait"
"$PYTHON" - "$REPO_ROOT/p910nd.c" >"$CASE_DIR/scan.out" 2>&1 <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r"static int take_lock\(.*?\n\}", s, re.S)
body = m.group(0) if m else ""
flag = body.find("got_term")
wait = body.find("F_SETLKW")
print("body_len=%d flag_at=%d F_SETLKW_at=%d" % (len(body), flag, wait))
sys.exit(0 if 0 <= flag < wait else 1)
EOF
scan_rc=$?
obs "C scan: $(tr '\n' '|' <"$CASE_DIR/scan.out")"
obs_kv "C scan exit" "$scan_rc"
assert_eq "$scan_rc" "0" \
	"C take_lock() checks got_term before entering F_SETLKW"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
