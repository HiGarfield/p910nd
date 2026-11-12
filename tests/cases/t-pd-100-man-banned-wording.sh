#!/bin/sh
#
# PD-100  [low]  New man-page prose must not name the removed wait interface.
#
# PD-98 documented P910ND_HAVE_MONOTONIC with the phrase "The code path
# selected when it is absent".  static_checks.py's PD-21 check scans every
# shipped text file for the old fixed-bit-set interface name, as a plain
# substring: "selected" contains "select", so the full suite turned red with
# "no-banned-text DEFECT p910nd.8:320".  The wording, not the code, was at
# fault, but CI treats that check as a hard gate.
#
# A: the TUNING section contains no occurrence of the banned substring.
# B: PD-21's checker reports all four checks OK.
# C: control -- the newly documented entries are still present, so the
#    finding was not "fixed" by deleting the prose.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-100 man-banned-wording

MAN="$REPO_ROOT/p910nd.8"

say "A: no banned substring in the tuning prose"
# Build the token the same way static_checks.py does, so this file cannot
# trip its own scan.
BANNED=$("$PYTHON" -c 'print("sel" + "ect")')
"$PYTHON" - "$MAN" "$BANNED" >"$CASE_DIR/a.out" 2>&1 <<'EOF'
import sys
text = open(sys.argv[1], encoding="utf-8").read()
banned = sys.argv[2]
tuning = text[text.index('.SH "TUNING AT BUILD TIME"'):]
hits = ["%d:%s" % (i, l.strip()) for i, l in
        enumerate(tuning.splitlines(), start=1) if banned in l.lower()]
print("banned=%s hits=%d" % (banned, len(hits)))
for h in hits:
    print("hit: " + h)
sys.exit(0 if not hits else 1)
EOF
assert_eq "$?" "0" \
	"A the tuning section is free of the removed interface name"
assert_contains "$CASE_DIR/a.out" "hits=0" \
	"A the scan really found nothing"

say "B: the PD-21 checker is green"
"$PYTHON" "$TOOLS_DIR/static_checks.py" --check PD-21 \
	>"$CASE_DIR/b.out" 2>&1
sed 's/^/    /' "$CASE_DIR/b.out" | while read -r l; do obs "$l"; done
assert_contains "$CASE_DIR/b.out" "no-banned-text  *OK" \
	"B no shipped file names the old interface"
assert_contains "$CASE_DIR/b.out" "SUMMARY checks=4 ok=4 not-ok=0" \
	"B and every PD-21 assertion is OK"

say "C: control -- the new documentation is intact"
for k in SOMAXCONN P910ND_HAVE_MONOTONIC; do
	assert_contains "$MAN" ".B $k" \
		"C the .B entry for $k is still present"
done
assert_contains "$MAN" "step-compensated" \
	"C and its explanation was not deleted"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
