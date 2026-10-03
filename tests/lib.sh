#!/bin/sh
#
# Common helpers for the p910nd defect verification cases.
#
# Sourced by every tests/cases/*.sh.  Provides:
#   - repository/build directory variables
#   - logging and observation recording
#   - variant building (delegated to variants.sh)
#   - daemon start/stop with process-group tracking
#   - per-case scratch directory management
#
# The p910nd daemon rewrites its own argv[0] (see main() in p910nd.c), so
# cleaning up by program name is unreliable.  Every daemon is therefore started
# with setsid(1) and the whole process group is signalled on teardown.

# The case scripts export REPO_ROOT before sourcing this file, because $0 is the
# case script and not this one.
: "${REPO_ROOT:?REPO_ROOT must be set by the caller}"
TESTS_DIR="$REPO_ROOT/tests"
TOOLS_DIR="$TESTS_DIR/tools"
CASES_DIR="$TESTS_DIR/cases"
BUILD_DIR="$REPO_ROOT/build/tests"
OBS_DIR="$BUILD_DIR/observations"
CC_BIN=${CC_BIN:-gcc}
PYTHON=${PYTHON:-python3}

# Every case gets a hard wall clock limit so a hung defect cannot wedge the run.
CASE_TIMEOUT=${CASE_TIMEOUT:-45}

# run-all.sh exports these; give them defaults so a case can also be run on its
# own with `sh tests/cases/t-pd-NN-....sh`.
TEST_PORT=${TEST_PORT:-19100}
LOCK_DIR_OK=${LOCK_DIR_OK:-$BUILD_DIR/lock-ok}
LOCK_DIR_MISSING=${LOCK_DIR_MISSING:-$BUILD_DIR/lock-absent}
FAKETIME_SO=${FAKETIME_SO:-$BUILD_DIR/faketime.so}

# Observation recording is available before a case calls case_init (the
# variant builders log through it too), so default to a harmless sink.
CASE_OBS=${CASE_OBS:-/dev/null}
CASE_ID=${CASE_ID:-}
CASE_NAME=${CASE_NAME:-}
CASE_DIR=${CASE_DIR:-$BUILD_DIR}
CASE_BIN=${CASE_BIN:-}

. "$TESTS_DIR/variants.sh"

mkdir -p "$BUILD_DIR" "$OBS_DIR" "$LOCK_DIR_OK" 2>/dev/null
rm -rf "$LOCK_DIR_MISSING"

# ---------------------------------------------------------------- reporting --

say() {
	printf '%s\n' "$*"
}

die() {
	printf 'FATAL: %s\n' "$*" >&2
	exit 99
}

# obs <text...> -- append one line to the current case observation record.
obs() {
	printf '%s\n' "$*" >>"$CASE_OBS"
}

obs_begin() {
	: >"$CASE_OBS"
	obs "# case      : ${CASE_NAME:-<unset>}"
	obs "# defect id : ${CASE_ID:-<unset>}"
	obs "# date      : $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
	obs "# uname     : $(uname -srm)"
	obs "# binary    : ${CASE_BIN:-<none>}"
	obs "#"
}

obs_kv() {
	obs "$1: $2"
}

# ------------------------------------------------------------ case scratch ---

# ensure_shared_variants -- (re)build the variants the cases share, when they are
# missing or when P910ND_SRC points somewhere other than the last build.  Without
# this a case that is run on its own would silently use whatever a previous
# `make test` left behind, and P910ND_SRC would have no effect on it.
ensure_shared_variants() {
	_stamp="$BUILD_DIR/.variants.stamp"
	if [ -f "$_stamp" ] && [ "$(cat "$_stamp" 2>/dev/null)" = "$P910ND_SRC" ] &&
	   [ -x "$BUILD_DIR/test" ]; then
		return 0
	fi
	build_baseline >/dev/null 2>&1 || return 1
	build_variant test "-DLOCKFILE_DIR=\"$LOCK_DIR_OK\"" >/dev/null 2>&1 || return 1
	printf '%s\n' "$P910ND_SRC" >"$_stamp"
	return 0
}

# case_init <id> <slug> -- create the per-case scratch/observation files.
case_init() {
	CASE_ID="$1"
	CASE_NAME="$2"
	ensure_shared_variants || obs "WARN: could not refresh the shared variants"
	CASE_DIR="$BUILD_DIR/cases/$CASE_ID"
	CASE_OBS="$OBS_DIR/$CASE_ID.txt"
	rm -rf "$CASE_DIR"
	mkdir -p "$CASE_DIR" "$OBS_DIR"
	: >"$CASE_OBS"
	obs_begin
	say "--- $CASE_ID ($CASE_NAME)"
}

