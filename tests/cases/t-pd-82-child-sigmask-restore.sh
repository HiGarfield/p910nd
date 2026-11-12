#!/bin/sh
#
# PD-82  [medium]  B-4's child signal-mask restore has no regression guard.
#
# DEFECT_REPORT.md B-4 says the job child used to inherit a blocked SIGCHLD
# and never restore it, was fixed in server(), and lists NO test case.  No
# case mentions SIG_SETMASK at all, so deleting the restore keeps the whole
# suite green.
#
# A: behaviour -- a live job child's /proc/PID/status SigBlk does not have
#    the SIGCHLD bit set while the parent holds SIGCHLD blocked across fork.
# B: static + self-mutation -- the restore really sits in the pid==0 branch,
#    and removing it makes the check fail (so the guard is not vacuous).
# C: the report names this case instead of claiming none exists.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-82 child-sigmask-restore

SRC="$REPO_ROOT/p910nd.c"
REPORT="$TESTS_DIR/DEFECT_REPORT.md"
CHECK="$CASE_DIR/check.py"
CASE_BIN="$BUILD_DIR/test"
DEV="$CASE_DIR/lp0"
: >"$DEV"

cat >"$CHECK" <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
start = s.find("if (pid == 0) {")
end = s.find("handle_connection(fd, lpnumber);", start)
body = s[start:end] if start >= 0 and end > start else ""
restore = len(re.findall(r"sigprocmask\(SIG_SETMASK, &chld_omask, NULL\);", body))
block = s.find("sigprocmask(SIG_BLOCK, &chld_mask, &chld_omask);")
print("branch_found=%d restore=%d block_before_fork=%d" %
      (len(body) > 0, restore, block >= 0 and block < s.find("pid = fork();")))
sys.exit(0 if body and restore == 1 and 0 <= block < s.find("pid = fork();")
         else 1)
EOF

say "A: a real job child does not have SIGCHLD blocked"
daemon_start "$CASE_DIR/daemon.log" "$CASE_BIN" -d -i 127.0.0.1 \
	-f "$DEV" 0
if [ -z "$DAEMON_PID" ]; then
	obs "daemon failed: $(tr '\n' '|' <"$CASE_DIR/daemon.log")"
	assert_true 1 "A the daemon starts"
else
	wait_for_port 127.0.0.1 "$TEST_PORT" 5
	assert_eq "$?" "0" "A the daemon listens"
	"$PYTHON" - "$TEST_PORT" >"$CASE_DIR/client.out" 2>&1 <<'EOF' &
import socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 2)
time.sleep(5)
EOF
	CLIENT_PID=$!
	"$PYTHON" - "$DAEMON_PID" >"$CASE_DIR/probe.out" 2>&1 <<'EOF'
import os, sys, time
daemon = int(sys.argv[1])
# Linux SIGCHLD is signal 17 -> bit 16 in SigBlk.
sigchld = 1 << 16
deadline = time.monotonic() + 3.0
while time.monotonic() < deadline:
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            text = open("/proc/%s/status" % name, encoding="utf-8").read()
        except OSError:
            continue
        ppid, sigblk = None, None
        for line in text.splitlines():
            if line.startswith("PPid:"):
                ppid = int(line.split()[1])
            elif line.startswith("SigBlk:"):
                sigblk = int(line.split()[1], 16)
        if ppid == daemon and sigblk is not None:
            blocked = bool(sigblk & sigchld)
            print("child_pid=%s sigblk=0x%x sigchld_blocked=%d"
                  % (name, sigblk, blocked))
            sys.exit(1 if blocked else 0)
    time.sleep(0.05)
print("no job child observed")
sys.exit(1)
EOF
	probe_rc=$?
	obs "probe: $(tr '\n' '|' <"$CASE_DIR/probe.out")"
	assert_eq "$probe_rc" "0" \
		"A the job child runs with SIGCHLD unblocked"
	assert_contains "$CASE_DIR/probe.out" "sigchld_blocked=0" \
		"A the SigBlk value was really read"
	kill -TERM "$CLIENT_PID" 2>/dev/null
	wait "$CLIENT_PID" 2>/dev/null
	daemon_stop
fi

say "B: static check plus self-mutation"
"$PYTHON" "$CHECK" "$SRC" >"$CASE_DIR/a.out" 2>&1
a_rc=$?
obs "source scan: $(cat "$CASE_DIR/a.out")"
assert_eq "$a_rc" "0" "B the restore is inside the child branch"
"$PYTHON" - "$SRC" "$CASE_DIR/mutated.c" <<'EOF'
import sys
s = open(sys.argv[1], encoding="utf-8").read()
start = s.find("if (pid == 0) {")
end = s.find("handle_connection(fd, lpnumber);", start)
assert start >= 0 and end > start
body = s[start:end]
n = "sigprocmask(SIG_SETMASK, &chld_omask, NULL);"
assert body.count(n) == 1, "the child-branch restore is missing"
# Other SIG_SETMASK calls exist outside the child branch; only this one
# matters, so mutate the branch slice rather than the first global hit.
open(sys.argv[2], "w", encoding="utf-8").write(
    s[:start] + body.replace(n, ";", 1) + s[end:])
EOF
"$PYTHON" "$CHECK" "$CASE_DIR/mutated.c" >"$CASE_DIR/b.out" 2>&1
b_rc=$?
obs "mutated scan: $(cat "$CASE_DIR/b.out")"
assert_eq "$b_rc" "1" "B removing the restore makes the check fail"

say "C: the report records the regression case"
assert_not_contains "$REPORT" '| B-4 | 子进程 fork 后恢复信号掩码 | — |' \
	"C B-4 no longer claims to have no case"
assert_contains "$REPORT" "t-pd-82" \
	"C and names this case"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
