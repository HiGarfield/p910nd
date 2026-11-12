#!/bin/sh
#
# PD-108  [medium]  A failed drain output must not leak the FIFO descriptor.
#
# tests/tools/devnode.py do_drain() opened --out OUTSIDE its try/finally and
# called out.close() before os.close(fd).  With a full disk (or /dev/full)
# BufferedWriter re-raises the ENOSPC on flush AND on close: the uncaught
# OSError killed the tool with a traceback, os.close(fd) was skipped, and
# "event=drain-end" had already been printed -- so a case watching that line
# believed the drain had succeeded.  Measured on the unfixed helper:
# event=drain-end followed by two OSError tracebacks.
#
# A: /dev/full is classified as an output error with no traceback.
# B: an injected close() failure still releases the FIFO descriptor.
# C: control -- a healthy --out still records every byte and exits 0.
# D: static -- cleanup order and error propagation.
#
# case-timeout: 150
# Two drain processes, two writers and an AST probe exceed the 45 s default
# on a loaded CI runner (PD-58).  Every wait below is bounded and can kill
# its child, so the larger budget never hides a hang.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-108 devnode-drain-cleanup

TOOL="$TOOLS_DIR/devnode.py"
D="$CASE_DIR/devices"
mkdir -p "$D" || exit 1

# A FIFO with no reader yet makes a plain O_WRONLY open block for ever,
# which wedged this case on a loaded CI runner.  Open non-blocking, retry
# ENXIO for a bounded period, and always release fd even if the reader
# disappears between open() and write().
write_chunk() {
	"$PYTHON" - "$1" <<'EOF'
import errno, os, sys, time
deadline = time.monotonic() + 8.0
while True:
    try:
        fd = os.open(sys.argv[1], os.O_WRONLY | os.O_NONBLOCK)
        break
    except OSError as e:
        if e.errno != errno.ENXIO or time.monotonic() >= deadline:
            raise
        time.sleep(0.05)
try:
    os.write(fd, b"X" * 128)
finally:
    os.close(fd)
EOF
}

# A zombie still answers kill -0, so inspect /proc before waiting: otherwise
# a drain that already exited would cost the whole polling window.  The
# bounded wait and the SIGKILL fallback keep wait(2) from wedging the case
# when a signal is ignored (the CI timeout(1) wrapper used to be relied on
# for that, and its behaviour is not portable across coreutils builds).
drain_exited() {
	[ -d "/proc/$1" ] || return 0
	[ "$(awk '{print $3}' "/proc/$1/stat" 2>/dev/null)" = "Z" ]
}

wait_for_drain() {
	# wait_for_drain <pid> <label>; sets WFD_RC to wait(2)'s status
	_wfd_pid=$1
	_n=0
	while [ "$_n" -lt 80 ]; do
		drain_exited "$_wfd_pid" && break
		sleep 0.1
		_n=$((_n + 1))
	done
	if ! drain_exited "$_wfd_pid"; then
		kill -KILL "$_wfd_pid" 2>/dev/null
		obs "$2 did not exit in 8 s; sent SIGKILL"
	fi
	wait "$_wfd_pid" 2>/dev/null
	WFD_RC=$?
}

# A background job started by a non-interactive shell inherits SIGINT set
# to SIG_IGN, so kill -INT on the drain would do nothing.  Restore the
# default Python handler in this one process: do_drain() then catches
# KeyboardInterrupt and reaches its finally block.  No coreutils timeout
# wrapper is needed, so the case does not depend on its signal behaviour.
start_drain() {
	# start_drain <fifo> <out> <logfile>; the caller reads $!
	"$PYTHON" -c '
import runpy, signal, sys
signal.signal(signal.SIGINT, signal.default_int_handler)
sys.argv = [sys.argv[1], "drain", "--fifo", sys.argv[2], "--out", sys.argv[3]]
runpy.run_path(sys.argv[0], run_name="__main__")
' "$TOOL" "$1" "$2" >"$3" 2>&1 &
}

say "A: a full output device is reported, not raised"
"$PYTHON" "$TOOL" fifo "$D/lp0" >"$CASE_DIR/mk0.out" 2>&1
start_drain "$D/lp0" /dev/full "$CASE_DIR/full.out"
FULL_PID=$!
sleep 0.4
write_chunk "$D/lp0" >"$CASE_DIR/writer-full.log" 2>&1
full_writer_rc=$?
obs "A writer exit: $full_writer_rc"
if [ "$full_writer_rc" -ne 0 ]; then
	say "A writer failed: $(tr '\n' '|' <"$CASE_DIR/writer-full.log")"
fi
assert_eq "$full_writer_rc" "0" "A the writer delivered the chunk"
wait_for_drain "$FULL_PID" "A /dev/full drain"
FULL_RC=$WFD_RC
obs "full-device exit: $FULL_RC"
obs "full-device output: $(tr '\n' '|' <"$CASE_DIR/full.out")"
assert_eq "$FULL_RC" "1" \
	"A the tool reports the output failure with a nonzero status"
