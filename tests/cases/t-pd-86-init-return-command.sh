#!/bin/sh
#
# PD-86  [high]  init script helpers never ran the commands they wrapped.
#
# Four fallback definitions used `return <command> args`:
#   checkproc()    { return pidofproc ${1+"$@"} >/dev/null 2>&1; }  (x2)
#   start_daemon() { return daemon ${1+"$@"}; }
#   rc_check()     { return rc_failed $?; }
# `return` takes a NUMBER, so the command name was just a non-numeric
# argument: bash printed "numeric argument required", returned 2, and
# pidofproc/daemon/rc_failed were NEVER executed.  checkproc() is called by
# the status branch (line ~493) and start_daemon() by start (line ~319), so
# on LSB/RH systems status detection and daemon startup silently failed.
# Static analysis flagged all four as SC2151 (error level).
#
# A: the real function definitions, extracted from aux/p910nd.init, actually
#    invoke their wrapped commands and propagate the exit status.
# B: static -- no code line still uses `return <word>`.
# C: shellcheck -S error (when available) reports zero diagnostics.

set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO_ROOT/tests/lib.sh"

case_init PD-86 init-return-command

INIT="$REPO_ROOT/aux/p910nd.init"

"$PYTHON" - "$INIT" "$CASE_DIR/funcs.sh" <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()

# '.*' not '[^}]*': ${1+"$@"} itself contains a closing brace.
single = r'^[ \t]*%s\(\)[ \t]*\{.*\}[ \t]*$'
checks = [d.strip() for d in re.findall(single % "checkproc", s, re.M)
          if "pidofproc" in d and "/sbin/checkproc" not in d]
starts = [d.strip() for d in re.findall(single % "start_daemon", s, re.M)
          if "/sbin/start_daemon" not in d]
rc = re.search(r'^[ \t]*rc_check\(\)\s*\n\s*\{.*?^\s*\}[ \t]*$',
               s, re.M | re.S)
assert checks, "no fallback checkproc() found"
assert starts, "no fallback start_daemon() found"
assert rc, "no rc_check() found"
# The two branches define the same checkproc; one instance is enough.
with open(sys.argv[2], "w", encoding="utf-8") as fh:
    fh.write(checks[0] + "\n")
    fh.write(starts[0] + "\n")
    fh.write(rc.group(0).strip() + "\n")
print("extracted=%d" % (len(checks) + len(starts) + 1))
EOF
assert_eq "$?" "0" \
	"A the real helper definitions were extracted"
obs "extracted functions:"
sed 's/^/    /' "$CASE_DIR/funcs.sh" | while read -r l; do obs "$l"; done

say "A: the wrapped commands really run and their status propagates"
{
	printf '%s\n' '#!/bin/bash'
	printf 'P_MARK="%s"\n' "$CASE_DIR/pidof.marker"
	printf 'D_MARK="%s"\n' "$CASE_DIR/daemon.marker"
	printf 'F_MARK="%s"\n' "$CASE_DIR/rcfailed.marker"
	printf '%s\n' 'pidofproc() { : > "$P_MARK"; return 0; }'
	printf '%s\n' 'daemon() { : > "$D_MARK"; return 0; }'
	printf '%s\n' 'rc_failed() { : > "$F_MARK"; return 7; }'
	cat "$CASE_DIR/funcs.sh"
	printf '%s\n' 'checkproc some-program'
	printf '%s\n' 'echo "checkproc_rc=$?"'
	printf '%s\n' 'start_daemon some-program'
	printf '%s\n' 'echo "start_daemon_rc=$?"'
	printf '%s\n' 'false'
	printf '%s\n' 'rc_check'
	printf '%s\n' 'echo "rc_check_rc=$?"'
} >"$CASE_DIR/probe.sh"
bash "$CASE_DIR/probe.sh" >"$CASE_DIR/probe.out" 2>"$CASE_DIR/probe.err"
obs "probe stdout: $(tr '\n' '|' <"$CASE_DIR/probe.out")"
obs "probe stderr: $(tr '\n' '|' <"$CASE_DIR/probe.err")"

assert_file_exists "$CASE_DIR/pidof.marker" \
	"A checkproc() called pidofproc"
assert_file_exists "$CASE_DIR/daemon.marker" \
	"A start_daemon() called daemon"
assert_file_exists "$CASE_DIR/rcfailed.marker" \
	"A rc_check() called rc_failed"
assert_contains "$CASE_DIR/probe.out" "checkproc_rc=0" \
	"A checkproc propagates the wrapped status"
assert_contains "$CASE_DIR/probe.out" "start_daemon_rc=0" \
	"A start_daemon propagates the wrapped status"
assert_contains "$CASE_DIR/probe.out" "rc_check_rc=7" \
	"A rc_check propagates rc_failed's status"
assert_not_contains "$CASE_DIR/probe.err" "numeric argument required" \
	"A no return-argument error remains"

say "B: static -- no code line returns a command name"
"$PYTHON" - "$INIT" >"$CASE_DIR/b.out" 2>&1 <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
code = "\n".join(l for l in s.splitlines()
                 if not l.lstrip().startswith("#"))
bad = re.findall(r'\breturn[ \t]+[A-Za-z_][A-Za-z0-9_]*', code)
print("bad_returns=%d %s" % (len(bad), ",".join(bad)))
sys.exit(0 if not bad else 1)
EOF
assert_eq "$?" "0" \
	"B every return argument is numeric or a variable expansion"
assert_contains "$CASE_DIR/b.out" "bad_returns=0" \
	"B the scan really found no remaining sites"
assert_contains "$INIT" "PD-86: \"return rc_failed" \
	"B the reason is documented"

say "C: shellcheck error-level baseline"
if command -v shellcheck >/dev/null 2>&1; then
	n_err=$(shellcheck -S error "$INIT" 2>&1 |
		grep -c 'SC[0-9]* (error)' || true)
	obs_kv "shellcheck error diagnostics" "$n_err"
	assert_eq "$n_err" "0" \
		"C the init script has no error-level shellcheck findings"
else
	obs "shellcheck unavailable; the static regex above still applies"
fi

kill_stray
[ "$ASSERT_FAIL" -eq 0 ] || exit 1
exit 0
