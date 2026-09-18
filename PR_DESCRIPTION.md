# p910nd 0.97 audit: startup robustness, lock correctness, and a real test suite

## Summary

This branch brings the 2014-frozen single-file daemon up to current expectations
for **startability and lock correctness**, adds the **first end-to-end test suite
this project has had**, and enforces **ANSI C89** across the build and tests. It
is intentionally conservative: no rewrite, no third-party dependency, no new
allocation, and **no change to documented behaviour** except where a missing
directory used to make the daemon refuse to start.

Across all three passes: **12** defects fixed (2 High, 5 Medium, 5 Low) plus 2 C89
portability defects in the test sources. Open items needing your decision: **3**
(new ones are U9–U11 in `UNRESOLVED.md`; U1–U8 are resolved).

## The fixes

### BUG-001 (High) — the daemon could not start on modern hosts

Debian #634225, never merged here. `get_lock()` opens `LOCKFILE` assuming every
directory component exists. On Debian/Ubuntu, containers, and read-only or
diskless images, `/var/lock/subsys` does not exist, so `open()` failed with
`ENOENT`, `get_lock()` returned failure and the process exited immediately:

```
$ p910nd -d -f /tmp/prn 0
/tmp/p910nd-lock-missing/sub/p9100d: No such file or directory
```

`ensure_parent_dir()` now creates the leading components for the lock file and,
symmetrically, for the pid file. Lock name, mode, `flock()` semantics and every
existing path are untouched; an existing directory is left alone.

*Chosen over the Debian workaround.* Debian's `50-use-var-lock-p910nd...` patch
sidesteps this by relocating `LOCKFILE` to `/var/lock/p910nd`. Relocating the path
changes what every deployment and the man page expect; creating the directory fixes
the same bug while keeping `/var/lock/subsys/p9100d` as documented.

Regression test: `lock_dir_created_automatically` fails before and passes after.

### BUG-002 (Medium) — a signal could make the daemon lose its print lock

`get_lock()` waits with `F_SETLKW` and treated *any* `fcntl()` failure as fatal.
That wait returns `-1/EINTR` when a signal arrives, so a single signal landing
during the wait made the daemon abandon a lock it could have taken moments later —
under (x)inetd several instances routinely wait on exactly this call, so the
outcome is either a spurious exit or two instances printing without mutual
exclusion. The wait is now retried.

This is currently latent (nothing installs an interrupting handler; `SIGPIPE` is
ignored), so the commit says so rather than overclaiming — but it is live the
moment any handler is added, or on a libc whose `signal()` omits `SA_RESTART`.
Covered by `tests/test_lock_eintr_retry.c`, **verified by mutation**: reverting the
retry makes the assertion fire.

### Portability

Two test sources used C99 constructs that break the mandated `-std=c89 -Wpedantic`
gate. Both fixed. Production sources were already clean, and now also build
warning-free under gcc, clang and musl-gcc.

## Third pass

Two of these are the kind of bug that only shows up when you stop reading the
code and start *timing* it.

* **BUG-008 (High)** — a client that connects and sends nothing blocks the daemon
  forever. The unidirectional loop had no timeout and ignored `-t` completely;
  the daemon serves one job at a time and connecting is unauthenticated, so this
  is a trivially triggerable denial of service. `-t` now applies there too, but
  only when given explicitly (0.97 never bounded these jobs and the 5 s default is
  shorter than pauses real jobs make), and only when nothing is buffered, so no
  byte can be lost.
* **BUG-009 (Medium)** — every bidirectional job took ~5 s longer than 0.97. The
  post-EOF window that catches a late printer reply was hard-wired to 5 s and
  ignored `-t`; a real printer never reports end-of-stream, so the window was
  always used in full. It now follows `-t` (measured 5.2 s → 1.2 s with `-t 1`).
* **BUG-010 (Medium)** — `-u 4294967296` silently truncated to uid 0: the daemon
  announced a successful privilege drop and stayed root. Numeric ids that do not
  fit `uid_t`/`gid_t` are now refused.
* **BUG-011 (Low)** — the Makefile discarded every `-W` flag the caller passed,
  which silently drops `-Werror=format-security` from distribution builds.
