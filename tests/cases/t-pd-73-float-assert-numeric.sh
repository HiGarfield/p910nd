#!/bin/sh
#
# PD-73  [medium]  Float assertions must not accept missing measurements.
#
# assert_f_lt()/assert_f_ge() handed the operands straight to awk.  awk
# coerces "" and non-numeric strings to zero, and compares a strnum with a
# pure string lexicographically, so a case whose measurement was never
# produced could still report PASS:
#   assert_f_lt "" 1.0        -> awk: "" < 1.0  -> PASS
#   assert_f_ge "abc" 0.0     -> awk: "abc" >= "0.0" -> PASS
# That is a false green exactly when a probe failed, i.e. when the suite most
# needs to notice.  PD-42/PD-55/PD-64 fixed the same class elsewhere.
#
# A: empty and non-numeric operands are rejected by both helpers.
# B: valid float comparisons keep their pass/fail behaviour.
# C: static -- the numeric guard is present in both helpers.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-73 float-assert-numeric

LIB="$TESTS_DIR/lib.sh"

# Run one assertion with the observations redirected: the deliberate
# failures below are probes, not failures of this case, and must not leave
# "ASSERT FAIL" lines in the real observation record.
probe() {
	_fn=$1
	_a=$2
	_b=$3
	_old_obs=$CASE_OBS
	CASE_OBS="$CASE_DIR/probes.obs"
	ASSERT_FAIL=0
	"$_fn" "$_a" "$_b" "probe $_fn($_a,$_b)"
	PROBE_RC=$ASSERT_FAIL
	CASE_OBS=$_old_obs
	ASSERT_FAIL=0
	obs "probe $_fn('$_a','$_b') assertion_rc: $PROBE_RC (1=failed)"
}

say "A: missing or non-numeric measurements must fail the assertion"
probe assert_f_lt "" "1.0"
assert_eq "$PROBE_RC" "1" "A assert_f_lt rejects an empty actual"
probe assert_f_ge "abc" "0.0"
assert_eq "$PROBE_RC" "1" "A assert_f_ge rejects a non-numeric actual"
probe assert_f_lt "0.1" ""
assert_eq "$PROBE_RC" "1" "A assert_f_lt rejects an empty threshold"
probe assert_f_ge "1.0" "n/a"
assert_eq "$PROBE_RC" "1" "A assert_f_ge rejects a non-numeric threshold"
probe assert_f_lt "nan" "1.0"
assert_eq "$PROBE_RC" "1" "A assert_f_lt rejects NaN-like text"
probe assert_f_ge "0.1" "0.1.2"
assert_eq "$PROBE_RC" "1" "A assert_f_ge rejects a malformed number"

say "B: real measurements still compare correctly"
probe assert_f_lt "0.100" "0.4"
assert_eq "$PROBE_RC" "0" "B 0.100 < 0.4 passes"
probe assert_f_ge "1.201" "1.0"
assert_eq "$PROBE_RC" "0" "B 1.201 >= 1.0 passes"
probe assert_f_ge "1.0" "1.0"
assert_eq "$PROBE_RC" "0" "B the inclusive boundary passes"
probe assert_f_lt "0.9" "0.4"
assert_eq "$PROBE_RC" "1" "B 0.9 < 0.4 fails"
probe assert_f_ge "0.1" "1.0"
assert_eq "$PROBE_RC" "1" "B 0.1 >= 1.0 fails"
probe assert_f_lt "-1" "0"
assert_eq "$PROBE_RC" "0" "B negative values are numeric"

say "C: static -- both helpers validate their operands"
assert_eq "$(count_lines "$LIB" 'a ~ /^-?\[0-9\]')" "2" \
	"C the actual operand is checked in both helpers"
assert_eq "$(count_lines "$LIB" 'b ~ /^-?\[0-9\]')" "2" \
	"C and so is the threshold"
assert_contains "$LIB" "PD-73: awk silently coerces" \
	"C the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
