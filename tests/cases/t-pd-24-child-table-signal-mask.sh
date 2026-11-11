#!/bin/sh
#
# PD-24  [medium]  The child pid table must not race with SIGCHLD.
#
# sigchld_handler() clears child_pids[i] asynchronously.  The shutdown path
# used to walk the same array with SIGCHLD unblocked, so the handler could
# zero an entry while the parent was reading it: a plain pid_t array is
# neither volatile sig_atomic_t nor protected by a signal mask.  POSIX only
# makes such a shared object safe when the reader blocks the signal.
#
# The parent now reads the table only through signal_job_children(), which
# holds a SIGCHLD block for the whole walk.  The check below verifies that
# invariant in the source, because the race itself cannot be reproduced on
# demand.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-24 child-table-signal-mask

"$PYTHON" - "$REPO_ROOT/p910nd.c" >"$CASE_DIR/check.out" 2>&1 <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()


def body(name):
    m = re.search(r"static void %s\([^)]*\) \{(.*?)\n\}" % re.escape(name),
                  s, re.S)
    return m.group(1) if m else None


reap = body("reap_children_and_exit")
helper = body("signal_job_children")
print("reap-table-access %s" %
      ("DEFECT" if reap is None or "child_pids[" in reap else "OK"))
print("helper-blocks-sigchld %s" %
      ("OK" if helper and "sigprocmask(SIG_BLOCK" in helper and
       "child_pids[" in helper else "DEFECT"))
print("helper-used %s" %
      ("OK" if reap and reap.count("signal_job_children(SIG") >= 2
       else "DEFECT"))
EOF
obs "checks:"
sed 's/^/    /' "$CASE_DIR/check.out" | while read -r l; do obs "$l"; done
assert_contains "$CASE_DIR/check.out" "reap-table-access OK" \
	"shutdown never indexes the child table with SIGCHLD unblocked"
assert_contains "$CASE_DIR/check.out" "helper-blocks-sigchld OK" \
	"the table reader blocks SIGCHLD for the whole walk"
assert_contains "$CASE_DIR/check.out" "helper-used OK" \
	"both forced-termination passes go through the protected helper"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
