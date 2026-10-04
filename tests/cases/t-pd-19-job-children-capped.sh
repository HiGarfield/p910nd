#!/bin/sh
#
# PD-19  [高]  Every client is served, but not every client is forked at once.
#
# d78a7a1 removed MAX_CHILDREN (12) and with it the last limit on how many job
# children may exist at the same time, on the argument that refusing a
# connection would drop a print job.  Conflating "every client is served" with
# "every client is forked immediately" is what lets a printer that never becomes
# available fork one process per connection - each holding a descriptor and each
# blocked on the printer - until the machine runs out of processes or memory,
# at which point *all* printing stops.
#
# A: with MAX_JOB_CHILDREN=2 the number of job children stops at two, and no
#    client is refused - the rest simply wait in the listen backlog
# B: control - with the cap removed every client is forked at once, which is
#    what makes the cap worth having

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-19 job-children-capped

LOCKDIR="$CASE_DIR/lock"
mkdir -p "$LOCKDIR" "$CASE_DIR/lock2"
NCLIENTS=6
LINGER=5

capped=$(build_variant test-jobcap "-DLOCKFILE_DIR=\"$LOCKDIR\"" -DMAX_JOB_CHILDREN=2)
assert_file_exists "$capped" "the capped variant builds"
uncapped=$(build_variant test-jobcap-off "-DLOCKFILE_DIR=\"$CASE_DIR/lock2\"" \
	-DMAX_JOB_CHILDREN=0)
assert_file_exists "$uncapped" "the uncapped variant builds"

DEV="$CASE_DIR/lp0"
: >"$DEV"

# Hold the byte-1 job lock so every job child blocks in it and stays alive:
# that is what makes the number of children observable.
occupy_job_lock() {
	# occupy_job_lock <lockfile> <holder-out>
	mkdir -p "$(dirname "$1")"
	rm -f "$1"
	"$PYTHON" "$TOOLS_DIR/lockhold.py" hold --file "$1" --byte 1 \
		--seconds 60 >"$2" 2>&1 &
	HOLDER=$!
	for _i in 1 2 3 4 5 6 7 8 9 10; do
		grep -q LOCKED "$2" 2>/dev/null && break
		sleep 0.2
	done
	obs "lock holder: $(tr '\n' '|' <"$2")"
}

count_job_children() {
	# direct children of the daemon, i.e. the job children it forked
	ps -o pid= --ppid "$1" 2>/dev/null | grep -c '[0-9]'
}

run_round() {
	# run_round <label> <binary> <lockfile>
	_label=$1
	_bin=$2
	_lock=$3

	occupy_job_lock "$_lock" "$CASE_DIR/holder-$_label.out"
	assert_contains "$CASE_DIR/holder-$_label.out" "byte=1" \
		"$_label the fixture holds the job lock range"

	daemon_start "$CASE_DIR/$_label.daemon.log" "$_bin" -d -i 127.0.0.1 \
		-f "$DEV" 0
	if [ -z "$DAEMON_PID" ]; then
		obs "$_label daemon failed to start: $(tr '\n' '|' <"$CASE_DIR/$_label.daemon.log")"
		kill "$HOLDER" 2>/dev/null
		wait "$HOLDER" 2>/dev/null
		return 1
	fi
	wait_for_port 127.0.0.1 "$TEST_PORT" 5 || obs "$_label the port never came up"

	# --linger keeps a client connected without sending or half-closing, so it
	# stays a live job for the whole window.
	_pids=
	_i=1
	while [ "$_i" -le "$NCLIENTS" ]; do
		"$PYTHON" "$TOOLS_DIR/client.py" --port "$TEST_PORT" \
			--linger "$LINGER" --read-timeout 20 \
			--label "$_label-$_i" >"$CASE_DIR/$_label.client.$_i" 2>&1 &
		_pids="$_pids $!"
		_i=$((_i + 1))
	done

	# Long enough for every client that will be accepted to have been accepted
	# and forked; well short of LINGER so none of them has gone away yet.
	sleep 2.5
	_kids=$(count_job_children "$DAEMON_PID")
	obs_kv "$_label job children forked" "$_kids"
	eval "KIDS_$_label=\$_kids"

	# wait for the clients only: the lock holder is also a background job and
	# is killed explicitly below.
	# shellcheck disable=SC2086
	wait $_pids

	_refused=0
	_i=1
	while [ "$_i" -le "$NCLIENTS" ]; do
		if grep -q 'how=rst\|connect failed' \
			"$CASE_DIR/$_label.client.$_i" 2>/dev/null; then
			_refused=$((_refused + 1))
		fi
		_i=$((_i + 1))
	done
	obs_kv "$_label clients refused or reset" "$_refused"
	eval "REFUSED_$_label=\$_refused"

	daemon_stop
	kill "$HOLDER" 2>/dev/null
	wait "$HOLDER" 2>/dev/null
	kill_stray
	return 0
}

# ---------------------------------------------------------------- part A ----
say "A: MAX_JOB_CHILDREN=2 -- at most two children, nobody refused"
run_round capped "$capped" "$LOCKDIR/p9100d" || obs "A round failed"

assert_lt "${KIDS_capped:-99}" "3" \
	"A no more than two job children exist at the same time"
assert_ge "${KIDS_capped:-0}" "1" \
	"A and the first client is served (the cap is not a refusal)"
assert_eq "${REFUSED_capped:-?}" "0" \
	"A none of the six clients was refused: the rest waited in the backlog"

# ---------------------------------------------------------------- part B ----
say "B: control -- without the cap every client is forked at once"
run_round off "$uncapped" "$CASE_DIR/lock2/p9100d" || obs "B round failed"

assert_ge "${KIDS_off:-0}" "$NCLIENTS" \
	"B the uncapped build forks one child per client, which is what the cap bounds"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
