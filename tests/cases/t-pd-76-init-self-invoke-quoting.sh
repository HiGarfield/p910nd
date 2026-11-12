#!/bin/sh
#
# PD-76  [medium]  Recursive init-script calls must quote $0.
#
# restart, try-restart and force-reload re-invoke the script as `$0 stop`,
# `$0 start`, `$0 status`, `$0 restart` and `$0 try-restart`.  Without
# quotes, an installation path containing spaces makes the shell try to run
# the first word: measured with the script copied to "path with spaces":
#   exit 127, "line 429: .../path: No such file or directory",
#   and neither "Shutting down p910nd" nor "Starting p910nd" was printed.
# The service was therefore neither stopped nor restarted, silently.
#
# A: control -- a plain path still performs both recursive calls.
# B: a path with spaces performs both recursive calls too.
# C: static -- all five self-invocations are quoted and none is bare.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-76 init-self-invoke-quoting

INIT="$REPO_ROOT/aux/p910nd.init"
CONF="$CASE_DIR/p910nd.conf"
printf 'P910ND_NUM=""\nP910ND_OPTS=""\n' >"$CONF"

make_copy() {
	_dest=$1
	mkdir -p "$(dirname "$_dest")"
	sed -e 's|^P910ND_BIN=/usr/sbin/p910nd|P910ND_BIN=/bin/true|' \
		-e "s|^P910ND_CONFIG=/etc/sysconfig/p910nd|P910ND_CONFIG=$CONF|" \
		"$INIT" >"$_dest"
	chmod +x "$_dest"
}

check_restart() {
	_label=$1
	_copy=$2
	bash "$_copy" restart >"$CASE_DIR/$_label.out" 2>&1
	_rc=$?
	obs "$_label exit: $_rc"
	obs "$_label output: $(tr '\n' '|' <"$CASE_DIR/$_label.out")"
	obs_kv "$_label stop/start markers" \
		"$(grep -c 'Shutting down p910nd\|Starting p910nd' "$CASE_DIR/$_label.out")"
	assert_not_contains "$CASE_DIR/$_label.out" "No such file or directory" \
		"$_label no path-splitting error"
	assert_contains "$CASE_DIR/$_label.out" "Shutting down p910nd" \
		"$_label the stop sub-command really ran"
	assert_contains "$CASE_DIR/$_label.out" "Starting p910nd" \
		"$_label and the start sub-command really ran"
	assert_lt "$_rc" "127" \
		"$_label and the command was found"
}

say "A: control -- an ordinary path"
PLAIN="$CASE_DIR/plain/p910nd.init"
make_copy "$PLAIN"
check_restart a "$PLAIN"

say "B: an installation path containing spaces"
SPACED="$CASE_DIR/path with spaces/p910nd.init"
make_copy "$SPACED"
check_restart b "$SPACED"

say "C: static -- every self-invocation is quoted"
"$PYTHON" - "$INIT" >"$CASE_DIR/quoting.out" 2>&1 <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
# Comments quote the old form as an example; only real code lines count.
code = "\n".join(l for l in s.splitlines()
                 if not l.lstrip().startswith("#"))
cmd = r"(?:status|restart|stop|start|try-restart)\b"
quoted = len(re.findall(r'"\$0" ' + cmd, code))
bare = len(re.findall(r'\$0 ' + cmd, code))
print("quoted=%d bare=%d" % (quoted, bare))
sys.exit(0 if quoted == 5 and bare == 0 else 1)
EOF
assert_eq "$?" "0" \
	"C all five self-calls are quoted and none is bare"
assert_contains "$CASE_DIR/quoting.out" "quoted=5 bare=0" \
	"C the scan really found all five call sites"
assert_contains "$INIT" "PD-76: \$0 must be quoted" \
	"C the reason is documented"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