# ------------------------------------------------------------- tcp helpers ---

port_is_free() {
	"$PYTHON" - "$1" <<'EOF'
import socket, sys
s = socket.socket()
# SO_REUSEADDR mirrors what p910nd itself sets, and without it a lingering
# TIME_WAIT socket from a previous case would look like "port busy".
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    s.bind(("127.0.0.1", int(sys.argv[1])))
except OSError:
    sys.exit(1)
finally:
    s.close()
sys.exit(0)
EOF
}

wait_for_port() {
	# wait_for_port <host> <port> <seconds>
	"$PYTHON" - "$1" "$2" "$3" <<'EOF'
import socket, sys, time
host, port, deadline = sys.argv[1], int(sys.argv[2]), time.time() + float(sys.argv[3])
while time.time() < deadline:
    try:
        s = socket.create_connection((host, port), 0.25)
        s.close()
        sys.exit(0)
    except OSError:
        time.sleep(0.05)
sys.exit(1)
EOF
}

wait_for_gone() {
	# wait_for_gone <host> <port> <seconds>
	"$PYTHON" - "$1" "$2" "$3" <<'EOF'
import socket, sys, time
host, port, deadline = sys.argv[1], int(sys.argv[2]), time.time() + float(sys.argv[3])
while time.time() < deadline:
    try:
        s = socket.create_connection((host, port), 0.25)
        s.close()
        time.sleep(0.05)
    except OSError:
        sys.exit(0)
sys.exit(1)
EOF
}

# ---------------------------------------------------------- daemon control ---

DAEMON_PID=""
DAEMON_PGID=""

# daemon_start <logfile> <binary> [args...]
# Starts the daemon detached in its own session so the whole process group
# (daemon + every forked job child) can be signalled at once.
#
# stdin is /dev/null on purpose: p910nd decides between "daemon" and "one-shot
# service" by calling getsockname(0), so an inherited socket on fd 0 -- which is
# exactly what an IDE or a CI runner may hand us -- would silently turn the
# daemon into a one-shot service that never listens.
daemon_start() {
	_log=$1
	shift
	# setsid(1) forks unless it already is a process group leader, so $! would
	# be the short-lived setsid process rather than the daemon.  Have the
	# process that finally execs the daemon write its own pid instead: the
	# exec keeps the number, so the file always names the real process.
	_pidfile="$_log.pid"
	rm -f "$_pidfile"
	setsid sh -c 'echo $$ > "$1"; shift; exec "$@"' _ "$_pidfile" "$@" \
		>"$_log" 2>&1 </dev/null &
	_setsid_pid=$!
	_n=0
	while [ "$_n" -lt 40 ]; do
		DAEMON_PID=$(cat "$_pidfile" 2>/dev/null)
		[ -n "$DAEMON_PID" ] && break
		sleep 0.1
		_n=$((_n + 1))
	done
	if [ -z "$DAEMON_PID" ] || ! kill -0 "$DAEMON_PID" 2>/dev/null; then
		# No pid file: the daemon died during start-up.
		kill -KILL "$_setsid_pid" 2>/dev/null
		DAEMON_PID=""
		DAEMON_PGID=""
		return 1
	fi
	DAEMON_PGID=$(ps -o pgid= -p "$DAEMON_PID" 2>/dev/null | tr -d ' ')
	[ -n "$DAEMON_PGID" ] || DAEMON_PGID=$DAEMON_PID
	return 0
}

# daemon_stop [signal] -- signal the whole daemon process group.
daemon_stop() {
	_sig=${1:-TERM}
	if [ -n "$DAEMON_PGID" ]; then
		kill "-$_sig" "-$DAEMON_PGID" 2>/dev/null
		_n=0
		while [ "$_n" -lt 30 ]; do
			kill -0 "$DAEMON_PGID" 2>/dev/null || break
			sleep 0.1
			_n=$((_n + 1))
		done
		kill -KILL "-$DAEMON_PGID" 2>/dev/null
	fi
	DAEMON_PID=""
	DAEMON_PGID=""
}

