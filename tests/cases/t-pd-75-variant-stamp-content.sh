#!/bin/sh
#
# PD-75  [high]  Shared variants must rebuild when the source changes.
#
# ensure_shared_variants() wrote only P910ND_SRC (a path) into
# build/tests/.variants.stamp.  Editing p910nd.c -- or overwriting a
# mutation fixture at the same path -- left the stamp valid, so running a
# single case on its own compiled nothing and exercised the PREVIOUS binary.
# A source change could therefore be reported green without ever being
# compiled, which is the exact opposite of what the harness is for.
#
# The stamp must include a checksum of the source bytes.
#
# A: a marker appended to the source at the same path appears in the binary
#    after ensure_shared_variants() runs again.
# B: an unchanged source still reuses the cached binary (no rebuild churn).
# C: static -- the stamp carries a checksum.
#
# The case restores the normal source/variants at the end so the rest of the
# suite is unaffected.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-75 variant-stamp-content

LIB="$TESTS_DIR/lib.sh"
SRC="$CASE_DIR/src/p910nd.c"
mkdir -p "$CASE_DIR/src"
cp "$REPO_ROOT/p910nd.c" "$SRC" || exit 1

marker_present() {
	# The global function survives -O2 in a non-stripped binary's symbol
	# table; grep -a avoids requiring nm/binutils on the host.
	grep -a -q 'pd75_probe_marker' "$BUILD_DIR/test" 2>/dev/null
}

say "A: source edits at an unchanged path force a rebuild"
P910ND_SRC="$SRC"
ensure_shared_variants
assert_eq "$?" "0" "A the first shared build succeeds"
if marker_present; then
	obs "A before edit: marker already present (unexpected)"
	assert_true 1 "A the pristine copy has no marker"
else
	assert_true 0 "A the pristine copy has no marker"
fi
before_sig=$(cksum <"$SRC")
obs_kv "source checksum before edit" "$before_sig"

# A global function so the compiler cannot discard it; C89 needs no prior
# declaration for a definition.
printf '\nint pd75_probe_marker(void) { return 75; }\n' >>"$SRC"
after_sig=$(cksum <"$SRC")
obs_kv "source checksum after edit" "$after_sig"
assert_str_contains "$before_sig" " " "A the checksum command works"
if [ "$before_sig" = "$after_sig" ]; then
	assert_true 1 "A the edit really changed the source bytes"
else
	assert_true 0 "A the edit really changed the source bytes"
fi

ensure_shared_variants
assert_eq "$?" "0" "A the rebuild after the edit succeeds"
if marker_present; then
	assert_true 0 "A the rebuilt binary contains the new source"
else
	assert_true 1 "A the rebuilt binary contains the new source"
fi

say "B: an unchanged source still reuses the cached binary"
cached_sig=$(cksum <"$BUILD_DIR/test")
ensure_shared_variants
assert_eq "$?" "0" "B the second call succeeds"
unchanged_sig=$(cksum <"$BUILD_DIR/test")
obs_kv "binary checksum cached" "$cached_sig"
obs_kv "binary checksum reused" "$unchanged_sig"
assert_eq "$unchanged_sig" "$cached_sig" \
	"B no needless rebuild happens when the source is unchanged"

say "C: static -- the stamp includes the source bytes"
assert_contains "$LIB" 'cksum <"$P910ND_SRC"' \
	"C the source checksum is computed"
assert_contains "$LIB" '_want="$P910ND_SRC|$_sig"' \
	"C and folded into the stamp alongside the path"
assert_contains "$LIB" '_want" >"$_stamp"' \
	"C the stamp written is the checksum-bearing value"
assert_contains "$LIB" "PD-75: the stamp used to contain only" \
	"C the reason is documented"

# Restore the normal variants: later cases must not inherit a binary built
# from a mutated copy, and the stamp must be back in its usual state.
# shellcheck disable=SC2034  # read inside ensure_shared_variants()
P910ND_SRC="$REPO_ROOT/p910nd.c"
ensure_shared_variants
restore_rc=$?
assert_eq "$restore_rc" "0" "the normal shared variants are restored"
if marker_present; then
	assert_true 1 "the restored binary no longer contains the marker"
else
	assert_true 0 "the restored binary no longer contains the marker"
fi

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
