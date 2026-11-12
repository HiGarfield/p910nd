#!/bin/sh
#
# PD-95  [high]  The RPM %install stage must stage into the buildroot.
#
# aux/p910nd.spec used `%makeinstall` (no underscore).  That legacy macro
# expands to a long list of lowercase prefix/bindir/libdir/... assignments
# and does NOT pass DESTDIR; openSUSE and Fedora packaging guidelines both
# say to avoid it and spell out `make install DESTDIR=%{buildroot}` instead.
# This Makefile installs through $(DESTDIR)$(BINDIR), $(DESTDIR)$(CONFIGDIR),
# $(DESTDIR)$(SCRIPTDIR) and $(DESTDIR)$(MANDIR), so DESTDIR stayed empty:
# the files went to the build host's real /usr/sbin (or the install failed
# for an unprivileged builder) while %files looked under the buildroot and
# found nothing.
#
# A: static -- %install uses DESTDIR and no legacy macro.
# B: behaviour -- run the command from %install with a private buildroot and
#    require all four packaged paths to land inside it.
# C: control -- the Makefile really honours DESTDIR on every install line.
# D: counter-evidence -- the lowercase variables the legacy macro supplies
#    do not redirect this Makefile at all.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-95 spec-install-destdir

SPEC="$REPO_ROOT/aux/p910nd.spec"
MAKEFILE="$REPO_ROOT/Makefile"
ROOT="$CASE_DIR/root"
CMD="$CASE_DIR/install-cmd.sh"
mkdir -p "$ROOT" || exit 1

say "A: the %install stage spells out DESTDIR"
"$PYTHON" - "$SPEC" "$REPO_ROOT" "$ROOT" "$CMD" >"$CASE_DIR/a.out" 2>&1 <<'EOF'
import re, shlex, sys
spec, repo, root, cmdfile = sys.argv[1:5]
s = open(spec, encoding="utf-8").read()
m = re.search(r"^%install\s*$(.*?)(?=^%[a-z]|\Z)", s, re.S | re.M)
if not m:
    print("no %install section")
    sys.exit(1)
lines = [l.strip() for l in m.group(1).splitlines()
         if l.strip() and not l.lstrip().startswith("#")]
body = "\n".join(lines)
has_destdir = "DESTDIR=" in body
has_make = "make install" in body
legacy = "%makeinstall" in body
print("install_lines=%d has_destdir=%s has_make_install=%s legacy=%s" %
      (len(lines), has_destdir, has_make, legacy))
cmd = (body.replace("%{?buildroot}", root).replace("%{buildroot}", root))
open(cmdfile, "w", encoding="utf-8").write(
    "set -e\ncd %s\n%s\n" % (shlex.quote(repo), cmd))
# A body of zero commands would make stage B a no-op that still exits 0.
sys.exit(0 if len(lines) >= 1 and has_destdir and has_make
         and not legacy else 1)
EOF
assert_eq "$?" "0" \
	"A %install passes DESTDIR to make install"
assert_contains "$CASE_DIR/a.out" \
	"has_destdir=True has_make_install=True legacy=False" \
	"A the scanned values are the expected ones"

say "B: all four packaged files land inside the private buildroot"
sh "$CMD" >"$CASE_DIR/install.out" 2>&1
INSTALL_RC=$?
obs "install exit: $INSTALL_RC"
obs "install output: $(tail -4 "$CASE_DIR/install.out" | tr '\n' '|')"
assert_eq "$INSTALL_RC" "0" \
	"B the staged install command succeeds"
for p in usr/sbin/p910nd etc/sysconfig/p910nd etc/init.d/p910nd \
	usr/share/man/man8/p910nd.8; do
	assert_file_exists "$ROOT/$p" "B $p is staged under the buildroot"
done
assert_eq "$(find "$ROOT" -type f | wc -l | tr -d ' ')" "4" \
	"B exactly the four packaged files were staged"
obs "staged files: $(cd "$ROOT" && find . -type f | sort | tr '\n' ' ')"

say "C: the Makefile honours DESTDIR on every install line"
"$PYTHON" - "$MAKEFILE" >"$CASE_DIR/c.out" 2>&1 <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r"^install:.*?$(.*?)(?=^\.PHONY|\Z)", s, re.S | re.M)
lines = [l.strip() for l in m.group(1).splitlines()
         if l.strip() and not l.startswith("#")]
installs = [l for l in lines if "$(INSTALL)" in l]
covered = [l for l in installs if "$(DESTDIR)" in l]
print("recipe_lines=%d install_lines=%d destdir_lines=%d" %
      (len(lines), len(installs), len(covered)))
sys.exit(0 if len(installs) == 4 and len(covered) == 4 else 1)
EOF
assert_eq "$?" "0" \
	"C every install line uses DESTDIR"
assert_contains "$CASE_DIR/c.out" "install_lines=4 destdir_lines=4" \
	"C the count was really checked"

say "D: legacy lowercase variables do not redirect this Makefile"
(cd "$REPO_ROOT" && make -n install \
	prefix="$ROOT/usr" bindir="$ROOT/usr/sbin" \
	mandir="$ROOT/usr/share/man" >"$CASE_DIR/d.out" 2>&1)
obs "legacy-style make -n tail: $(tail -2 "$CASE_DIR/d.out" | tr '\n' '|')"
"$PYTHON" - "$CASE_DIR/d.out" "$ROOT" >"$CASE_DIR/d-scan.out" 2>&1 <<'EOF'
import re, sys
text = open(sys.argv[1], encoding="utf-8").read()
root = sys.argv[2]
# The legacy macro's lowercase overrides never reach $(BINDIR) etc., so even
# the dry-run targets the real system directories.
plain = [l for l in text.splitlines()
         if re.search(r"install\s+(-m\s+\d+\s+)?\S+\s+/", l)]
print("install_commands=%d outside_buildroot=%d" %
      (len(plain), sum(1 for l in plain if root not in l)))
sys.exit(0 if plain and all(root not in l for l in plain) else 1)
EOF
assert_eq "$?" "0" \
	"D without DESTDIR the same Makefile targets the host paths"
assert_not_contains "$SPEC" "%makeinstall" \
	"D and the spec no longer relies on that macro"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