# kill_stray -- belt and braces sweep of any daemon left behind by a failed
# case.  Matching is done on the build directory inside /proc/*/cmdline
# because the daemon renames its own argv[0].
kill_stray() {
	for _p in /proc/[0-9]*/cmdline; do
		_pid=${_p#/proc/}
		_pid=${_pid%/cmdline}
		[ "$_pid" = "$$" ] && continue
		if tr '\0' ' ' <"$_p" 2>/dev/null | grep -q "$BUILD_DIR/"; then
			kill -KILL "$_pid" 2>/dev/null
		fi
	done 2>/dev/null
	return 0
}

# --------------------------------------------------------------- assertions --

ASSERT_FAIL=0

assert_true() {
	# assert_true <condition-result 0/1> <description>
	if [ "$1" -eq 0 ]; then
		obs "ASSERT PASS  $2"
	else
		obs "ASSERT FAIL  $2"
		ASSERT_FAIL=1
	fi
}

assert_eq() {
	# assert_eq <actual> <expected> <description>
	if [ "$1" = "$2" ]; then
		obs "ASSERT PASS  $3 (actual=$1)"
	else
		obs "ASSERT FAIL  $3 (actual=$1 expected=$2)"
		ASSERT_FAIL=1
	fi
}

assert_contains() {
	# assert_contains <file> <pattern> <description>
	if grep -q -- "$2" "$1" 2>/dev/null; then
		obs "ASSERT PASS  $3"
	else
		obs "ASSERT FAIL  $3 (pattern '$2' not found in $1)"
		ASSERT_FAIL=1
	fi
}

assert_not_contains() {
	# assert_not_contains <file> <pattern> <description>
	if grep -q -- "$2" "$1" 2>/dev/null; then
		obs "ASSERT FAIL  $3 (pattern '$2' unexpectedly found in $1)"
		obs "            first match: $(grep -m1 -- "$2" "$1")"
		ASSERT_FAIL=1
	else
		obs "ASSERT PASS  $3"
	fi
}

assert_ge() {
	# assert_ge <actual> <minimum> <description>
	if [ "$1" -ge "$2" ] 2>/dev/null; then
		obs "ASSERT PASS  $3 (actual=$1 >= $2)"
	else
		obs "ASSERT FAIL  $3 (actual=$1 < $2)"
		ASSERT_FAIL=1
	fi
}

assert_lt() {
	# assert_lt <actual> <maximum> <description>
	if [ "$1" -lt "$2" ] 2>/dev/null; then
		obs "ASSERT PASS  $3 (actual=$1 < $2)"
	else
		obs "ASSERT FAIL  $3 (actual=$1 >= $2)"
		ASSERT_FAIL=1
	fi
}

# Floating point comparisons, for durations.  awk keeps this dependency free.
assert_f_ge() {
	if awk -v a="$1" -v b="$2" 'BEGIN{exit !(a>=b)}'; then
		obs "ASSERT PASS  $3 (actual=$1 >= $2)"
	else
		obs "ASSERT FAIL  $3 (actual=$1 < $2)"
		ASSERT_FAIL=1
	fi
}

assert_f_lt() {
	if awk -v a="$1" -v b="$2" 'BEGIN{exit !(a<b)}'; then
		obs "ASSERT PASS  $3 (actual=$1 < $2)"
	else
		obs "ASSERT FAIL  $3 (actual=$1 >= $2)"
		ASSERT_FAIL=1
	fi
}

# count_lines <file> <pattern> -- a single number, unlike `grep -c || echo 0`
# which prints two lines when grep finds nothing and exits 1.
count_lines() {
	if [ ! -e "$1" ]; then
		printf '0'
		return
	fi
	printf '%s' "$(grep -c -- "$2" "$1" 2>/dev/null | head -1)"
}

assert_file_exists() {
	if [ -e "$1" ]; then
		obs "ASSERT PASS  $2 ($1)"
	else
		obs "ASSERT FAIL  $2 ($1 missing)"
		ASSERT_FAIL=1
	fi
}

# Literal substring test on a *string* (assert_contains greps a file, and its
# pattern is a regex, so brackets in paths like p910[0-9]d.pid need this one).
assert_str_contains() {
	# assert_str_contains <string> <literal> <description>
	case "$1" in
	*"$2"*)
		obs "ASSERT PASS  $3"
		;;
	*)
		obs "ASSERT FAIL  $3 (literal '$2' not in: $1)"
		ASSERT_FAIL=1
		;;
	esac
}

assert_dir_exists() {
	if [ -d "$1" ]; then
		obs "ASSERT PASS  $2 ($1)"
	else
		obs "ASSERT FAIL  $2 ($1 missing)"
		ASSERT_FAIL=1
	fi
}
