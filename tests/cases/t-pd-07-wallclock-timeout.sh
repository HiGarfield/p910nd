#!/bin/sh
#
# PD-07  [中]  Timeouts must not depend on the wall clock.
#
# Every deadline used to come from gettimeofday(), i.e. CLOCK_REALTIME, so a
# single backwards clock step made every "now - start" difference negative:
# IDLE_TIMEOUT, SILENT_TIMEOUT, PRINTER_STALL_TIMEOUT, PRINTER_REPLY_WINDOW,
# SHUTDOWN_GRACE and the permanent-open budget all stopped firing and the job --
# and the daemon -- hung.  All of them now go through mono_now(), which uses
# CLOCK_MONOTONIC where the platform has it.
#
# A: with the monotonic clock, stepping the wall clock changes nothing
# B: the documented -DNO_CLOCK_GETTIME fallback still shows the old behaviour,
#    which proves both that the shim works and that the fallback is real
# C: static, both code paths are present and nothing bypasses mono_now()
#
# The clock is driven with an LD_PRELOAD shim, because a real clock step needs
# root and because the shim proves the dependency was on the *wall clock*.
#
# The order matters: the job has to be under way, and its start timestamp taken,
# *before* the clock is stepped.  A job that starts after the step sees a
# consistently shifted clock and times out normally, which would prove nothing.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-07 wallclock-timeout
CLOCK="$CASE_DIR/clock"
: >"$CASE_DIR/lp0"

# run-all.sh builds the shim; build it here too so the case stands alone.
if [ ! -f "$FAKETIME_SO" ]; then
	"$CC_BIN" -shared -fPIC -O1 -o "$FAKETIME_SO" \
		"$REPO_ROOT/tests/tools/faketime_preload.c" -ldl \
		>"$CASE_DIR/faketime.build.log" 2>&1 ||
		{ sed 's/^/    /' "$CASE_DIR/faketime.build.log" | while read -r l; do obs "$l"; done
		  assert_true 1 "the fake clock shim builds"; exit 1; }
fi

fb=$(build_variant test-noclock -DNO_CLOCK_GETTIME=1 "-DLOCKFILE_DIR=\"$CASE_DIR/lock\"")
assert_file_exists "$fb" "the gettimeofday fallback variant builds"
assert_file_exists "$BUILD_DIR/test" "the monotonic variant builds"

# probe_job <label> <binary> -- start a probe job, step the clock one second in,
# and report whether the client is still connected after seven more seconds.
probe_job() {
	_label=$1
	_bin=$2
	rm -f "$CLOCK"
	printf 'skew=0\n' >"$CLOCK"
	# exported only around the daemon: a function-scoped prefix would stay in
	# the shell's environment and drag the preload into every later command
	LD_PRELOAD="$FAKETIME_SO"
	P910ND_FAKE_CLOCK="$CLOCK"
	export LD_PRELOAD P910ND_FAKE_CLOCK
	daemon_start "$CASE_DIR/$_label.daemon.log" "$_bin" \
		-d -i 127.0.0.1 -f "$CASE_DIR/lp0" 0
	unset LD_PRELOAD P910ND_FAKE_CLOCK
	wait_for_port 127.0.0.1 "$TEST_PORT" 5
	# a probe: connects, sends nothing, never half-closes
	"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" --expect-reply 1 \
		--read-timeout 30 --label "$_label" >"$CASE_DIR/$_label.client" 2>&1 &
	CLIENT_PID=$!
	sleep 1
	printf 'skew=-3600\n' >"$CLOCK"
	obs "$_label: clock stepped back one hour 1s into the job"
	sleep 7
	if kill -0 "$CLIENT_PID" 2>/dev/null; then
		eval "STUCK_$_label=0"
		obs "$_label: after 8s the probe client is still connected"
	else
		eval "STUCK_$_label=1"
		obs "$_label: the probe client was already closed"
		wait "$CLIENT_PID" 2>/dev/null
	fi
	# While the client is still connected the daemon has to stay up: restoring
	# the clock is what makes the overdue timeout fire, and that is the point.
	eval "_stuck=\$STUCK_$_label"
	# snapshot the log *before* the clock is restored: afterwards the overdue
	# timeout is expected to have fired, so the log cannot answer the question
	_early=$(count_lines "$CASE_DIR/$_label.daemon.log" 'no data transferred')
	eval "EARLY_$_label=\$_early"
	obs "$_label: idle timeouts logged while the clock was behind: $_early"
	if [ "$_stuck" -eq 0 ] && [ "$_label" = "b" ]; then
		printf 'skew=0\n' >"$CLOCK"
		obs "$_label: wall clock restored, the overdue timeout should fire now"
		_n=0
		while [ "$_n" -lt 30 ]; do
			kill -0 "$CLIENT_PID" 2>/dev/null || break
			sleep 0.2
			_n=$((_n + 1))
		done
		kill -KILL "$CLIENT_PID" 2>/dev/null
		wait "$CLIENT_PID" 2>/dev/null
	fi
	obs "$_label client: $(tr '\n' '|' <"$CASE_DIR/$_label.client")"
	obs "$_label daemon log:"
	sed 's/^/    /' "$CASE_DIR/$_label.daemon.log" | while read -r l; do obs "$l"; done
	daemon_stop
	kill_stray
}

