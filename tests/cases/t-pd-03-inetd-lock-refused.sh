#!/bin/sh
#
# PD-03  [中]  A refused job must be closed with RST on every path.
#
# one_job() used to answer a failed get_lock() with a bare `return`, so fd 0 was
# closed by process exit and the client saw a clean FIN -- which reads as
# "printed" to a client that has no application-level acknowledgement.  It now
# goes through close_connection(0, 1) like handle_connection() always did.
#
# The lock failure is produced with a lock directory that cannot exist
# (/proc/...), which is the one condition get_lock() still refuses.
#
# A1 the client already streamed a job  -> RST
# A2 the client has sent nothing         -> RST   (this is what the fix changes)
# B  the printer device is missing       -> RST   (the intended path, unchanged)

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-03 inetd-lock-refused
DENY_DIR=/proc/p910nd-deny
CASE_BIN=$(build_variant test-lockdir-deny "-DLOCKFILE_DIR=\"$DENY_DIR\"")
assert_file_exists "$CASE_BIN" "the uncreatable-lock variant builds"
DEV="$CASE_DIR/lp0"
: >"$DEV"

run_inetd() {
	# run_inetd <label> <send-repeat> [extra args...]
	_label=$1
	_reps=$2
	shift 2
	"$PYTHON" "$TOOLS_DIR/inetd_sim.py" --port 19199 --target "$CASE_BIN" \
		--send-repeat "$_reps" --read-timeout 6 --device "$DEV" \
		"$@" >"$CASE_DIR/$_label.out" 2>&1
	sed 's/^/    /' "$CASE_DIR/$_label.out" | while read -r l; do obs "$l"; done
	eval "HOW_$_label=\$(sed -n 's/^RESULT how=\\([a-z-]*\\).*/\\1/p' "$CASE_DIR/$_label.out")"
}

# ---- A1: a client that already streamed a job ---------------------------
say "A1: instance lock unavailable, client sent 64 bytes then half-closed"
run_inetd a1 64 --half-close -- -i 127.0.0.1 0
obs_kv "A1 how the client saw the end" "${HOW_a1:-?}"
assert_eq "${HOW_a1:-x}" "rst" "A1 the refused job is reported as a failure"
assert_eq "$(wc -c <"$DEV")" "0" "A1 the printer never received the job"

# ---- A2: a client that has sent nothing ---------------------------------
say "A2: same refusal, but the client has sent nothing yet"
run_inetd a2 0 -- -i 127.0.0.1 0
obs_kv "A2 how the client saw the end" "${HOW_a2:-?}"
assert_eq "${HOW_a2:-x}" "rst" \
	"A2 a client that has not sent anything is told the job failed, not that it finished"

# ---- B: the intended path, for contrast ---------------------------------
say "B: control, printer device missing -> close_connection(fd,1)"
fastfail=$(build_variant test-lockdir-fastfail \
	"-DLOCKFILE_DIR=\"$LOCK_DIR_OK\"" -DOPEN_PRINTER_PERMANENT_WAIT=2 \
	-DOPEN_PRINTER_RETRY_INTERVAL=1)
assert_file_exists "$fastfail" "the control variant builds"
"$PYTHON" "$TOOLS_DIR/inetd_sim.py" --port 19199 --target "$fastfail" \
	--send-repeat 0 --read-timeout 12 --device "$DEV" \
	-- -i 127.0.0.1 -f "$CASE_DIR/does-not-exist" 0 >"$CASE_DIR/b.out" 2>&1
sed 's/^/    /' "$CASE_DIR/b.out" | while read -r l; do obs "$l"; done
b_how=$(sed -n 's/^RESULT how=\([a-z-]*\).*/\1/p' "$CASE_DIR/b.out")
obs_kv "B how the client saw the end" "$b_how"
assert_eq "$b_how" "rst" "B the printer-open refusal still reports ECONNRESET"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
