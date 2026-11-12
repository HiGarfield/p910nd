#!/bin/sh
#
# PD-46  [medium]  Case discovery must not assume two-digit ids.
#
# The driver enumerated cases with sed 's/^(t-pd-[0-9][0-9]).*/\1/p' and
# the README index check had its own copy of the same two-digit expression.
# A file named t-pd-100-*.sh is truncated to t-pd-10 by both: the new case
# is never run, PD-10 may run twice, and the index check still says the
# documentation is complete.  The suite stays green while a regression
# case is silently ignored.
#
# The fix puts one width-independent case_ids() helper in lib.sh and uses it
# from both call sites.  This case exercises that helper on a synthetic
# fixture containing 2-, 3- and 4-digit ids.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-46 case-id-width

FIX="$CASE_DIR/fixture"
mkdir -p "$FIX"
for n in 10 100 1000; do
	: >"$FIX/t-pd-$n-probe.sh"
done
: >"$FIX/notes.txt"          # not a case
: >"$FIX/t-pd-abc-probe.sh"  # not a numeric case

ids=$(case_ids "$FIX")
obs_kv "discovered ids" "$(printf '%s' "$ids" | tr '\n' ',')"
assert_eq "$ids" "$(printf '10\n100\n1000')" \
	"case_ids() returns every numeric id without truncating its width"

# Reproduce the driver's exact no-argument pipeline on the same fixture.
discovered=$(case_ids "$FIX" | sed 's/^/t-pd-/')
obs_kv "driver CASE_LIST" "$(printf '%s' "$discovered" | tr '\n' ',')"
assert_eq "$discovered" "$(printf 't-pd-10\nt-pd-100\nt-pd-1000')" \
	"the driver's no-argument scan would run a three-digit case"

old=$(ls "$FIX" | sed -n 's/^\(t-pd-[0-9][0-9]\).*/\1/p' | sort -u)
obs_kv "old two-digit expression (for the record)" \
	"$(printf '%s' "$old" | tr '\n' ',')"
assert_eq "$old" "t-pd-10" \
	"the old expression really did collapse all three ids to t-pd-10"

assert_contains "$REPO_ROOT/tests/run-all.sh" "case_ids" \
	"the driver uses the shared helper instead of its own regex"
assert_contains "$REPO_ROOT/tests/cases/t-pd-32-readme-index.sh" "case_ids" \
	"the index check uses the shared helper too"
assert_contains "$REPO_ROOT/tests/cases/t-pd-32-readme-index.sh" \
	"t-pd-\[0-9\]\[0-9\]\*-" \
	"and its index-row pattern accepts wider ids"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
