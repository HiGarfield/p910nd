#!/bin/sh
#
# PD-13  [低]  The dead variable lock_held must be gone.
#
# T6 changed the lock file from "unlink it on exit" to "leave it alone", which
# removed the only reason lock_held existed (knowing whether this process may
# delete the file).  The four assignments stayed behind, so the code still
# claimed to track an ownership it never consulted.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-13 lock-held-dead

"$PYTHON" "$TOOLS_DIR/static_checks.py" --check PD-13 >"$CASE_DIR/check.out" 2>&1
sed 's/^/    /' "$CASE_DIR/check.out" | while read -r l; do obs "$l"; done

assert_contains "$CASE_DIR/check.out" "lock-held-removed  *OK" \
	"the dead variable and its four assignments are gone"

obs "lock_held occurrences in the source (comments excluded by the check):"
if grep -n 'lock_held' "$REPO_ROOT/p910nd.c" | sed 's/^/    /' | while read -r l; do obs "$l"; done; then
	obs_kv "raw occurrence count" "$(grep -c 'lock_held' "$REPO_ROOT/p910nd.c" || true)"
else
	obs_kv "raw occurrence count" "0"
	obs "    (none: the identifier does not appear at all)"
fi
assert_eq "$(grep -c 'lock_held' "$REPO_ROOT/p910nd.c" || true)" "0" \
	"lock_held does not appear in p910nd.c any more"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
