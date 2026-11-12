#!/bin/sh
#
# PD-62  [low]  The repl device log must not use wall-clock timestamps.
#
# devnode.py's pty deadlines and drain durations were moved to
# time.monotonic() (PD-51), but do_repl() still printed an absolute
# time.time() epoch value as "t=".  A backwards clock step (NTP, VM
# resume, the fake clock shim PD-07 loads) makes those stamps jump back
# an hour in the middle of a run and makes them incomparable with the
# drain log of the same FIFO.
#
# A: with time.time() stepped back one hour, three "t=" stamps must be
#    small, non-negative and non-decreasing elapsed values.
# B: static -- no absolute wall-clock stamp remains, and the units are
#    documented.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-62 devnode-repl-clock

DEV="$TOOLS_DIR/devnode.py"
FIFO="$CASE_DIR/lp0"

cat >"$CASE_DIR/sitecustomize.py" <<'EOF'
import time
_real = time.time
_n = [0]
def shifted():
    _n[0] += 1
    return _real() - (3600 if _n[0] >= 3 else 0)
time.time = shifted
EOF

"$PYTHON" "$DEV" fifo "$FIFO" >"$CASE_DIR/node.out" 2>&1
assert_eq "$?" "0" "the FIFO test device is created"

# O_WRONLY open on a FIFO needs a reader; drain opens it O_RDWR.
"$PYTHON" "$DEV" drain --fifo "$FIFO" >"$CASE_DIR/drain.out" 2>&1 &
DRAIN_PID=$!
sleep 0.3

say "A: three repl writes under a backwards wall-clock step"
env PYTHONPATH="$CASE_DIR" "$PYTHON" "$DEV" repl --fifo "$FIFO" \
	--data CHUNK --repeat 3 --delay 0.1 >"$CASE_DIR/repl.out" 2>&1
repl_rc=$?
obs "repl output: $(tr '\n' '|' <"$CASE_DIR/repl.out")"
obs_kv "repl exit" "$repl_rc"
assert_eq "$repl_rc" "0" "A the repl helper completes"

"$PYTHON" - "$CASE_DIR/repl.out" >"$CASE_DIR/scan.out" 2>&1 <<'EOF'
import re, sys
text = open(sys.argv[1], encoding="utf-8").read()
vals = [float(v) for v in re.findall(r"event=write t=([0-9.]+)", text)]
print("stamps=%d values=%s" % (len(vals), ",".join("%.3f" % v for v in vals)))
ok = (len(vals) == 3 and all(0 <= v < 2.0 for v in vals)
      and vals == sorted(vals))
print("ok=%s" % ok)
sys.exit(0 if ok else 1)
EOF
scan_rc=$?
obs "scan: $(tr '\n' '|' <"$CASE_DIR/scan.out")"
obs_kv "scan exit" "$scan_rc"
assert_eq "$scan_rc" "0" \
	"A the stamps are small elapsed values in order, not epoch times"
assert_eq "$(count_lines "$CASE_DIR/repl.out" 'event=write t=')" "3" \
	"A three writes really were logged"

kill -TERM "$DRAIN_PID" 2>/dev/null
wait "$DRAIN_PID" 2>/dev/null

say "B: static -- absolute wall-clock stamps are gone"
assert_eq "$(count_lines "$DEV" 'time.time(), i')" "0" \
	"B do_repl no longer prints time.time()"
assert_contains "$DEV" "t0 = time.monotonic()" \
	"B repl uses a monotonic origin"
assert_contains "$DEV" "elapsed monotonic seconds" \
	"B the log units are documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
