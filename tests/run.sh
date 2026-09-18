#!/bin/sh
#
# Full verification suite for p910nd (GPLv2).
#
#   tests/run.sh [filter-regex]
#
# Phases:
#   A  build gates    - every supported build must compile with ZERO warnings
#   B  unit tests     - one binary per tests/*.c (each includes ../p910nd.c)
#   C  functional     - end-to-end daemon tests via tests/functional.py
#   D  sanitizers     - the same functional traffic under ASan/UBSan
#   E  valgrind       - leak + file-descriptor tracking on a real job
#
# ANSI C89 (ISO C90) is the mandatory language level: every phase compiles
# with -std=c89 -Wall -Wextra -Wpedantic.
#
# Prints a PASS/FAIL/SKIP table and exits non-zero if anything failed.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2

SRC=p910nd.c
CC_GCC=${CC_GCC:-gcc}
CC_CLANG=${CC_CLANG:-clang}
# test_fuzz_timing and test_bidir_printer_preclear_response iterate dozens of
# jobs through the IDLE_TIMEOUT_SEC grace window and legitimately need ~220s
# of wall clock (they are cheap in CPU: they mostly sleep).
UNIT_TIMEOUT=${UNIT_TIMEOUT:-300}
FILTER=${1:-}

WARN="-Wall -Wextra -Wpedantic"
STD="-std=c89"

BUILD=$(mktemp -d "${TMPDIR:-/tmp}/p910nd-audit-XXXXXX")
LOCKDIR="$BUILD/lock"
DEEPLOCK="$BUILD/deep/missing/lockdir"
PIDDIR="$BUILD/pidtest"
mkdir -p "$LOCKDIR" || exit 2

PASSES=0
FAILS=0
SKIPS=0

pass() { PASSES=$((PASSES + 1)); printf '%-8s %s\n' '[PASS]' "$1"; }
fail() { FAILS=$((FAILS + 1)); printf '%-8s %s\n' '[FAIL]' "$1"; }
skip() { SKIPS=$((SKIPS + 1)); printf '%-8s %s\n' '[SKIP]' "$1"; }

# compile_ok <label> <output> <compiler-args...>
# Fails unless the compilation succeeds AND emits no diagnostics at all.
compile_ok() {
	_lbl=$1
	_out=$2
	shift 2
	_log="$BUILD/$_lbl.compile.log"
	if "$@" -o "$_out" 2>"$_log" && [ ! -s "$_log" ]; then
		pass "$_lbl"
		return 0
	fi
	fail "$_lbl (see $_log)"
	sed -n '1,25p' "$_log"
	return 1
}

echo "=== p910nd verification suite (C89) ==="
echo "root=$ROOT build=$BUILD"
echo

# ---------------------------------------------------------------- phase A
echo "--- phase A: build gates (zero warnings required) ---"

compile_ok gate-gcc-c89 "$BUILD/p910nd-gcc" \
	"$CC_GCC" $STD $WARN -O2 $SRC

compile_ok gate-clang-c89 "$BUILD/p910nd-clang" \
	"$CC_CLANG" $STD $WARN -O2 $SRC

compile_ok gate-gcc-testing "$BUILD/p910nd-testing" \
	"$CC_GCC" $STD $WARN -DTESTING -c $SRC

compile_ok gate-gcc-asan "$BUILD/p910nd-asan" \
	"$CC_GCC" $STD $WARN -O1 -g -fsanitize=address,undefined \
	-fno-omit-frame-pointer -DLOCKFILE_DIR="\"$LOCKDIR\"" $SRC

compile_ok gate-gcc-default-make "$BUILD/p910nd-make" \
	"$CC_GCC" $WARN $SRC

