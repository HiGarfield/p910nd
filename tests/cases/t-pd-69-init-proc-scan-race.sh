#!/bin/sh
#
# PD-69  [low]  /proc scans must not print spurious diagnostics.
#
# p910nd_running_pids() and the stop branch read /proc/PID/cmdline after
# globbing /proc/[0-9]*/cmdline.  A process that exits in that window makes
# the shell itself fail the input redirection, and the message goes to
# stderr BEFORE tr runs -- so tr's own 2>/dev/null does not suppress it.
# Observed in PD-68's status output:
#   "...: line 276: /proc/3347229/cmdline: No such file or directory"
# A status/stop command must not report an error for a normal process exit.
#
# The case extracts the real helper verbatim, points its /proc paths at a
# fake tree containing a dangling cmdline, and requires empty stderr while
# a valid entry is still discovered.
#
# A: no stderr noise, valid pid still found, unrelated pid ignored.
# B: static -- all four reads use the grouped redirection.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-69 init-proc-scan-race

INIT="$REPO_ROOT/aux/p910nd.init"
EMPTY="$CASE_DIR/empty-piddir"
FAKE="$CASE_DIR/fake-proc"
mkdir -p "$EMPTY" "$FAKE/123" "$FAKE/456" "$FAKE/789"

sed -n '/^p910nd_running_pids() {/,/^}/p' "$INIT" >"$CASE_DIR/helper.sh"
assert_contains "$CASE_DIR/helper.sh" 'for p910nd_proc in' \
	"the real /proc scan helper was extracted"
assert_contains "$CASE_DIR/helper.sh" 'printf' \
	"and the whole function body is present"

# A dangling cmdline is exactly what a vanished process leaves behind at
# glob time; a real one proves the scan still works.
ln -s "$CASE_DIR/does-not-exist" "$FAKE/123/cmdline"
printf 'p9100d\000' >"$FAKE/456/cmdline"
printf 'sleep\000' >"$FAKE/789/cmdline"
sed "s|/proc/|$FAKE/|g" "$CASE_DIR/helper.sh" >"$CASE_DIR/helper-fake.sh"

say "A: a disappearing process produces no diagnostic"
( . "$CASE_DIR/helper-fake.sh"
  P910ND_PIDDIR="$EMPTY" p910nd_running_pids
) >"$CASE_DIR/scan.out" 2>"$CASE_DIR/scan.err"
scan_rc=$?
obs "scan stdout: $(tr '\n' '|' <"$CASE_DIR/scan.out")"
obs "scan stderr: $(tr '\n' '|' <"$CASE_DIR/scan.err")"
obs_kv "scan exit" "$scan_rc"
assert_eq "$scan_rc" "0" "A the helper completes"
assert_eq "$(wc -c <"$CASE_DIR/scan.err" | tr -d ' ')" "0" \
	"A stderr is empty despite the dangling cmdline"
assert_not_contains "$CASE_DIR/scan.err" "No such file" \
	"A no fake error about a process that simply exited"
assert_contains "$CASE_DIR/scan.out" "456" \
	"A the valid p9100d entry is still discovered"
assert_not_contains "$CASE_DIR/scan.out" "789" \
	"A an unrelated process is still ignored"

say "B: static -- every cmdline read redirects the whole group"
assert_eq "$(count_lines "$INIT" '} 2>/dev/null | sed -n 1p')" "4" \
	"B all four cmdline reads use the grouped redirection"
assert_contains "$INIT" "PD-69: a process can disappear" \
	"B the reason is documented"
assert_not_contains "$INIT" 'cmdline" 2>/dev/null | sed' \
	"B no old unsuppressed form remains"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
