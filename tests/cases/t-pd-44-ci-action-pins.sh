#!/bin/sh
#
# PD-44  [medium]  CI actions must be pinned to immutable version tags.
#
# Every official action used @main and both third-party actions used
# @master.  Those are moving branches: an upstream push (or a compromised
# maintainer account) silently changes what the release and test jobs run,
# with no change in this repository and no review.  Two identical commits
# could also produce different binaries and different releases.
#
# Pin every action to a published version tag.  The tags used here were
# checked against the upstream repositories: actions/* v4,
# HiGarfield/action-gh-release v2, HiGarfield/delete-release-action v1.2.
#
# The check also requires the full set of steps to remain present, so
# "fixing" the finding by deleting a job cannot pass.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-44 ci-action-pins

CI="$REPO_ROOT/.github/workflows/CI.yml"
obs "CI uses: lines:"
grep -n 'uses:' "$CI" | sed 's/^/    /' | while read -r l; do obs "$l"; done

"$PYTHON" - "$CI" >"$CASE_DIR/scan.out" 2>&1 <<'EOF'
import re, sys
lines = open(sys.argv[1], encoding="utf-8").read().splitlines()
refs = []
for line in lines:
    u = re.search(r"(?:-\s*)?uses:\s*(\S+)", line)
    if u:
        refs.append(u.group(1))
mutable = [r for r in refs if re.search(r"@(main|master|HEAD|develop)$", r)]
unversioned = [r for r in refs if not re.search(r"@v\d", r)]
names = sorted(set(r.split("@")[0] for r in refs))
print("refs=%d mutable=%d unversioned=%d unique_actions=%d" %
      (len(refs), len(mutable), len(unversioned), len(names)))
for r in mutable:
    print("mutable: " + r)
for r in unversioned:
    print("unversioned: " + r)
print("actions: " + ",".join(names))
sys.exit(0 if len(refs) == 8 and not mutable and not unversioned
         and len(names) == 6 else 1)
EOF
scan_rc=$?
obs "scan output:"
sed 's/^/    /' "$CASE_DIR/scan.out" | while read -r l; do obs "$l"; done
obs_kv "scan exit status" "$scan_rc"
assert_eq "$scan_rc" "0" \
	"every uses: reference is a version tag and all eight steps remain"

assert_not_contains "$CI" "@main" "no action follows the upstream main branch"
assert_not_contains "$CI" "@master" "no action follows the upstream master branch"
assert_contains "$CI" "actions/checkout@v4" "checkout is pinned"
assert_contains "$CI" "actions/cache@v4" "the toolchain cache is pinned"
assert_contains "$CI" "actions/upload-artifact@v4" "artifact upload is pinned"
assert_contains "$CI" "actions/download-artifact@v4" "artifact download is pinned"
assert_contains "$CI" "HiGarfield/action-gh-release@v2" \
	"the release action is pinned to a published tag"
assert_contains "$CI" "HiGarfield/delete-release-action@v1.2" \
	"the release cleanup action is pinned to a published tag"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
