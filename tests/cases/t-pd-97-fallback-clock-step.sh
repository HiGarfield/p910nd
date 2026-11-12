#!/bin/sh
#
# PD-97  [high]  The no-monotonic-clock build must survive a wall-clock step.
#
# DEFECT_REPORT.md records this explicitly under PD-07: with
# -DNO_CLOCK_GETTIME=1, mono_now() is just gettimeofday(), so stepping the
# wall clock back one hour one second into a job made every
# "now - start >= limit" comparison negative.  The documented measurement
# was "8 s later the client is still connected, zero idle timeouts".
# That build is shipped for glibc < 2.17 (clock_gettime lives in librt) and
# for any toolchain that cannot use CLOCK_MONOTONIC, so those hosts still
# had the very bug PD-07 claims to fix.
#
# mono_now() now accumulates the total backwards step in wall_correction.
# A step freezes the returned time for one sample; subsequent samples keep
# advancing from the corrected base, so deadlines still fire.
#
# A: two consecutive backwards steps do not stop the idle timeout.
# B: control -- with no step the same build times out on the same schedule.
# C: static -- the compensation is confined to the fallback code path.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-97 fallback-clock-step

SRC="$REPO_ROOT/p910nd.c"
CLOCK="$CASE_DIR/clock"
: >"$CASE_DIR/lp0"

if [ ! -f "$FAKETIME_SO" ]; then
	"$CC_BIN" -shared -fPIC -O1 -o "$FAKETIME_SO" \
		"$REPO_ROOT/tests/tools/faketime_preload.c" -ldl \
		>"$CASE_DIR/faketime.build.log" 2>&1 ||
		{ assert_true 1 "the fake clock shim builds"; kill_stray; exit 1; }
fi
BIN=$(build_variant test-noclock-step -DNO_CLOCK_GETTIME=1 \
	"-DLOCKFILE_DIR=\"$CASE_DIR/lock\"")
assert_file_exists "$BIN" "the gettimeofday-only variant builds"

# run_job <label> <step-mode>; step-mode 2 = two steps, 0 = no step.
run_job() {
	_label=$1
	_mode=$2
	rm -f "$CLOCK"
	printf 'skew=0\n' >"$CLOCK"
	LD_PRELOAD="$FAKETIME_SO"
	P910ND_FAKE_CLOCK="$CLOCK"
	export LD_PRELOAD P910ND_FAKE_CLOCK
	daemon_start "$CASE_DIR/$_label.daemon.log" "$BIN" \
		-d -i 127.0.0.1 -f "$CASE_DIR/lp0" 0
	unset LD_PRELOAD P910ND_FAKE_CLOCK
	if [ -z "$DAEMON_PID" ]; then
		obs "$_label daemon failed: $(tr '\n' '|' <"$CASE_DIR/$_label.daemon.log")"
		return 1
	fi
	wait_for_port 127.0.0.1 "$TEST_PORT" 5 || return 1
	"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" \
		--expect-reply 1 --read-timeout 20 --label "$_label" \
		>"$CASE_DIR/$_label.client" 2>&1 &
	CLIENT_PID=$!
	sleep 1
	if [ "$_mode" -eq 2 ]; then
		printf 'skew=-3600\n' >"$CLOCK"
		obs "$_label: first wall-clock step (-1h) 1s into the job"
		sleep 2
		printf 'skew=-7200\n' >"$CLOCK"
		obs "$_label: second wall-clock step (-2h) 3s into the job"
	fi
	_n=0
	while [ "$_n" -lt 60 ] && kill -0 "$CLIENT_PID" 2>/dev/null; do
		sleep 0.2
		_n=$((_n + 1))
	done
	if kill -0 "$CLIENT_PID" 2>/dev/null; then
		obs "$_label: client STILL connected after 12s"
	else
		obs "$_label: client closed on schedule"
	fi
	kill -KILL "$CLIENT_PID" 2>/dev/null
	wait "$CLIENT_PID" 2>/dev/null
	obs "$_label client: $(tr '\n' '|' <"$CASE_DIR/$_label.client")"
	obs "$_label timeout lines: $(count_lines "$CASE_DIR/$_label.daemon.log" 'no data transferred')"
	daemon_stop
	kill_stray
	return 0
}

check_result() {
	_label=$1
	_how=$(sed -n 's/.*RESULT how=\([a-z-]*\).*/\1/p' "$CASE_DIR/$_label.client")
	_el=$(sed -n 's/.*elapsed=\([0-9.]*\).*/\1/p' "$CASE_DIR/$_label.client")
	obs "$_label how=$_how elapsed=${_el:-missing}"
	assert_eq "$_how" "fin" \
		"$_label the probe was closed with a clean FIN"
	assert_f_ge "${_el:-0}" "2" \
		"$_label not before the idle deadline"
	assert_f_lt "${_el:-0}" "10" \
		"$_label and not wedged by the clock step"
	assert_contains "$CASE_DIR/$_label.daemon.log" "no data transferred" \
		"$_label the daemon logged the idle timeout"
}

say "A: two backwards wall-clock steps do not disable the timeout"
run_job stepped 2
check_result stepped

say "B: control -- without a step the schedule is unchanged"
run_job steady 0
check_result steady

say "C: static -- compensation lives only in the fallback path"
"$PYTHON" - "$SRC" >"$CASE_DIR/c.out" 2>&1 <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r"static int mono_now\(struct timeval \*tv\) \{(.*?)\n\}",
              s, re.S)
if not m:
    print("no mono_now body")
    sys.exit(1)
body = m.group(1)
guard = "#ifndef P910ND_HAVE_MONOTONIC" in body
decl = "static long wall_correction;" in body
applied = "tv->tv_sec += wall_correction;" in body
accum = "wall_correction += (long)(last.tv_sec - tv->tv_sec);" in body
gettime = body.find("gettimeofday(tv, 0)")
apply_at = body.find("tv->tv_sec += wall_correction;")
print("guard=%s decl=%s applied=%s accum=%s after_gettimeofday=%s" %
      (guard, decl, applied, accum, 0 <= gettime < apply_at))
sys.exit(0 if guard and decl and applied and accum
         and 0 <= gettime < apply_at else 1)
EOF
assert_eq "$?" "0" \
	"C wall_correction is declared, applied after gettimeofday and accumulated"
assert_contains "$CASE_DIR/c.out" \
	"guard=True decl=True applied=True accum=True after_gettimeofday=True" \
	"C every part of the guard was checked"
assert_contains "$SRC" "PD-97: with no monotonic source" \
	"C the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
