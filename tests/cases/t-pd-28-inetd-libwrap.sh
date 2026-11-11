#!/bin/sh
#
# PD-28  [medium]  USE_LIBWRAP must also guard (x)inetd connections.
#
# server() calls hosts_ctl() for every accepted socket, but one_job() --
# the one-shot process that (x)inetd hands fd 0 to -- never did.  A build
# with USE_LIBWRAP therefore had no tcpwrappers access control at all in
# one-shot mode, even though the man page describes the check as a property
# of the binary.
#
# A preloaded hosts_ctl() records every check and allows the connection.
# The case asserts the check happened and that the job still completes.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-28 inetd-libwrap

INETD_PORT=${INETD_PORT:-19199}
LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR"
CASE_BIN=$(build_variant libwrap "-DLOCKFILE_DIR=\"$LOCKDIR\"" \
	-DUSE_LIBWRAP -lwrap)
assert_file_exists "$CASE_BIN" "the libwrap variant builds"

SHIM="$CASE_DIR/hosts_ctl.so"
"$CC_BIN" -shared -fPIC -O1 -o "$SHIM" \
	"$TOOLS_DIR/hosts_ctl_preload.c" >"$CASE_DIR/shim.build.log" 2>&1
shim_rc=$?
obs "shim build:"
sed 's/^/    /' "$CASE_DIR/shim.build.log" | while read -r l; do obs "$l"; done
assert_eq "$shim_rc" "0" "the hosts_ctl shim builds"

DEV="$CASE_DIR/lp0"
: >"$DEV"
LOG="$CASE_DIR/hosts.log"
rm -f "$LOG"

env LD_PRELOAD="$SHIM" P910ND_HOSTS_CTL_LOG="$LOG" \
	"$PYTHON" "$TOOLS_DIR/inetd_sim.py" --port "$INETD_PORT" \
	--target "$CASE_BIN" --send-repeat 512 --half-close \
	--read-timeout 12 --device "$DEV" -- -i 127.0.0.1 -f "$DEV" 0 \
	>"$CASE_DIR/inetd.out" 2>&1
obs "inetd sim:"
sed 's/^/    /' "$CASE_DIR/inetd.out" | while read -r l; do obs "$l"; done

checks=$(count_lines "$LOG" 'p910nd')
obs_kv "hosts_ctl calls with the daemon name" "$checks"
assert_ge "${checks:-0}" "1" \
	"one_job() consults hosts_ctl() before serving the connection"
assert_contains "$LOG" "127.0.0.1" \
	"the real client address is passed to the access check"
assert_contains "$CASE_DIR/inetd.out" "device_bytes=512" \
	"the allowed job still reaches the printer in full"
assert_not_contains "$CASE_DIR/inetd.out" "how=rst" \
	"an allowed connection is not reported as a failed job"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
