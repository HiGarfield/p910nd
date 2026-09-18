# UNRESOLVED

The first eight items (U1–U8) are resolved: each used to be an open question and
is now implemented or settled by a commit on this branch; the *residual* notes
record what still cannot be proven on this particular machine, which is a
different thing from being unresolved.

Three **new** items opened during the third pass (U9–U11) are at the bottom of
this file. **U9 is now resolved by your decision** (idle timer armed by default
in both directions); **U10 is resolved as "leave it"** (the bidirectional grace
window keeps its 5 s default); **U11 remains open** and is a one-line change
either way.

| Item | Subject | Resolution | Commit |
|---|---|---|---|
| U1 | `-b` + `-f` arbitrary file read/append (CVE-2018-10123 class) | Opt-in build-time allowlist `DEVICE_ALLOWLIST` | `55560c5` |
| U2 | No privilege dropping | `-u`/`-g`, default off | `d7192b5` |
| U3 | Missing printer blocks the whole daemon | Open the device before `accept()` | `1097fd4` |
| U4 | Pid file stale and symlink-following | `O_NOFOLLOW`, removed on stop | `3aa6c45` |
| U5 | libwrap unverified | Compiled, linked and exercised; no defect | `24d7453` |
| U6 | Cross compilation not exercised | musl and 32-bit gates added | `9f3c760` |
| U7 | No `-t` option | `-t <seconds>`, 0 disables | `51a97df` |
| U8 | Denied-connection path under libwrap | Covered by the U5 tests | `24d7453` |

---

## U1 — `-b` + `-f` arbitrary file access · resolved by `55560c5`

**Chosen design: A (allowlist).** `-DDEVICE_ALLOWLIST='"/dev/lp%c:/dev/usblp%c"'` —
colon separated path patterns where `%c` is the printer number. A device outside
the list, whether from `-f` or the default, is refused at startup before anything
is opened. Undefined, the check is compiled out and the build behaves exactly as
before, so no existing deployment changes.

Design B (refusing `-b` with `-f` unless a new flag is passed) was **not**
implemented: it forbids legitimate setups (a real bidirectional printer that is
not `/dev/lpN`) unless the operator passes yet another option, whereas the
allowlist permits the legitimate ones and blocks the dangerous ones with one
build-time decision.

*Residual:* the allowlist is a build-time choice, so a distribution that does not
set it remains exposed. That is deliberate — flipping the default would break
deployments — but it means the protection only exists for builds that opt in.

## U2 — privilege dropping · resolved by `d7192b5`

`-u <user>` and `-g <group>`, names or numeric ids, both optional and defaulting
to no change. The switch happens after the privileged work (lock file, pid file,
listening socket) and before any printer device is opened. Supplementary groups
are initialised only while privilege is still held, so an already unprivileged
daemon may ask for the identity it has. An unknown user or group is a hard error
rather than something to ignore, since silently continuing as root is the worst
outcome.

*Residual:* the ordering subtlety I flagged — the device is re-opened per job, so
the target account must be able to open it — is real and is documented in the man
page. And **dropping to a *different* account cannot be tested here**: `setuid()`
needs root, so the tests only cover dropping to the caller's own identity plus the
unknown-user rejection path. A root-run check of `-u` to a foreign account is still
worth doing before shipping.

## U3 — missing printer blocks the daemon · resolved by `1097fd4`

The device is now opened before `accept()`, so no connection is accepted while it
is unavailable. Retrying forever is unchanged, and `connect()` still succeeds
because the kernel completes the handshake into the listen backlog; the job is
served as soon as the device appears.

*Residual:* the trade-off I described is real and now documented in the man page —
the device is opened slightly earlier, so a printer that disappears while a job is
queued fails that job; the next iteration reopens it. Option (c), bounding the
retry and refusing the job, was rejected because it changes documented behaviour
more than this does.

## U4 — pid file · resolved by `3aa6c45`

Both halves implemented: `open(..., O_NOFOLLOW)` so a planted symlink is refused
instead of truncating its target, and the file is unlinked when the daemon stops
(SIGTERM/SIGINT handled by flag only, installed without `SA_RESTART` so they
interrupt a blocking `accept()`; `atexit()` covers the other orderly exits).
`PIDFILE` is also overridable now, like `LOCKFILE` and `PRINTERFILE`.

