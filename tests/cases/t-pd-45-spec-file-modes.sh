#!/bin/sh
#
# PD-45  [medium]  The RPM must not ship data files as executables.
#
# aux/p910nd.spec declared %defattr(755, root, root) with no per-file
# overrides.  RPM applies that default to every entry in %files, so the
# config file /etc/sysconfig/p910nd and the compressed man page were
# packaged world-executable (0755), even though the Makefile installs
# them 0644.  Only /etc/init.d/p910nd and /usr/sbin/p910nd are programs.
#
# rpmbuild is not available in the test environment, so the case parses
# the %files section with the same %defattr/%attr precedence RPM uses and
# computes the effective mode of every packaged path.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-45 spec-file-modes

SPEC="$REPO_ROOT/aux/p910nd.spec"
obs "%files section:"
sed -n '/^%files/,/^%changelog/p' "$SPEC" | sed 's/^/    /' |
	while read -r l; do obs "$l"; done

"$PYTHON" - "$SPEC" >"$CASE_DIR/modes.out" 2>&1 <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r"^%files\s*$(.*?)(?=^%changelog|\Z)", s, re.S | re.M)
if not m:
    print("no %files section")
    sys.exit(1)
default = None
dirmode = None
entries = []
for line in m.group(1).splitlines():
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    d = re.match(r"%defattr\(([^)]*)\)", line)
    if d:
        parts = [p.strip() for p in d.group(1).split(",")]
        default = parts[0]
        dirmode = parts[3] if len(parts) > 3 else None
        continue
    a = re.match(r"%attr\(([^)]*)\)\s*(.+)", line)
    if a:
        parts = [p.strip() for p in a.group(1).split(",")]
        entries.append((a.group(2).strip(), parts[0]))
        continue
    if line.startswith("/") or line.startswith("%{"):
        entries.append((line, default))

print("default_file_mode=%s dir_mode=%s entries=%d" %
      (default, dirmode, len(entries)))
bad = []
for path, mode in entries:
    print("%s %s" % (mode, path))
    want = "755" if ("/usr/sbin/" in path or "/etc/init.d/" in path) else "644"
    if mode != want:
        bad.append((path, mode, want))
for path, mode, want in bad:
    print("wrong: %s mode=%s want=%s" % (path, mode, want))
ok = (default == "644" and dirmode == "755" and len(entries) == 4
      and not bad)
sys.exit(0 if ok else 1)
EOF
scan_rc=$?
obs "computed modes:"
sed 's/^/    /' "$CASE_DIR/modes.out" | while read -r l; do obs "$l"; done
obs_kv "mode scan exit" "$scan_rc"
assert_eq "$scan_rc" "0" \
	"the packaged config and man page are 0644, programs are 0755"
assert_contains "$CASE_DIR/modes.out" "default_file_mode=644 dir_mode=755 entries=4" \
	"the default and the four packaged paths are explicit"
assert_contains "$CASE_DIR/modes.out" "644 /etc/sysconfig/p910nd" \
	"the config file is not executable"
assert_contains "$CASE_DIR/modes.out" "644 %{_mandir}/man8/p910nd.8.gz" \
	"the man page is not executable"
assert_contains "$CASE_DIR/modes.out" "755 /usr/sbin/p910nd" \
	"the daemon stays executable"
assert_contains "$CASE_DIR/modes.out" "755 /etc/init.d/p910nd" \
	"the init script stays executable"
# BRE: \( opens a group, so match the bare directive name instead.
assert_eq "$(count_lines "$SPEC" '^%attr')" "2" \
	"only the two programs need an explicit mode override"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
