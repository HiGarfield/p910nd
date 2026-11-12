#!/bin/sh
#
# PD-98  [low]  Every new build-time guard must be documented.
#
# PD-96 added `#ifndef SOMAXCONN` and PD-97 added
# `#ifndef P910ND_HAVE_MONOTONIC` to p910nd.c without adding the names to
# p910nd.8.  PD-14 scans every #ifndef in the source and requires the man
# page to mention it; its own shell scan and static_checks.py both turned
# red on the full suite (2 undocumented names / knobs-documented DEFECT).
# Documenting a guard is part of adding it, not a follow-up.
#
# A: a fresh scan of every #ifndef finds zero undocumented names.
# B: the two new guards each have their own .B entry with real semantics.
# C: the PD-14 checker reports knobs-documented OK.
# D: control -- the pre-existing entries are still in the section.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-98 new-knobs-documented

SRC="$REPO_ROOT/p910nd.c"
MAN="$REPO_ROOT/p910nd.8"

say "A: every build-time guard appears in the man page"
"$PYTHON" - "$SRC" "$MAN" >"$CASE_DIR/a.out" 2>&1 <<'EOF'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
man = open(sys.argv[2], encoding="utf-8").read()
# Match the case's own rule: every top-level #ifndef <UPPER_NAME>.
knobs = sorted(set(re.findall(r"^#ifndef\s+([A-Z][A-Z0-9_]*)", src, re.M)))
knobs = [k for k in knobs if k != "TESTING"]
missing = [k for k in knobs if k not in man]
print("knobs=%d missing=%s" % (len(knobs), ",".join(missing) or "none"))
print("names=" + ",".join(knobs))
sys.exit(0 if len(knobs) >= 20 and not missing else 1)
EOF
assert_eq "$?" "0" \
	"A no #ifndef is undocumented"
assert_contains "$CASE_DIR/a.out" "missing=none" \
	"A the scan really found zero gaps"

say "B: the two new guards have their own .B entries"
"$PYTHON" - "$MAN" >"$CASE_DIR/b.out" 2>&1 <<'EOF'
import re, sys
man = open(sys.argv[1], encoding="utf-8").read()
tuning = man[man.index('.SH "TUNING AT BUILD TIME"'):]
blocks = re.split(r"^\.TP$", tuning, flags=re.M)
found = {}
for b in blocks:
    m = re.search(r"^\.B\s+(\S+)", b, re.M)
    if m:
        found[m.group(1)] = b
for name, word in (("SOMAXCONN", "backlog"),
                   ("P910ND_HAVE_MONOTONIC", "fallback")):
    print("%s entry=%s explains_%s=%s" %
          (name, name in found, word,
           word in found.get(name, "").lower()))
ok = all(name in found and word in found[name].lower()
         for name, word in (("SOMAXCONN", "backlog"),
                            ("P910ND_HAVE_MONOTONIC", "fallback")))
sys.exit(0 if ok else 1)
EOF
assert_eq "$?" "0" \
	"B both entries exist and explain what the guard does"
obs "B detail: $(tr '\n' '|' <"$CASE_DIR/b.out")"

say "C: the PD-14 checker agrees"
"$PYTHON" "$TOOLS_DIR/static_checks.py" --check PD-14 \
	>"$CASE_DIR/c.out" 2>&1
sed 's/^/    /' "$CASE_DIR/c.out" | while read -r l; do obs "$l"; done
assert_contains "$CASE_DIR/c.out" "knobs-documented  *OK" \
	"C static_checks.py reports no undocumented knob"

say "D: control -- the section was not emptied to pass"
for k in NO_CLOCK_GETTIME MAX_JOB_CHILDREN LOCK_WAIT_TIMEOUT \
	PRINTERFILE; do
	assert_contains "$MAN" ".B $k" \
		"D the pre-existing .B entry for $k is still there"
done
obs "TUNING entries: $(grep -c '^\.B ' "$MAN") .B lines in the whole page"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
