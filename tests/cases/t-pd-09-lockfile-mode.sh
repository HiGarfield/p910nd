#!/bin/sh
#
# PD-09  [低]  The lock file must not be world writable, and must not be
# followable through a symlink.
#
# open(..., 0666) left the resulting mode entirely to the caller's umask, and
# the -d and (x)inetd paths never set one.  Measured before the fix: umask 0002
# gave 664, umask 000 gave 666.  The request is now 0644, and O_NOFOLLOW keeps a
# planted symlink from redirecting the lock somewhere else.
#
# A: the mode no longer depends on the umask
# B: a symlink at the lock path is refused instead of followed
# C: only the daemonising path calls umask(), and it does not matter any more

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-09 lockfile-mode

measure_mode() {
	# measure_mode <label> <umask-value>
	_label=$1
	_um=$2
	_dir="$CASE_DIR/lock-$_label"
	rm -rf "$_dir"
	mkdir -p "$_dir"
	_bin=$(build_variant "test-lockdir-$_label" "-DLOCKFILE_DIR=\"$_dir\"")
	( umask "$_um"; daemon_start "$CASE_DIR/$_label.daemon.log" "$_bin" \
		-d -i 300.300.300.300 0 </dev/null )
	sleep 0.6
	daemon_stop
	if [ -f "$_dir/p9100d" ]; then
		_mode=$(stat -c '%a' "$_dir/p9100d")
	else
		_mode="absent"
	fi
	eval "MODE_$_label=\$_mode"
	obs_kv "$_label: umask $_um -> lock file mode" "$_mode"
}

say "A: the mode is 0644 whatever the umask is"
measure_mode a "$(umask)"
measure_mode b 000
measure_mode c 022

assert_eq "${MODE_a:-x}" "644" "A the caller's umask ($(umask)) no longer widens the lock file"
assert_eq "${MODE_b:-x}" "644" "A umask 000 no longer produces a world writable lock file"
assert_eq "${MODE_c:-x}" "644" "A umask 022 is unchanged"

say "B: a symlink at the lock path is refused, not followed"
LINKDIR="$CASE_DIR/lock-link"
rm -rf "$LINKDIR"
mkdir -p "$LINKDIR"
target="$CASE_DIR/decoy"
: >"$target"
ln -s "$target" "$LINKDIR/p9100d"
linkbin=$(build_variant test-lockdir-symlink "-DLOCKFILE_DIR=\"$LINKDIR\"")
daemon_start "$CASE_DIR/link.log" "$linkbin" -d -i 127.0.0.1 -f "$CASE_DIR/lp0" 0
sleep 0.6
if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
	daemon_stop
	assert_true 1 "a symlinked lock file is refused"
else
	assert_true 0 "a symlinked lock file is refused"
fi
obs "B daemon log:"
sed 's/^/    /' "$CASE_DIR/link.log" | while read -r l; do obs "$l"; done
assert_contains "$CASE_DIR/link.log" "p9100d" "B the refusal names the lock file"
assert_eq "$(wc -c <"$target" | tr -d ' ')" "0" "B the symlink target was not used as the lock"

say "C: only the daemonising path calls umask(), and it no longer matters"
obs "umask() call sites: $(grep -n 'umask(' "$REPO_ROOT/p910nd.c" | tr '\n' ' ')"
assert_eq "$(grep -c 'umask(022)' "$REPO_ROOT/p910nd.c")" "1" \
	"C exactly one umask() call exists, inside the daemonising branch"
assert_contains "$REPO_ROOT/p910nd.c" "O_CREAT | O_RDWR | O_NOFOLLOW, 0644" \
	"C the lock is opened with a fixed mode and no symlink following"

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
