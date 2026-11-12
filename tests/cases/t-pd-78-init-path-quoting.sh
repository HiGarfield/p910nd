#!/bin/sh
#
# PD-78  [medium]  Binary and config paths must be quoted in the init script.
#
# `test -x $P910ND_BIN`, `test -r $P910ND_CONFIG` and `. $P910ND_CONFIG`
# word-split a path containing spaces.  Measured with a perfectly legal
# quoted assignment P910ND_BIN="/tmp/.../path with spaces/p910nd":
#   line 121: test: too many arguments
#   ".../p910nd not installed", exit 5
# so a working installation was reported as "program not installed" and the
# config file was never sourced.  The start/stop/status branches passed the
# unquoted binary to start_daemon, start-stop-daemon, killproc and checkproc
# as well.  P910ND_OPTS/NUM must stay unquoted (they are option lists).
#
# A: a spaced path is accepted and the config really gets sourced.
# B: control -- an ordinary path behaves the same.
# C: the start branch executes the binary from the spaced path.
# D: static -- no bare path usage remains, option lists still split.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-78 init-path-quoting

INIT="$REPO_ROOT/aux/p910nd.init"

make_init() {
	_out=$1
	_bin=$2
	_conf=$3
	"$PYTHON" - "$INIT" "$_out" "$_bin" "$_conf" <<'EOF'
import sys
src, out, b, c = sys.argv[1:5]
s = open(src, encoding="utf-8").read()
assert s.count("P910ND_BIN=/usr/sbin/p910nd") == 1
assert s.count("P910ND_CONFIG=/etc/sysconfig/p910nd") == 1
s = s.replace("P910ND_BIN=/usr/sbin/p910nd", 'P910ND_BIN="%s"' % b)
s = s.replace("P910ND_CONFIG=/etc/sysconfig/p910nd", 'P910ND_CONFIG="%s"' % c)
open(out, "w", encoding="utf-8").write(s)
EOF
	chmod +x "$_out"
}

make_fixture() {
	_dir=$1
	_tag=$2
	mkdir -p "$_dir"
	_BIN="$_dir/p910nd"
	_CONF="$_dir/p910nd.conf"
	_CONF_MARK="$CASE_DIR/$_tag.conf-loaded"
	_START_MARK="$CASE_DIR/$_tag.started"
	cat >"$_BIN" <<EOF
#!/bin/sh
: > "$_START_MARK"
exit 0
EOF
	chmod +x "$_BIN"
	cat >"$_CONF" <<EOF
P910ND_NUM=""
P910ND_OPTS=""
: > "$_CONF_MARK"
EOF
	make_init "$CASE_DIR/$_tag.init" "$_BIN" "$_CONF"
	printf '%s\n' "$_CONF_MARK" >"$CASE_DIR/$_tag.confmark.path"
	printf '%s\n' "$_START_MARK" >"$CASE_DIR/$_tag.startmark.path"
}

check_status() {
	_tag=$1
	_confmark=$(cat "$CASE_DIR/$_tag.confmark.path")
	rm -f "$_confmark"
	bash "$CASE_DIR/$_tag.init" status >"$CASE_DIR/$_tag.status.out" 2>&1
	STATUS_RC=$?
	obs "$_tag status exit: $STATUS_RC"
	obs "$_tag status output: $(tr '\n' '|' <"$CASE_DIR/$_tag.status.out")"
	[ "$STATUS_RC" -ne 5 ]
	assert_true "$?" "$_tag the script does not claim the binary is missing"
	assert_not_contains "$CASE_DIR/$_tag.status.out" "not installed" \
		"$_tag no false 'not installed' diagnostic"
	assert_not_contains "$CASE_DIR/$_tag.status.out" "too many arguments" \
		"$_tag no word-splitting error in test"
	# Locale-dependent shell messages ("filename argument required" vs a
	# translated equivalent) are not reliable; the marker below is the
	# locale-independent proof that '. "$P910ND_CONFIG"' really ran.
	assert_file_exists "$_confmark" \
		"$_tag the config file was actually sourced"
}

say "A: an installation path containing spaces"
make_fixture "$CASE_DIR/path with spaces" spaced
check_status spaced

say "B: control -- an ordinary path"
make_fixture "$CASE_DIR/plain" plain
check_status plain

say "C: the start branch executes the binary at the spaced path"
_startmark=$(cat "$CASE_DIR/spaced.startmark.path")
rm -f "$_startmark"
bash "$CASE_DIR/spaced.init" start >"$CASE_DIR/spaced.start.out" 2>&1
START_RC=$?
obs "spaced start exit: $START_RC"
obs "spaced start output: $(tr '\n' '|' <"$CASE_DIR/spaced.start.out")"
assert_file_exists "$_startmark" \
	"C the daemon binary at the spaced path was invoked"

say "D: static -- no bare binary/config path remains"
"$PYTHON" - "$INIT" >"$CASE_DIR/quoting.out" 2>&1 <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
code = "\n".join(l for l in s.splitlines()
                 if not l.lstrip().startswith("#"))
bare = re.findall(
    r'(?:test -[xr] |\. |start_daemon |--exec |checkproc |killproc -TERM )'
    r'\$P910ND_(?:BIN|CONFIG)', code)
bare += re.findall(r'^\s*\$P910ND_BIN\s', code, re.M)
opts_ok = ('"$P910ND_BIN" $P910ND_OPTS $P910ND_NUM' in code)
print("bare=%d opts_split=%s" % (len(bare), opts_ok))
sys.exit(0 if not bare and opts_ok else 1)
EOF
assert_eq "$?" "0" \
	"D every path use is quoted and P910ND_OPTS still splits"
assert_contains "$CASE_DIR/quoting.out" "bare=0 opts_split=True" \
	"D the scan really checked every call site"
assert_contains "$INIT" "PD-78: P910ND_BIN and P910ND_CONFIG" \
	"D the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
