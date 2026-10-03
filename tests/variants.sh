#!/bin/sh
#
# Variant builds for the defect verification cases.
#
# p910nd.c is never modified.  Variants that need a different constant are
# built from a throw-away copy in a temporary directory which is deleted again
# as soon as the compiler is done (see patched_source()/build_variant()).
#
# Three flavours exist:
#
#   p910nd-baseline        exactly what `make` produces (default macros, no
#                          LOCKFILE_DIR) -- the reference for "this is what
#                          ships".
#   p910nd-<flavour>       pristine p910nd.c plus a single -D, so any behaviour
#                          difference is attributable to that one -D.
#   p910nd-test*           a patched copy of p910nd.c: BASEPORT is moved off the
#                          real 9100-9102 range and the wall-clock timeouts are
#                          scaled down so a case finishes in seconds.  A copy
#                          whose name contains "test" is always built this way.

# CFLAGS_BASE mirrors the Makefile so a variant behaves like a normal build.
CFLAGS_BASE="-O2 -Wall -Wextra -std=c89 -pedantic -Wdeclaration-after-statement"

# P910ND_SRC replaces the source every variant is built from.  It exists so a
# fix can be verified without editing p910nd.c: copy the tree, revert one hunk in
# the copy, point the harness at it and check that the matching case fails (see
# tests/mutation-check.sh).  p910nd.c itself is never written to.
P910ND_SRC=${P910ND_SRC:-$REPO_ROOT/p910nd.c}

# TEST_PORT is where the patched variants listen.  It is deliberately far away
# from the real 9100+n range so a test can never talk to a production printer.
TEST_PORT=${TEST_PORT:-19100}

PATCH_LOG=""

# patched_source -- copy p910nd.c to a temp dir, apply the test profile patches
# to the copy, echo the copy's path.  The caller owns the copy and must call
# drop_source() on it.
patched_source() {
	_dir=$(mktemp -d "$BUILD_DIR/src.XXXXXX") || return 1
	cp "$P910ND_SRC" "$_dir/p910nd.c" || return 1
	if ! "$PYTHON" "$TOOLS_DIR/patch_source.py" --profile fast \
		--port "$TEST_PORT" --log "${PATCH_LOG:-/dev/null}" \
		"$_dir/p910nd.c" >>"${PATCH_LOG:-/dev/null}" 2>&1; then
		printf 'patch_source.py failed:\n' >&2
		cat "${PATCH_LOG:-/dev/null}" >&2 2>/dev/null
		rm -rf "$_dir"
		return 1
	fi
	printf '%s\n' "$_dir/p910nd.c"
}

# drop_source <path-to-copied-source> -- remove the throw-away copy.
drop_source() {
	[ -n "$1" ] && rm -rf "$(dirname "$1")"
	return 0
}

# build_variant <name> [extra cc flags...]
# Echoes the path of the built binary.  Names containing "test" are built from
# a patched copy; everything else from the pristine source.
build_variant() {
	_name=$1
	shift
	_out="$BUILD_DIR/$_name"
	_log="$BUILD_DIR/$_name.build.log"
	case "$_name" in
	*test*)	_src=$(patched_source) || return 1 ;;
	*)	_src="$REPO_ROOT/p910nd.c" ;;
	esac
	# shellcheck disable=SC2086
	if $CC_BIN -o "$_out" "$_src" $CFLAGS_BASE "$@" >"$_log" 2>&1; then
		case "$_name" in
		*test*) drop_source "$_src" ;;
		esac
		chmod +x "$_out"
		printf '%s\n' "$_out"
		return 0
	fi
	printf 'build of variant %s failed:\n' "$_name" >&2
	cat "$_log" >&2
	case "$_name" in
	*test*) drop_source "$_src" ;;
	esac
	return 1
}

# build_baseline -- build the shipping configuration with the real Makefile and
# copy the result into build/tests.  This is the only variant whose provenance
# is "make", which is what makes it usable as the reference for PD-01.
#
# The baseline variant is the one the Makefile itself built, so it is the
# reference for "what ships".  With P910ND_SRC set it is built from that copy
# instead, which is how a fix round is checked.
build_baseline() {
	if [ "$P910ND_SRC" != "$REPO_ROOT/p910nd.c" ]; then
		_log="$BUILD_DIR/baseline.build.log"
		# shellcheck disable=SC2086
		if ! $CC_BIN -o "$BUILD_DIR/p910nd-baseline" "$P910ND_SRC" \
			$CFLAGS_BASE >"$_log" 2>&1; then
			printf 'baseline build from %s failed:\n' "$P910ND_SRC" >&2
			cat "$_log" >&2
			return 1
		fi
		chmod +x "$BUILD_DIR/p910nd-baseline"
		obs_kv "baseline source" "$P910ND_SRC (not p910nd.c)"
		printf '%s\n' "$BUILD_DIR/p910nd-baseline"
		return 0
	fi
	if ! make -C "$REPO_ROOT" p910nd >"$BUILD_DIR/baseline.build.log" 2>&1; then
		printf 'make failed:\n' >&2
		cat "$BUILD_DIR/baseline.build.log" >&2
		return 1
	fi
	cp "$REPO_ROOT/p910nd" "$BUILD_DIR/p910nd-baseline" || return 1
	chmod +x "$BUILD_DIR/p910nd-baseline"
	obs_kv "baseline build log warnings" \
		"$(grep -c 'warning:' "$BUILD_DIR/baseline.build.log")"
	printf '%s\n' "$BUILD_DIR/p910nd-baseline"
}

# build_all -- build the variants that more than one case needs.  Returns 0
# when every requested variant was produced.
build_all() {
	_rc=0
	for _v in "$@"; do
		case "$_v" in
		baseline)
			build_baseline >/dev/null || _rc=1
			;;
		*)
			build_variant "$_v" >/dev/null || _rc=1
			;;
		esac
	done
	return $_rc
}
