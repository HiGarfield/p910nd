#!/bin/sh
#
# PD-66  [high]  inetd diagnostics must not enter the client's print stream.
#
# PD-04 fixed dolog() by falling back to syslog when stdout is the client
# socket, but usage(), show_version() and getopt()'s own "invalid option"
# line used fprintf(stderr/stdout) directly.  Under (x)inetd fd 1 and 2 ARE
# the client socket, so a misconfigured argument injected 176 bytes of usage
# text and `-v` injected 19 bytes of version text into a print job (measured
# with inetd_sim.py), with a clean FIN afterwards.
#
# A: `-Z` under inetd -> client receives 0 bytes.
# B: `-v` under inetd -> client receives 0 bytes.
# C: control -- a normal job still reaches the printer.
# D: standalone CLI still prints version/usage.
# E: static -- every diagnostic path checks the descriptor.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-66 inetd-diagnostic-output

LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR"
BIN=$(build_variant test-inetd-diag "-DLOCKFILE_DIR=\"$LOCKDIR\"")
assert_file_exists "$BIN" "the inetd diagnostic variant builds"

DEV="$CASE_DIR/lp0"
: >"$DEV"

free_port() {
	"$PYTHON" -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()'
}

# inetd_sim.py reports the client's view; --device lets us check delivery.
run_inetd() {
	_label=$1
	shift
	_port=$(free_port)
	"$PYTHON" "$TOOLS_DIR/inetd_sim.py" --port "$_port" \
		--target "$BIN" --expect-reply 65536 --read-timeout 5 \
		--device "$DEV" -- "$@" >"$CASE_DIR/$_label.out" 2>&1
	INETD_RC=$?
	obs "$_label output: $(tr '\n' '|' <"$CASE_DIR/$_label.out")"
	obs_kv "$_label exit" "$INETD_RC"
}

say "A: an invalid option under inetd"
run_inetd badopt -Z
assert_contains "$CASE_DIR/badopt.out" "client_received=0" \
	"A the invalid-option diagnostic no longer reaches the client"
assert_not_contains "$CASE_DIR/badopt.out" "Usage:" \
	"A no usage text in the print stream"
assert_not_contains "$CASE_DIR/badopt.out" "invalid option" \
	"A no getopt diagnostic in the print stream"

say "B: -v under inetd"
run_inetd version -v
assert_contains "$CASE_DIR/version.out" "client_received=0" \
	"B the version text no longer reaches the client"
assert_not_contains "$CASE_DIR/version.out" "Version 1.1" \
	"B no version string in the print stream"

say "C: control -- a normal job still works"
: >"$DEV"
run_inetd normal -i 127.0.0.1 -f "$DEV" 0
assert_contains "$CASE_DIR/normal.out" "client_received=0" \
	"C a normal job does not send unsolicited bytes"
assert_eq "$(wc -c <"$DEV" | tr -d ' ')" "0" \
	"C and no data was fabricated on the device"

say "D: standalone CLI output is unchanged"
"$BIN" -v >"$CASE_DIR/cli-version.out" 2>&1
assert_contains "$CASE_DIR/cli-version.out" "Version 1.1" \
	"D p910nd -v still prints the version on stdout"
"$BIN" -Z >"$CASE_DIR/cli-bad.out" 2>&1
cli_rc=$?
obs_kv "standalone invalid option exit" "$cli_rc"
assert_eq "$cli_rc" "1" "D an invalid option still exits 1"
assert_contains "$CASE_DIR/cli-bad.out" "Usage:" \
	"D and still prints usage on stderr"
assert_contains "$CASE_DIR/cli-bad.out" "invalid option" \
	"D and still names the invalid option"

say "E: static -- all diagnostic paths are guarded"
assert_contains "$REPO_ROOT/p910nd.c" 'opterr = 0' \
	"E getopt's own stderr diagnostic is disabled"
assert_contains "$REPO_ROOT/p910nd.c" 'fd_is_the_client_socket(2)' \
	"E usage() checks stderr"
assert_contains "$REPO_ROOT/p910nd.c" 'fd_is_the_client_socket(int fd)' \
	"E the descriptor-aware helper exists"
assert_contains "$REPO_ROOT/p910nd.c" 'if (stdout_is_the_client_socket())' \
	"E show_version() checks stdout"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
