#!/bin/sh
#
# PD-64  [medium]  inetd_sim.py must not report a failed job as success.
#
# The helper always returned 0 from main(), even when it printed
# "RESULT how=rst" or "RESULT how=timeout".  A case that checks only $?
# would pass on a job that was refused or never confirmed -- exactly the
# false-green class fixed for client.py in PD-48.  Measured before the fix:
# "RESULT how=rst received=0 exit=0" with process exit 0.
#
# A: RST -> exit 1.      B: clean FIN -> exit 0.
# C: timeout -> exit 1.  D: complete reply -> exit 0.
# E: static -- the failure set matches client.py and is documented.
#
# Dependency: PD-47's probe deliberately times out, so its two exit-code
# expectations were updated to 1 in the same commit (test-only consequence,
# not a separate defect).

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-64 inetd-sim-exit-status

SIM="$TOOLS_DIR/inetd_sim.py"

cat >"$CASE_DIR/rst.py" <<'EOF'
#!/usr/bin/env python3
import os, socket, struct
s = socket.socket(fileno=0)
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
s.close()
os._exit(0)
EOF
cat >"$CASE_DIR/fin.py" <<'EOF'
#!/usr/bin/env python3
import os
os._exit(0)
EOF
cat >"$CASE_DIR/complete.py" <<'EOF'
#!/usr/bin/env python3
import os, time
os.write(1, b"12345678")
time.sleep(1)
EOF
cat >"$CASE_DIR/hold.py" <<'EOF'
#!/usr/bin/env python3
import time
time.sleep(2)
EOF
chmod +x "$CASE_DIR"/*.py

free_port() {
	"$PYTHON" -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()'
}

run_sim() {
	_label=$1
	_target=$2
	_expect=$3
	_readtimeout=$4
	_port=$(free_port)
	timeout 15 "$PYTHON" "$SIM" --port "$_port" \
		--target "$CASE_DIR/$_target" --expect-reply "$_expect" \
		--read-timeout "$_readtimeout" >"$CASE_DIR/$_label.out" 2>&1
	SIM_RC=$?
	obs "$_label output: $(tr '\n' '|' <"$CASE_DIR/$_label.out")"
	obs_kv "$_label exit" "$SIM_RC"
}

say "A: RST from the service"
run_sim rst rst.py 65536 2
assert_contains "$CASE_DIR/rst.out" "RESULT how=rst" \
	"A the RST is reported"
assert_eq "$SIM_RC" "1" "A and the process exits non-zero"

say "B: clean FIN"
run_sim fin fin.py 65536 2
assert_contains "$CASE_DIR/fin.out" "RESULT how=fin" \
	"B the clean FIN is reported"
assert_eq "$SIM_RC" "0" "B and still exits zero"

say "C: read timeout"
run_sim timeout hold.py 1 1
assert_contains "$CASE_DIR/timeout.out" "RESULT how=timeout" \
	"C the timeout is reported"
assert_eq "$SIM_RC" "1" "C and the unconfirmed job exits non-zero"

say "D: complete reply"
run_sim complete complete.py 8 1
assert_contains "$CASE_DIR/complete.out" "RESULT how=complete" \
	"D a complete reply is reported"
assert_eq "$SIM_RC" "0" "D and remains a success"

say "E: static -- classification matches client.py"
assert_contains "$SIM" '("rst", "connect-error", "timeout")' \
	"E the same failure tuple as client.py is used"
assert_contains "$SIM" 'how.startswith("oserror:")' \
	"E OS errors are included"
assert_contains "$SIM" "non-zero whenever the job was NOT confirmed" \
	"E the exit-status contract is documented"
assert_contains "$TOOLS_DIR/client.py" '("rst", "connect-error", "timeout")' \
	"E client.py still uses the same classification"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
