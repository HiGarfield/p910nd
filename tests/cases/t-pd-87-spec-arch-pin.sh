#!/bin/sh
#
# PD-87  [medium]  The RPM spec must not exclude the architectures CI builds.
#
# aux/p910nd.spec carried `BuildArchitectures: i386 x86_64`, an obsolete
# hard-coded list.  .github/workflows/CI.yml cross-builds 34 targets
# (aarch64, arm, mips, powerpc, riscv64, s390x, ...), but rpmbuild was told
# the package only ever targets i386/x86_64, so an RPM build on any other
# host could not produce a usable package.  rpmbuild already infers the
# target architecture when no BuildArch is pinned.
#
# A: the CI architecture matrix is really parsed and no pinned list excludes
#    its non-x86 members.
# B: static -- no BuildArchitectures line remains and the reason is recorded.
# C: the %files mode rules from PD-45 are untouched.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-87 spec-arch-pin

SPEC="$REPO_ROOT/aux/p910nd.spec"
CI="$REPO_ROOT/.github/workflows/CI.yml"

say "A: no hard-coded architecture list excludes the CI matrix"
"$PYTHON" - "$SPEC" "$CI" >"$CASE_DIR/a.out" 2>&1 <<'EOF'
import re, sys
spec = open(sys.argv[1], encoding="utf-8").read()
ci = open(sys.argv[2], encoding="utf-8").read()
m = re.search(r'^[ \t]+arch:[ \t]*\n((?:[ \t]+-[ \t]+\S+[ \t]*\n)+)',
              ci, re.M)
arches = re.findall(r'^[ \t]+-[ \t]+(\S+)', m.group(1), re.M) if m else []
non_x86 = [a for a in arches if not a.startswith("x86")]
pins = re.findall(r'^BuildArchitectures:[ \t]*(.+)$', spec, re.M)
allowed = pins[0].split() if pins else None
if allowed is None:
    excluded, verdict = [], "auto-detect"
else:
    excluded = [a for a in non_x86 if a not in allowed]
    verdict = "pinned:%s" % ",".join(allowed)
print("ci_arches=%d non_x86=%d excluded=%d verdict=%s" %
      (len(arches), len(non_x86), len(excluded), verdict))
sys.exit(0 if len(arches) >= 20 and len(non_x86) >= 10 and
         allowed is None else 1)
EOF
assert_eq "$?" "0" \
	"A the CI matrix is parsed and no pinned list filters it out"
assert_contains "$CASE_DIR/a.out" "verdict=auto-detect" \
	"A rpmbuild is left to infer the target architecture"
"$PYTHON" - "$CASE_DIR/a.out" <<'EOF'
import re, sys
t = open(sys.argv[1], encoding="utf-8").read()
n = int(re.search(r'ci_arches=(\d+)', t).group(1))
k = int(re.search(r'non_x86=(\d+)', t).group(1))
sys.exit(0 if n >= 20 and k >= 10 else 1)
EOF
assert_eq "$?" "0" \
	"A the check is not vacuous: a real, mostly non-x86 matrix was read"

say "B: static -- the pin is gone and documented"
assert_not_contains "$SPEC" "BuildArchitectures:" \
	"B no obsolete hard-coded architecture tag remains"
assert_contains "$SPEC" "PD-87: do not hard-code BuildArchitectures" \
	"B the reason is recorded in the spec"

say "C: control -- %files modes are unchanged"
assert_contains "$SPEC" "%defattr(644, root, root, 755)" \
	"C regular files still ship 0644"
# BRE: \( opens a group, so match the bare directive name instead.
assert_eq "$(count_lines "$SPEC" '^%attr')" "2" \
	"C and only the two executables are 0755"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
