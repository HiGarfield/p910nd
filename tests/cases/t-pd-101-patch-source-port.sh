#!/bin/sh
#
# PD-101  [medium]  A patched variant must never listen on a wrapped port.
#
# tests/tools/patch_source.py took --port as a bare int.  server() builds its
# service string from (unsigned short)(BASEPORT + lpnumber - '0'), so values
# outside 1..65533 compiled cleanly (the explicit cast hides the wrap) but
# made every test variant listen somewhere else:
#   -1 -> 65535, 0 -> 0, 65536 -> 0, 99999 -> 34463
# All cases then died in wait_for_port() with no hint that the BUILD was
# wrong.  TEST_PORT is an environment variable, so a typo or a bad CI value
# reached this path directly.  Measured on the unfixed tool: all four values
# exited 0 and wrote "BASEPORT -1" / "BASEPORT 99999" into the source copy.
#
# A: out-of-range ports are refused and the source copy stays untouched.
# B: valid ports are still patched correctly.
# C: static -- validation happens before the file is opened for writing.
# D: the fast profile uses the same guard and still applies every patch.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-101 patch-source-port

TOOL="$TOOLS_DIR/patch_source.py"
SRC="$REPO_ROOT/p910nd.c"
COPY="$CASE_DIR/copy.c"

say "A: invalid ports are refused without touching the copy"
for p in -1 0 65534 65535 65536 99999; do
	cp "$SRC" "$COPY"
	"$PYTHON" "$TOOL" --profile ports --port "$p" "$COPY" \
		>"$CASE_DIR/reject-$p.out" 2>&1
	_rc=$?
	obs "port $p rc=$_rc stderr: $(tr '\n' '|' <"$CASE_DIR/reject-$p.out")"
	[ "$_rc" -ne 0 ]
	assert_true "$?" "A port $p is rejected with a nonzero status"
	if cmp -s "$SRC" "$COPY"; then
		_untouched=0
	else
		_untouched=1
	fi
	assert_eq "$_untouched" "0" \
		"A port $p leaves the throw-away source copy pristine"
done

say "B: valid ports still produce the right BASEPORT"
for p in 1 19100 65533; do
	cp "$SRC" "$COPY"
	"$PYTHON" "$TOOL" --profile ports --port "$p" "$COPY" \
		>"$CASE_DIR/accept-$p.out" 2>&1
	assert_eq "$?" "0" "B port $p is accepted"
	assert_contains "$COPY" "BASEPORT $p" \
		"B port $p is written into the copy"
	assert_not_contains "$COPY" "BASEPORT 9100" \
		"B and the original port is gone"
done

say "C: static -- the guard runs before any write"
"$PYTHON" - "$TOOL" >"$CASE_DIR/c.out" 2>&1 <<'EOF'
import sys
s = open(sys.argv[1], encoding="utf-8").read()
has_fn = "def validate_port(port):" in s
has_ceiling = "65535 - 2" in s
called = "validate_port(args.port)" in s
write_at = s.find('open(args.source, "w"')
call_at = s.find("validate_port(args.port)")
print("has_fn=%s has_ceiling=%s called=%s before_write=%s" %
      (has_fn, has_ceiling, called, 0 <= call_at < write_at))
sys.exit(0 if has_fn and has_ceiling and called
         and 0 <= call_at < write_at else 1)
EOF
assert_eq "$?" "0" \
	"C the check exists, covers lp+2 and precedes the write"
assert_contains "$CASE_DIR/c.out" \
	"has_fn=True has_ceiling=True called=True before_write=True" \
	"C every part of the guard was checked"
assert_contains "$TOOL" "PD-101: --port used to be an unchecked int" \
	"C the reason is documented"

say "D: the fast profile shares the guard and still patches"
cp "$SRC" "$COPY"
"$PYTHON" "$TOOL" --profile fast --port 99999 "$COPY" \
	>"$CASE_DIR/fast-bad.out" 2>&1
[ "$?" -ne 0 ]
assert_true "$?" "D fast rejects the same bad port"
cp "$SRC" "$COPY"
"$PYTHON" "$TOOL" --profile fast --port 19100 "$COPY" \
	>"$CASE_DIR/fast-ok.out" 2>&1
assert_eq "$?" "0" "D fast accepts a good port"
obs "fast patch lines: $(count_lines "$CASE_DIR/fast-ok.out" '->')"
assert_eq "$(count_lines "$CASE_DIR/fast-ok.out" '->')" "7" \
	"D all seven fast-profile patches were applied"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
