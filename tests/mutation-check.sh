#!/bin/sh
#
# Sensitivity check: undo a fix in a *copy* of p910nd.c and confirm the matching
# case notices.  A regression test that cannot fail is worse than none, so this
# is run for the fixes whose behaviour is easiest to get subtly wrong.
#
#   sh tests/mutation-check.sh
#
# BASELINE_SRC is honoured by tests/variants.sh, so the case under test builds
# its "shipping" variant from the mutated copy.  p910nd.c itself is untouched.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

WORK="$BUILD_DIR/mutation"
mkdir -p "$WORK"

# revert <name> <sed-program> -- write a mutated copy and echo its path
revert() {
	_name=$1
	_prog=$2
	cp "$REPO_ROOT/p910nd.c" "$WORK/$_name.c"
	sed -i "$_prog" "$WORK/$_name.c" || return 1
	if cmp -s "$REPO_ROOT/p910nd.c" "$WORK/$_name.c"; then
		printf 'mutation %s did not change anything\n' "$_name" >&2
		return 1
	fi
	printf '%s\n' "$WORK/$_name.c"
}

say "1/3  PD-05: put the printer read-error exit back out"
m=$(revert pd05 '/printerToNetworkBuffer.err & READ_ERR/d') || exit 1
if P910ND_SRC="$m" sh "$CASES_DIR/t-pd-05-bidir-read-error.sh" >"$WORK/pd05.log" 2>&1; then
	say "FAIL: the case still passes with the fix reverted"
	RESULT=1
else
	say "ok: the case fails without the fix"
	RESULT=0
fi

say "2/3  PD-09: ask for the world writable lock file again"
m=$(revert pd09 's/O_RDWR | O_NOFOLLOW, 0644/O_RDWR | O_NOFOLLOW, 0666/') || exit 1
if P910ND_SRC="$m" sh "$CASES_DIR/t-pd-09-lockfile-mode.sh" >"$WORK/pd09.log" 2>&1; then
	say "FAIL: the case still passes with the fix reverted"
	RESULT=1
else
	say "ok: the case fails without the fix"
	RESULT=0
fi

say "3/3  PD-07: force the wall clock in the default build"
m=$(revert pd07 's/^#if !defined(NO_CLOCK_GETTIME)/#if 0 \&\& !defined(NO_CLOCK_GETTIME)/') || exit 1
if P910ND_SRC="$m" sh "$CASES_DIR/t-pd-07-wallclock-timeout.sh" >"$WORK/pd07.log" 2>&1; then
	say "FAIL: the case still passes with the fix reverted"
	RESULT=1
else
	say "ok: the case fails without the fix"
	RESULT=0
fi

kill_stray
say ""
[ "$RESULT" -eq 0 ] && say "all mutations were detected" || say "at least one mutation went unnoticed"
exit "$RESULT"
