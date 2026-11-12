#!/bin/sh
#
# PD-63  [medium]  The documented single-case command does not work.
#
# tests/README.md told users to run "make test PD-01", but GNU make treats
# extra words as goals, not recipe arguments.  `make -n test PD-01` prints
# `sh tests/run-all.sh` (the FULL suite) and then fails with
# "No rule to make target 'PD-01'".  The defect report repeated the same
# command in two "how it was verified" notes.
#
# Fix: add a `make test-case CASE=<id>` target that really forwards the id,
# and correct both documents.
#
# A: static -- the target exists, is phony, and both documents are fixed.
# B: dry-run -- the old form is invalid and the new form forwards the id.
# C: behaviour -- the new target runs exactly one case; empty CASE is refused.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-63 single-case-command

MK="$REPO_ROOT/Makefile"
README="$TESTS_DIR/README.md"
REPORT="$TESTS_DIR/DEFECT_REPORT.md"

say "A: static -- a forwarding target and correct documentation"
assert_contains "$MK" "test-case:" \
	"A the Makefile provides a single-case target"
assert_contains "$MK" 'sh $(TESTS_DIR)/run-all.sh $(CASE)' \
	"A and forwards CASE to the driver"
assert_contains "$MK" 'test -n "$(CASE)"' \
	"A an omitted CASE is rejected"
assert_contains "$MK" "test-case" \
	"A the target is declared phony"
assert_contains "$README" "make test-case CASE=PD-01" \
	"A the README documents the working form"
assert_contains "$README" "sh tests/run-all.sh PD-01" \
	"A and the equivalent direct invocation"
assert_not_contains "$README" '^make test PD-01' \
	"A no line still instructs users to run the broken command"
assert_eq "$(count_lines "$REPORT" 'make test PD-')" "0" \
	"A the defect report no longer repeats the broken command"

say "B: dry-run -- make never forwards an extra goal"
make -C "$REPO_ROOT" -n test PD-01 >"$CASE_DIR/old.out" 2>&1
old_rc=$?
obs "old dry-run: $(tr '\n' '|' <"$CASE_DIR/old.out")"
obs_kv "old dry-run exit" "$old_rc"
[ "$old_rc" -ne 0 ]
_b_rc=$?
assert_true "$_b_rc" "B the old form is rejected by make"
assert_contains "$CASE_DIR/old.out" "sh tests/run-all.sh" \
	"B and its recipe would have run the whole suite"
assert_not_contains "$CASE_DIR/old.out" "sh tests/run-all.sh PD-01" \
	"B the case id is not forwarded"

make -C "$REPO_ROOT" -n test-case CASE=PD-15 >"$CASE_DIR/new.out" 2>&1
obs "new dry-run: $(tr '\n' '|' <"$CASE_DIR/new.out")"
assert_contains "$CASE_DIR/new.out" "sh tests/run-all.sh PD-15" \
	"B the new target forwards exactly the requested id"

say "C: behaviour -- one case runs, empty CASE is refused"
make -C "$REPO_ROOT" test-case CASE=PD-15 >"$CASE_DIR/one.out" 2>&1
one_rc=$?
obs "single-case output: $(tr '\n' '|' <"$CASE_DIR/one.out" | tail -c 350)"
obs_kv "single-case exit" "$one_rc"
assert_eq "$one_rc" "0" "C the documented single-case command succeeds"
assert_contains "$CASE_DIR/one.out" "passed : pd-15" \
	"C and runs the requested case"
assert_not_contains "$CASE_DIR/one.out" "passed : pd-00" \
	"C without running the whole suite"

make -C "$REPO_ROOT" test-case >"$CASE_DIR/empty.out" 2>&1
empty_rc=$?
obs "empty CASE output: $(tr '\n' '|' <"$CASE_DIR/empty.out")"
obs_kv "empty CASE exit" "$empty_rc"
assert_eq "$empty_rc" "2" "C omitting CASE fails with a usage error"
assert_contains "$CASE_DIR/empty.out" "usage: make test-case CASE=PD-01" \
	"C and explains the required syntax"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
