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

# PD-105: run the mutated cases in a throw-away copy of the tree.  Every
# case's case_init() truncates $OBS_DIR/<id>.txt and rebuilds
# $BUILD_DIR/cases/<id>/, so running them in the live tree overwrote the
# observations of the last `make test` with deliberately failing mutations:
# after a fully green suite PD-05/PD-07/PD-09 contained ASSERT FAIL lines.
# CI archiving build/tests/ then reported failures that never happened.
TREE="$WORK/tree"
rm -rf "$TREE"
mkdir -p "$TREE" || exit 1
(cd "$REPO_ROOT" && tar cf - --exclude=.git --exclude=build .) |
	(cd "$TREE" && tar xf -) || exit 1

# Sticky failure flag: each mutation may report a missed detection, and a
# later, successful mutation must not erase it.  RESULT used to be assigned in
# both branches, so the last mutation decided the whole run.
RESULT=0

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
# The READ_ERR exit became a two-line if statement when the source was
# clang-formatted, so deleting the matching line left a stray
# "bytes == 0)" behind: the copy no longer compiled, the harness kept its
# previous binary and the case passed vacuously.  Mask the flag test to a
# value that can never be true instead - the copy still builds and only
# the fix is disabled, which is what a mutation has to do.
m=$(revert pd05 's/printerToNetworkBuffer.err & READ_ERR/printerToNetworkBuffer.err \& 0/') || exit 1
if (cd "$TREE" && P910ND_SRC="$m" sh tests/cases/t-pd-05-bidir-read-error.sh) >"$WORK/pd05.log" 2>&1; then
	say "FAIL: the case still passes with the fix reverted"
	RESULT=1
else
	say "ok: the case fails without the fix"
fi

say "2/3  PD-09: ask for the world writable lock file again"
m=$(revert pd09 's/O_RDWR | O_NOFOLLOW, 0644/O_RDWR | O_NOFOLLOW, 0666/') || exit 1
if (cd "$TREE" && P910ND_SRC="$m" sh tests/cases/t-pd-09-lockfile-mode.sh) >"$WORK/pd09.log" 2>&1; then
	say "FAIL: the case still passes with the fix reverted"
	RESULT=1
else
	say "ok: the case fails without the fix"
fi

say "3/3  PD-07: force the wall clock in the default build"
m=$(revert pd07 's/^#if !defined(NO_CLOCK_GETTIME)/#if 0 \&\& !defined(NO_CLOCK_GETTIME)/') || exit 1
if (cd "$TREE" && P910ND_SRC="$m" sh tests/cases/t-pd-07-wallclock-timeout.sh) >"$WORK/pd07.log" 2>&1; then
	say "FAIL: the case still passes with the fix reverted"
	RESULT=1
else
	say "ok: the case fails without the fix"
fi

kill_stray
# Also sweep the isolated tree's process group namespace.
(cd "$TREE" && REPO_ROOT="$TREE" . ./tests/lib.sh && kill_stray)
say ""
[ "$RESULT" -eq 0 ] && say "all mutations were detected" || say "at least one mutation went unnoticed"
exit "$RESULT"
