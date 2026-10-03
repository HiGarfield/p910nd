#!/bin/sh
#
# PD-05  [中高]  A read error on the printer must end the printer->network
# direction.
#
# The loop used to test READ_ERR for the network->printer direction only, so an
# EIO from the printer neither ended the job nor marked end of stream: a printer
# failing outright was handled *worse* than one that merely had nothing to say,
# because the zero-read counter ended the soft case at once while the hard error
# only stopped the job when the reply window ran out.  Measured before the fix
# with everything else held constant: hard error 5.87 s, soft 0.34 s.
#
# After the fix the hard error ends the job as soon as the already-received
# bytes have been forwarded.
#
#   A  -f /proc/self/mem   read(2) returns EIO, a hard error, every time
#   B  -f /dev/null        read(2) returns 0, a soft "no data"  (unchanged)
#   C  static: the printer direction now has a read-error exit

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-05 bidir-read-error
CASE_BIN="$BUILD_DIR/test"     # PRINTER_REPLY_WINDOW reduced to 6s
assert_file_exists "$CASE_BIN" "the instrumented variant builds"

run_device() {
	# run_device <label> <device>
	_label=$1
	_dev=$2
	daemon_start "$CASE_DIR/$_label.daemon.log" "$CASE_BIN" -d -b -i 127.0.0.1 \
		-f "$_dev" 0
	# wait_for_port opens a throwaway connection of its own, which the daemon
	# serves as a 0-byte job; the measured job is the second one.
	wait_for_port 127.0.0.1 "$TEST_PORT" 5
	_t0=$(date +%s.%N)
	"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" \
		--send-repeat 0 --half-close --expect-reply 1 \
		--read-timeout 25 --label "$_label" >"$CASE_DIR/$_label.client" 2>&1
	_t1=$(date +%s.%N)
	daemon_stop
	kill_stray
	_el=$("$PYTHON" -c "print('%.2f' % ($_t1 - $_t0))")
	eval "DUR_$_label=\$_el"
	obs_kv "$_label device" "$_dev"
	obs_kv "$_label job duration (s)" "$_el"
	obs "$_label client: $(tr '\n' '|' <"$CASE_DIR/$_label.client")"
	obs_kv "$_label hard read errors logged" \
		"$(count_lines "$CASE_DIR/$_label.daemon.log" 'Input/output error')"
	obs_kv "$_label zero-read notices" \
		"$(count_lines "$CASE_DIR/$_label.daemon.log" 'sent no data, stop reading')"
	obs "$_label daemon log:"
	sed 's/^/    /' "$CASE_DIR/$_label.daemon.log" | while read -r l; do obs "$l"; done
}

say "A: printer fails hard -- /proc/self/mem, read(2) returns EIO"
run_device a /proc/self/mem

say "B: control -- /dev/null, read(2) returns 0 (soft, no data)"
run_device b /dev/null

obs_kv "A duration" "${DUR_a:-?}"
obs_kv "B duration" "${DUR_b:-?}"

assert_ge "$(count_lines "$CASE_DIR/a.daemon.log" 'Input/output error')" "1" \
	"A the printer really does fail with a hard read error"
assert_f_lt "${DUR_a:-99}" "5" \
	"A the job now ends on the error instead of waiting out the reply window"
assert_contains "$CASE_DIR/a.daemon.log" "Input/output error" \
	"A and the log keeps the real cause"
assert_not_contains "$CASE_DIR/a.daemon.log" "sent no data, stop reading" \
	"A the hard error is no longer reported as a quiet printer"
assert_f_lt "${DUR_b:-99}" "2" \
	"B the soft case still ends almost immediately, through the zero-read counter"

# ---------------------------------------------------------------- part C ----

say "C: static -- the printer direction has a read-error exit"
obs "READ_ERR occurrences:"
grep -n 'READ_ERR' "$REPO_ROOT/p910nd.c" | sed 's/^/    /' | while read -r l; do obs "$l"; done
n_prn=$(count_lines "$REPO_ROOT/p910nd.c" 'printerToNetworkBuffer.err & READ_ERR')
n_n2p=$(count_lines "$REPO_ROOT/p910nd.c" 'networkToPrinterBuffer.err & READ_ERR')
obs_kv "tests of printerToNetworkBuffer.err & READ_ERR" "$n_prn"
obs_kv "tests of networkToPrinterBuffer.err & READ_ERR" "$n_n2p"
assert_ge "$n_prn" "1" "C the printer direction has a read-error exit"
assert_ge "$n_n2p" "2" "C the print direction still has one (bidir and unidirectional)"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
