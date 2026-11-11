#!/bin/sh
#
# PD-33  [low]  A build must not dirty the working tree.
#
# .gitignore listed build/, *.o and *.d but not the p910nd binary that
# `make` writes in the repository root.  Every build therefore left an
# untracked executable behind (visible in `git status`), which is easy to
# commit by accident and hides real local changes.
#
# The case asks git itself about each artifact and then does a real build in
# an isolated checkout, asserting that `git status --porcelain` stays empty.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-33 ignore-build-artifacts

IGNORE="$REPO_ROOT/.gitignore"
assert_contains "$IGNORE" "/p910nd" \
	"the ignore file names the root binary explicitly"

for path in p910nd p910nd.d build/tests/test probe.o; do
	if git -C "$REPO_ROOT" check-ignore -q --no-index "$path"; then
		obs "ignored: $path"
		assert_true 0 "git ignores $path"
	else
		obs "NOT ignored: $path"
		assert_true 1 "git ignores $path"
	fi
done

# Integration half: a pristine checkout, a real make, a clean tree.
WORK="$CASE_DIR/checkout"
mkdir -p "$WORK"
cp "$REPO_ROOT/.gitignore" "$REPO_ROOT/Makefile" "$REPO_ROOT/p910nd.c" \
	"$WORK/"
if (cd "$WORK" && git init -q &&
	git add .gitignore Makefile p910nd.c &&
	git -c user.email=test@example.com -c user.name=test commit -qm init &&
	make p910nd >"$CASE_DIR/make.log" 2>&1); then
	dirty=$(cd "$WORK" && git status --porcelain)
	obs_kv "untracked files after make" "${dirty:-none}"
	assert_eq "${dirty:-}" "" \
		"make leaves no untracked build artifacts behind"
else
	obs "isolated build failed:"
	sed 's/^/    /' "$CASE_DIR/make.log" | while read -r l; do obs "$l"; done
	assert_true 1 "the isolated checkout builds"
fi

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