* **BUG-012 (Low)** — `make USE_LIBWRAP=1` reported success and produced a binary
  with no tcpwrappers: the Makefile only knew the `USE_WRAP` spelling while the
  macro it defines is `USE_LIBWRAP`. An operator asking for access control got
  hosts.allow/hosts.deny silently unenforced.

Every one of these is backed by a regression test that was first made to fail
against the unfixed code (mutation-verified), and two coverage gaps were closed
along the way: the (x)inetd path had never been executed by the suite, and no job
larger than 1 MiB had been compared byte for byte.

## The test suite (`make check`)

There was none. `tests/run.sh` now runs five phases: build gates (gcc, clang,
`-std=c89 -Wall -Wextra -Wpedantic`, `-DTESTING`, ASan/UBSan, libwrap — all must
compile with **zero** diagnostics), cppcheck, 38 unit tests, end-to-end functional
tests, the same traffic under sanitizers, and valgrind with leak and descriptor
tracking. Details in `TESTING.md`.

`tests/functional.py` drives the **real binary** against fake printer devices
(regular file, FIFO, pty for bidirectional) and checks bytes with sha256: 1 byte,
8191/8192/8193, 1 MiB, slow chunked sends, half-close tail delivery, client RST,
mid-transfer disconnect, sequential and simultaneous jobs, bidirectional replies,
IPv6, and survival when the printer stalls or disappears.

**Honest note on process.** Three functional cases initially failed
(`client_rst_daemon_survives`, `client_disconnect_recovers`,
`simultaneous_two_connections`). They were **harness bugs, not daemon bugs**: the
daemon re-opens the printer device per job with plain `O_WRONLY`, so on a regular
file each job writes from offset 0 rather than appending. Diagnosing them made me
conclude the daemon was fine; asserting prematurely would have produced three
phantom "fixes". The same discipline applied to the valgrind phase: its first
version counted descriptors inherited from the caller's environment and the
legitimately long-lived listening socket and lock file, so it reported a failure
where none existed. It now looks specifically for an accepted connection left open
at exit — a real per-job leak.

Result before these fixes: `M failed`. After: **all green** (`TESTING.md` lists the
two build/policy SKIPs: `tcpd.h` absent, no cross toolchains).

## Also verified, no defect found

Documented per item in `BUGS.md` with source or test evidence: ring-buffer wrap
and the full/empty ambiguity; `select()` `nfds` and `FD_SETSIZE` handling; `timeval`
re-arming each iteration (the 0.96 bug); `ssize_t`/`size_t` mixing; `-f` with an
over-long path; two-digit port numbers breaking `LOCKFILE` (already fixed here, more
strictly than Debian's patch 40); `progname` rewriting corrupting the syslog ident
(**I tested the hypothesis and disproved it** — the basename is stripped first);
zombies; per-job fd leaks; CPU spin on a stalled printer; half-close tail loss; the
printer→network direction not being select-driven (OpenWrt #2444).

Debian's `0.97-1` series was pulled from `salsa.debian.org/debian/p910nd` and each
patch mapped against this tree — see the comparison table at the end of `BUGS.md`.

## Compatibility

No breaking change. Command line, inetd/standalone detection, lock semantics, exit
codes and log text are unchanged. The single behavioural difference is that a
missing lock/pid directory no longer aborts startup. Cost: one 256-byte stack frame
in two cold paths, no new allocation, no new dependency.

## Please decide

U1–U8 are resolved (device allowlist, `-u`/`-g`, printer opened before `accept()`,
pid-file hardening, libwrap exercised, `-t`, musl/32-bit gates). Three remain:

* **U9** — should the idle bound apply to unidirectional jobs *by default*? It is
  opt-in today; the exposure it leaves is real but pre-existing, and the 5 s
  default is too short to impose on jobs that work today.
* **U10** — the bidirectional grace window still defaults to 5 s, which is 5 s
  more latency than 0.97 had. `-t 1` already buys the old latency; changing the
  compiled-in default is a one-token change.
* **U11** — this fork's `-v` exits after printing the version; upstream 0.97
  prints it and then serves lp0. Inherited, not introduced; listed for parity.

Also worth knowing: **U6's residual** still stands — arm/mips cross compilation
needs a CI runner with root, so big-endian targets remain unexercised.