# The Makefile must keep the warning flags the caller passes in: several
# distributions build with -Werror=... hardening switches, and dropping them
# would disable checks the package build expects to run.  -n prints the
# command line without running it; -B forces it to be printed even though a
# binary may already exist.
if make -n -B CFLAGS="-Werror -O2" p910nd >"$BUILD/makeflags.log" 2>&1 &&
	grep -q -- '-Werror' "$BUILD/makeflags.log"; then
	pass "makefile-keeps-user-wflags"
else
	fail "makefile-keeps-user-wflags (caller's -W flags are discarded)"
	sed -n '1,5p' "$BUILD/makeflags.log"
fi

# Portability: a second C library and a second word size catch assumptions
# about sizeof(long), pid_t and friends that only one target would hide.
# Probed rather than assumed, so a host without them SKIPs instead of failing.
if command -v musl-gcc >/dev/null 2>&1; then
	compile_ok gate-musl-c89 "$BUILD/p910nd-musl" \
		musl-gcc $STD $WARN -O2 $SRC
else
	skip "gate-musl-c89 (musl-gcc not installed)"
fi

printf 'int main(void){return 0;}\n' >"$BUILD/m32probe.c"
if "$CC_GCC" -m32 "$BUILD/m32probe.c" -o "$BUILD/m32probe" \
	2>/dev/null; then
	compile_ok gate-gcc-32bit "$BUILD/p910nd-32" \
		"$CC_GCC" -m32 $STD $WARN -O2 $SRC
else
	skip "gate-gcc-32bit (no 32-bit multilib)"
fi

if [ -f /usr/include/tcpd.h ]; then
	compile_ok gate-libwrap "$BUILD/p910nd-libwrap" \
		"$CC_GCC" $STD $WARN -DUSE_LIBWRAP -DUSE_GETPROTOBYNAME $SRC -lwrap
	# A libwrap build usable by the functional phase, plus the preloadable
	# stub that forces hosts_ctl() to deny.
	"$CC_GCC" $STD $WARN -O1 -g -DUSE_LIBWRAP \
		-DLOCKFILE_DIR="\"$LOCKDIR\"" -o "$BUILD/p910nd-libwrap-func" \
		$SRC -lwrap 2>>"$BUILD/libwrap.build.log"
	cc -shared -fPIC -o "$BUILD/hosts_ctl_stub.so" tests/hosts_ctl_stub.c \
		2>>"$BUILD/libwrap.build.log"
	LIBWRAP_ARGS="--bin-libwrap $BUILD/p910nd-libwrap-func"
	if [ -f "$BUILD/hosts_ctl_stub.so" ]; then
		LIBWRAP_ARGS="$LIBWRAP_ARGS --libwrap-stub $BUILD/hosts_ctl_stub.so"
	fi
else
	LIBWRAP_ARGS=""
	skip "gate-libwrap (tcpd.h not installed)"
fi

# ------------------------------------------------- phase A2: static analysis
echo
echo "--- phase A2: static analysis (cppcheck) ---"
if command -v cppcheck >/dev/null 2>&1; then
	cppcheck --enable=warning,performance,portability \
		--inline-suppr --error-exitcode=0 \
		--template='{severity}:{file}:{line}: {id} {message}' \
		"$SRC" >"$BUILD/cppcheck.log" 2>&1
	_n=$(wc -l <"$BUILD/cppcheck.log")
	if grep -q '^error:' "$BUILD/cppcheck.log"; then
		fail "cppcheck ($_n findings, includes errors)"
		grep '^error:' "$BUILD/cppcheck.log" | head -20
	else
		pass "cppcheck ($_n findings, no errors)"
	fi
else
	skip "cppcheck (not installed)"
fi