assert_contains "$CASE_DIR/full.out" "event=write-error" \
	"A the write failure is classified"
assert_contains "$CASE_DIR/full.out" "event=out-close-error" \
	"A and so is the deferred close failure"
assert_contains "$CASE_DIR/full.out" "event=drain-end" \
	"A the end event is still emitted"
assert_not_contains "$CASE_DIR/full.out" "Traceback" \
	"A with no uncaught exception"

say "B: os.close(fd) runs even when out.close() raises"
"$PYTHON" - "$TOOL" >"$CASE_DIR/b.out" 2>&1 <<'EOF'
import importlib.util, os, sys, types
spec = importlib.util.spec_from_file_location("devnode_mod", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
r, w = os.pipe()
os.write(w, b"X" * 8)
m.os.open = lambda *a, **k: r
seq = iter([b"X" * 8])
def fake_read(fd, n):
    try:
        return next(seq)
    except StopIteration:
        raise KeyboardInterrupt()
m.os.read = fake_read
closed = []
m.os.close = lambda fd: closed.append(fd)
class BadOut:
    def write(self, b):
        raise OSError(28, "No space left on device")
    def flush(self):
        raise OSError(28, "No space left on device")
    def close(self):
        raise OSError(28, "No space left on device")
m.open = lambda *a, **k: BadOut()
rc = m.do_drain(types.SimpleNamespace(
    fifo="ignored", out="/dev/full", verbose=False))
print("rc=%d closed=%s fifo_fd=%d released=%s" %
      (rc, closed, r, r in closed))
sys.exit(0 if rc == 1 and r in closed else 1)
EOF
assert_eq "$?" "0" \
	"B the FIFO descriptor is released despite the close failure"
assert_str_contains "$(cat "$CASE_DIR/b.out")" "released=True" \
	"B the probe really checked that descriptor"

say "C: control -- a healthy output still works"
"$PYTHON" "$TOOL" fifo "$D/lp1" >"$CASE_DIR/mk1.out" 2>&1
start_drain "$D/lp1" "$D/stream.bin" "$CASE_DIR/ok.out"
OK_PID=$!
sleep 0.4
write_chunk "$D/lp1" >"$CASE_DIR/writer-ok.log" 2>&1
ok_writer_rc=$?
obs "C writer exit: $ok_writer_rc"
if [ "$ok_writer_rc" -ne 0 ]; then
	say "C writer failed: $(tr '\n' '|' <"$CASE_DIR/writer-ok.log")"
fi
assert_eq "$ok_writer_rc" "0" "C the writer delivered the chunk"
# Wait until the drain has actually flushed the chunk before interrupting
# it; a fixed sleep made the byte-count assertion timing dependent.
_n=0
while [ "$_n" -lt 40 ]; do
	if [ -f "$D/stream.bin" ]; then
		_size=$(wc -c <"$D/stream.bin" | tr -d ' ')
	else
		_size=0
	fi
	[ "$_size" = "128" ] && break
	sleep 0.1
	_n=$((_n + 1))
done
kill -INT "$OK_PID" 2>/dev/null
wait_for_drain "$OK_PID" "C healthy drain"
OK_RC=$WFD_RC
obs "healthy exit: $OK_RC output: $(tr '\n' '|' <"$CASE_DIR/ok.out")"
assert_eq "$OK_RC" "0" \
	"C a healthy drain exits 0"
assert_eq "$(wc -c <"$D/stream.bin" | tr -d ' ')" "128" \
	"C and all 128 bytes reached the output file"
assert_contains "$CASE_DIR/ok.out" "event=drain-end" \
	"C the end event is present"
assert_not_contains "$CASE_DIR/ok.out" "write-error" \
	"C with no spurious error"
assert_not_contains "$CASE_DIR/ok.out" "Traceback" \
	"C and no traceback"

say "D: static -- cleanup order and status propagation"
"$PYTHON" - "$TOOL" >"$CASE_DIR/d.out" 2>&1 <<'EOF'
import ast, sys
tree = ast.parse(open(sys.argv[1], encoding="utf-8").read())
fn = next(n for n in tree.body
          if isinstance(n, ast.FunctionDef) and n.name == "do_drain")
src = ast.get_source_segment(open(sys.argv[1], encoding="utf-8").read(), fn)
open_inside = "out = open(args.out" in src
classified = "event=write-error" in src and "event=out-close-error" in src
returns_error = "return 1 if error is not None else 0" in src
close_last = src.rfind("os.close(fd)") > src.rfind("out.close()")
documented = "PD-108" in src
print("open_inside=%s classified=%s returns_error=%s close_last=%s documented=%s"
      % (open_inside, classified, returns_error, close_last, documented))
sys.exit(0 if all((open_inside, classified, returns_error,
                   close_last, documented)) else 1)
EOF
assert_eq "$?" "0" \
	"D output errors are classified and fd is closed last"
assert_str_contains "$(cat "$CASE_DIR/d.out")" \
	"open_inside=True classified=True returns_error=True close_last=True documented=True" \
	"D every property was checked"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