*Residual:* a pid file left by an unclean kill (`SIGKILL`, power loss) still
survives — no shutdown path can prevent that. It is simply overwritten on the next
start rather than blocking it.

## U5 / U8 — libwrap · resolved by `24d7453`

My earlier claim that this could not be verified was **wrong**: I had misread
`ls`'s exit status. `/usr/include/tcpd.h` and `libwrap.so` are present, so the
path is now compiled, linked and run.

*No defect.* The declaration in `p910nd.c` is identical to the one in `tcpd.h`,
so the 0.95 "incomplete conversion to `ip_addr`" concern does not apply here; the
address is passed as `client_addr` with `STRING_UNKNOWN` elsewhere, which is the
documented way to match on address.

Both branches are exercised: no rule → job served byte-exact; preloaded stub that
denies → connection closed with nothing delivered, rejection logged, daemon keeps
serving. Preloading is used because `hosts_access()` reads `/etc/hosts.deny`,
which a test suite must not write to. valgrind confirms five rejected connections
leave no descriptor behind.

*Residual:* real `/etc/hosts.allow`/`hosts.deny` rules are not exercised (they
need root to write). The daemon's own branch is covered; libwrap's rule parsing is
trusted as libwrap's own behaviour.

## U6 — portability · resolved by `9f3c760`

`-m32` (32-bit) and `musl-gcc` (second C library) are now build gates that must
compile with zero warnings, probed so hosts without them SKIP.

*Residual:* arm/mips cross compilation is still **not** exercised — the distro
carries `gcc-arm-linux-gnueabi` and `gcc-mips-linux-gnu`, but installing them
needs root, which this environment does not have. Big-endian and non-x86 targets
remain unverified and should be added to CI. Recording this rather than claiming
it.

## U7 — `-t` · resolved by `51a97df`

`-t <seconds>` sets the idle timeout; `0` disables it. Parsed with `strtol()` so
junk, empty strings, negatives and overflow are rejected instead of silently
becoming 0. The post-EOF grace window deliberately stays at the compile-time
default, because that window is what guarantees a job terminates — exposing "wait
forever" there would let one silent printer pin a connection open.

---

# Third pass — open items

## U9 — should the idle bound apply to unidirectional jobs *by default*? · **RESOLVED: yes**

**Decision (2026-09-18): the timer is armed by default.** Both directions now
time out after `idle_timeout` seconds (default 5, settable with `-t`, disabled by
`-t 0`), so the "one silent connection blocks the daemon" exposure is closed
without the operator having to know about it.

Implemented by deleting the "was it given explicitly?" flag, so there is one rule
for both directions. Consequences worth remembering:

* a client that pauses for more than 5 s in the middle of a unidirectional job is
  now disconnected — the *decision* was taken with that in mind, and `-t 60` (or
  any larger value) restores the old tolerance without losing the bound;
* no data can be lost: the job is only closed when the buffer is empty, and a job
  whose printer stopped accepting data keeps bytes pending and is never cut short;
* the man page's `-t` paragraph now says the timer is armed in both directions and
  tells operators to raise it for long-pausing clients.

## U10 — how long should the bidirectional grace window be by default? · **RESOLVED: leave it at 5 s**

Before BUG-009 that window was always 5 s; it now follows `-t`, so a site whose
printers never answer can set `-t 1` and get jobs that finish in ~1 s instead of
~5 s. **Decision (2026-09-18): the default stays 5 s**, because the window exists
precisely to catch a genuinely late reply and shortening it would risk dropping
one. No code change; `-t` remains the knob for sites that want the shorter job
time.

## U11 — `-v` exits; 0.97 printed the version and then ran the daemon

Upstream 0.97's `case 'v'` calls `show_version()` and *breaks*, so `p910nd -v`
prints the version and then falls through to the normal startup logic and serves
lp0 as a real daemon. This tree exits 0 instead, which is surely what anyone
typing `-v` meant, and it is what the man page describes.

The deviation is **inherited, not introduced**: it is present in this fork's
initial checkin, so it predates the audit and I left it alone. It is listed here
only because the brief asked for 0.97 parity. Removing `exit(0)` restores strict
compatibility and is a one-line change.

(The `break;` after `exit(0)` is dead code. It is also inherited and produces no
diagnostic, so it was left as is rather than touched for tidiness.)
