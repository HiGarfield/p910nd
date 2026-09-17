# UNRESOLVED — status: all eight items have been resolved

Every item below used to be an open question. Each is now implemented or
settled by a commit on this branch; the *residual* notes record what still
cannot be proven on this particular machine, which is a different thing from
being unresolved.

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