# ---------------------------------------------------------------- phase B
echo
echo "--- phase B: unit tests ---"
for t in tests/*.c; do
	name=$(basename "$t" .c)
	if [ -n "$FILTER" ]; then
		case "$name" in *"$FILTER"*) ;; *) continue ;; esac
	fi
	bin="$BUILD/$name"
	# Helpers such as tests/hosts_ctl_stub.c are shared objects, not
	# programs: skip anything without a main().
	if ! grep -q 'int main' "$t"; then
		continue
	fi
	# Keep lock-touching tests inside the scratch directory instead of the
	# host's /var/lock, unless the test already pins LOCKFILE_DIR itself.
	if grep -q 'define LOCKFILE_DIR' "$t"; then
		lockdef=""
	else
		# The embedded quotes are meant to reach the compiler, so the macro
		# expands to a C string literal; shellcheck cannot see that.
		# shellcheck disable=SC2089,SC2090
		lockdef="-DLOCKFILE_DIR=\"$LOCKDIR\""
	fi
	# shellcheck disable=SC2086
	if ! "$CC_GCC" $STD $WARN -O1 -DTESTING $lockdef \
		-o "$bin" "$t" 2>"$BUILD/$name.build.log"; then
		fail "$name (compile)"
		sed -n '1,15p' "$BUILD/$name.build.log"
		continue
	fi
	if [ -s "$BUILD/$name.build.log" ]; then
		fail "$name (compile warnings)"
		sed -n '1,15p' "$BUILD/$name.build.log"
		continue
	fi
	if timeout "$UNIT_TIMEOUT" "$bin" >"$BUILD/$name.run.log" 2>&1; then
		pass "$name"
	else
		rc=$?
		fail "$name (exit $rc)"
		tail -15 "$BUILD/$name.run.log"
	fi
done

# ---------------------------------------------------------------- phase C
echo
echo "--- phase C: functional end-to-end tests ---"
FUNCBIN="$BUILD/p910nd-func"
DEEPBIN="$BUILD/p910nd-deeplock"
ALLOWBIN="$BUILD/p910nd-allowlist"

# -DPIDFILE keeps the pid file inside the scratch tree too, so the
# pid-file cases can run without touching the real /var/run.
if "$CC_GCC" $STD $WARN -O1 -g -DLOCKFILE_DIR="\"$LOCKDIR\"" \
	-DPIDFILE="\"$PIDDIR/p910%cd.pid\"" \
	-o "$FUNCBIN" $SRC 2>"$BUILD/func.build.log" &&
"$CC_GCC" $STD $WARN -O1 -g -DLOCKFILE_DIR="\"$DEEPLOCK\"" \
	-o "$DEEPBIN" $SRC 2>>"$BUILD/func.build.log" &&
"$CC_GCC" $STD $WARN -O1 -g -DLOCKFILE_DIR="\"$LOCKDIR\"" \
	-DDEVICE_ALLOWLIST="\"/dev/lp%c\"" \
	-o "$ALLOWBIN" $SRC 2>>"$BUILD/func.build.log"; then
	func_args="--bin $FUNCBIN --bin-deeplock $DEEPBIN --pidfile-dir $PIDDIR"
	func_args="$func_args --bin-allowlist $ALLOWBIN $LIBWRAP_ARGS"
	[ -n "$FILTER" ] && func_args="$func_args --filter $FILTER"
	# shellcheck disable=SC2086
	out=$(python3 tests/functional.py $func_args 2>&1)
	printf '%s\n' "$out"
	n_pass=$(printf '%s\n' "$out" | grep -c '^PASS ' || true)
	n_fail=$(printf '%s\n' "$out" | grep -c '^FAIL ' || true)
	n_skip=$(printf '%s\n' "$out" | grep -c '^SKIP ' || true)
	PASSES=$((PASSES + n_pass))
	FAILS=$((FAILS + n_fail))
	SKIPS=$((SKIPS + n_skip))
else
	fail "functional build"
	sed -n '1,20p' "$BUILD/func.build.log"
fi

# ---------------------------------------------------------------- phase D
echo
echo "--- phase D: functional traffic under ASan/UBSan ---"
if [ -x "$BUILD/p910nd-asan" ]; then
	ASAN_ENV="ASAN_OPTIONS=detect_leaks=1:abort_on_error=0"
	UBSAN_ENV="UBSAN_OPTIONS=print_stacktrace=1"
	asan_out=$(env $ASAN_ENV $UBSAN_ENV timeout 600 \
		python3 tests/functional.py --bin "$BUILD/p910nd-asan" \
		--filter 't_transfer_1byte|t_boundaries|t_half_close|t_bidir' 2>&1)
	printf '%s\n' "$asan_out" | grep -E '^(PASS|FAIL|SKIP) ' || true
	a_fail=$(printf '%s\n' "$asan_out" | grep -c '^FAIL ' || true)
	a_pass=$(printf '%s\n' "$asan_out" | grep -c '^PASS ' || true)
	PASSES=$((PASSES + a_pass))
	FAILS=$((FAILS + a_fail))
	if printf '%s\n' "$asan_out" |
		grep -qE 'ERROR: (Address|Leak)Sanitizer|runtime error'; then
		fail "asan/ubsan reported diagnostics"
	else
		pass "asan+ubsan clean"
	fi
else
	skip "asan/ubsan (build failed)"
fi

# ---------------------------------------------------------------- phase E
echo
echo "--- phase E: valgrind (leaks + fd tracking) ---"
if command -v valgrind >/dev/null 2>&1 && [ -x "$FUNCBIN" ]; then
	sink="$BUILD/vg-printer"
	: >"$sink"
	vglog="$BUILD/valgrind.log"
	valgrind --leak-check=full --track-fds=yes --error-exitcode=99 \
		--log-file="$vglog" "$FUNCBIN" -d -f "$sink" 0 &
	vgpid=$!
	# Give the daemon time to bind, then run one job.
	i=0
	while [ $i -lt 100 ]; do
		if python3 -c "import socket,sys
s=socket.create_connection(('127.0.0.1',9100),0.2)
s.close()" 2>/dev/null; then break; fi
		i=$((i + 1))
		sleep 0.1
	done
	# Several jobs in a row: a descriptor leaked once per job would show up
	# as a growing number of open sockets.
	python3 - <<'PY'
import socket
for _ in range(10):
    d = bytes(range(256)) * 400
    s = socket.create_connection(('127.0.0.1', 9100), timeout=20)
    s.sendall(d)
    s.shutdown(socket.SHUT_WR)
    while s.recv(65536):
        pass
    s.close()
PY
	sleep 1
	kill -TERM "$vgpid" 2>/dev/null
	wait "$vgpid" 2>/dev/null
	if grep -qE 'definitely lost: [1-9]|indirectly lost: [1-9]' "$vglog"; then
		fail "valgrind: leaked memory"
		grep -E 'lost:' "$vglog"
	elif grep -E 'Open (AF_INET|AF_INET6) socket' "$vglog" |
		grep -v unbound | grep -q '<->'; then
		# Every accepted connection must have been closed.  The listening
		# socket is reported as "<-> unbound" and legitimately stays open;
		# descriptors marked "inherited from parent" belong to caller's
		# environment, not to the daemon.
		fail "valgrind: accepted connection left open (fd leak per job)"
		grep -E 'Open (AF_INET|AF_INET6) socket' "$vglog" | grep -v unbound
	elif grep -qE 'ERROR SUMMARY: [1-9]' "$vglog"; then
		fail "valgrind errors"
		grep -E 'ERROR SUMMARY' "$vglog"
	else
		pass "valgrind clean (10 jobs, no leaked memory or connection fd)"
	fi
else
	skip "valgrind (not installed or functional build failed)"
fi

# ---------------------------------------------------------------- summary
echo
echo "==================== summary ===================="
printf 'passed: %d   failed: %d   skipped: %d\n' "$PASSES" "$FAILS" "$SKIPS"
echo "artifacts: $BUILD"
if [ "$FAILS" -gt 0 ]; then
	exit 1
fi
exit 0