# ---------------------------------------------------------------- part A ----
say "A: CLOCK_MONOTONIC build"
probe_job a "$BUILD_DIR/test"
a_how=$(sed -n 's/.*RESULT how=\([a-z-]*\).*/\1/p' "$CASE_DIR/a.client")
a_el=$(sed -n 's/.*elapsed=\([0-9.]*\).*/\1/p' "$CASE_DIR/a.client")
assert_eq "$STUCK_a" "1" "A the idle probe was closed despite the wall clock step"
assert_eq "$a_how" "fin" "A with a clean FIN, i.e. the job ended normally"
assert_f_ge "${a_el:-0}" "3" "A and on schedule (IDLE_TIMEOUT), not before"
assert_contains "$CASE_DIR/a.daemon.log" "no data transferred" \
	"A the daemon logged the idle timeout"

# ---------------------------------------------------------------- part B ----
say "B: -DNO_CLOCK_GETTIME=1 build (the documented fallback)"
probe_job b "$fb"
if [ "${STUCK_b:-1}" -eq 0 ]; then
	assert_eq "${EARLY_b:-1}" "0" \
		"B the fallback build really does depend on the wall clock"
	b_how=$(sed -n 's/.*RESULT how=\([a-z-]*\).*/\1/p' "$CASE_DIR/b.client" 2>/dev/null)
	assert_eq "$b_how" "fin" \
		"B restoring the wall clock makes the overdue timeout fire at once"
	assert_contains "$CASE_DIR/b.daemon.log" "no data transferred" \
		"B and the daemon then logs the timeout it had been missing"
else
	assert_true 1 "B the fallback build really does depend on the wall clock"
fi

# ---------------------------------------------------------------- part C ----
say "C: static -- the source really has both paths"
obs "clock_gettime / gettimeofday in mono_now():"
grep -n 'clock_gettime\|(void)gettimeofday' "$REPO_ROOT/p910nd.c" | sed 's/^/    /' |
	while read -r l; do obs "$l"; done
assert_contains "$REPO_ROOT/p910nd.c" "P910ND_HAVE_MONOTONIC" "C the conditional gate exists"
assert_contains "$REPO_ROOT/p910nd.c" "NO_CLOCK_GETTIME" "C and can be forced off"
assert_contains "$REPO_ROOT/p910nd.c" "clock_gettime(CLOCK_MONOTONIC, &ts)" "C the monotonic clock is used"
assert_contains "$REPO_ROOT/p910nd.c" "(void)gettimeofday(tv, 0);" "C with a gettimeofday fallback"
n_direct=$(count_lines "$REPO_ROOT/p910nd.c" 'gettimeofday(&')
obs_kv "C direct gettimeofday(&...) call sites outside mono_now" "$n_direct"
assert_eq "$n_direct" "0" "C every deadline goes through mono_now()"
assert_eq "$(count_lines "$REPO_ROOT/p910nd.c" 'time(0)')" "0" \
	"C and no deadline uses time() either"
# and the two builds really differ
# wc -l rather than grep -c: grep exits 1 on no match, which would append a
# second line and turn the count into "0\n0".
nm_mono=$(nm -D "$BUILD_DIR/test" 2>/dev/null | grep clock_gettime | wc -l)
nm_fb=$(nm -D "$fb" 2>/dev/null | grep clock_gettime | wc -l)
obs_kv "C clock_gettime references in the default build" "$nm_mono"
obs_kv "C clock_gettime references in the fallback build" "$nm_fb"
assert_ge "$nm_mono" "1" "C the default build calls clock_gettime"
assert_eq "$nm_fb" "0" "C the fallback build does not"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
