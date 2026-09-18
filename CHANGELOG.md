# CHANGELOG

Branch `fix/audit-0.97`, based on this tree's lineage of upstream 0.97
(`kenyapcomau/p910nd` @ `57ebc07`). All commits keep ANSI C89 as the enforced
language level and add no third-party dependency.

## This audit

### Fixed

* **BUG-001 · High · robustness** — the daemon no longer refuses to start when its
  lock or pid directory is missing. `ensure_parent_dir()` creates the leading
  components before the lock file and the pid file are opened. Path, name, mode
  and flock semantics are unchanged, and an existing directory is untouched; only
  the "directory absent ⇒ exit" behaviour is gone. This is Debian #634225.
  Note: Debian's own patch (`50-use-var-lock-p910nd...`) sidesteps the bug
  by relocating `LOCKFILE` to `/var/lock/p910nd`; this change keeps the documented
  `/var/lock/subsys/p9100d` instead, so existing deployments and the man page FILES
  entry stay valid.

* **BUG-002 · Medium · concurrency** — `get_lock()` now retries `fcntl(F_SETLKW)`
  when the blocking wait is interrupted by a signal (`EINTR`). Previously such an
  interruption was treated as a hard failure, so a daemon waiting its turn for the
  printer lock could exit for no reason or proceed without mutual exclusion.

* **BUG-003/BUG-004 · Low · portability** — two test sources used C99 constructs
  (a `for`-initialiser declaration, and declarations after statements) that break
  the mandated `-std=c89 -Wpedantic` gate. Both are now C89-clean. Production
  sources were already clean.

### Added

* `tests/run.sh` (`make check`) — one-command verification: build gates (gcc,
  clang, `-std=c89 -Wall -Wextra -Wpedantic`, `-DTESTING`, ASan/UBSan, libwrap),
  cppcheck, unit tests, end-to-end functional tests, the same traffic under
  sanitizers, and valgrind with leak and descriptor tracking. See `TESTING.md`.
* `tests/functional.py` — daemon-level tests with sha256 integrity checks:
  1 byte, the 8 KiB wrap boundaries, 1 MiB, slow chunked writes, half-close tail,
  client RST and mid-transfer disconnect, sequential and simultaneous jobs,
  bidirectional replies, IPv6, and survival when the printer stalls or vanishes.
* `tests/test_lock_eintr_retry.c` — deterministic regression test for BUG-002,
  verified by mutation.
* `BUGS.md`, `TESTING.md`, `UNRESOLVED.md`, this file, `PR_DESCRIPTION.md`.

### Documentation

* `p910nd.8` — new **SECURITY** section: no authentication or filtering, and combining
  `-b` with `-f` gives any reachable client read/append access to the named file
  (CVE-2018-10123 class). FILES now notes that the pid/lock directories are created
  if missing.

### Not changed — needs your decision

`-f` hardening designs, privilege dropping, the head-of-line block when the
printer is missing, pid-file lifetime/`O_NOFOLLOW`, libwrap verification, cross
compilation. All written up in `UNRESOLVED.md`.

---

## Second pass — everything in UNRESOLVED.md is now resolved

Each item has its own commit; `UNRESOLVED.md` carries the per-item write-up and
the residual caveats that this machine cannot prove.

### More defects fixed

* **BUG-005 · Medium · robustness** — `server()` accepted a connection and only
  then retried `open_printer()` forever, so a single client (or one mistyped `-f`)
  held the daemon: connected but unserved, with no other client able to take its
  place. The device is now opened **before** `accept()`; the descriptor is
  released on every path that does not reach a job. Retrying forever is
  unchanged.
* **BUG-006 · Low · security/hygiene** — the pid file was written with
  `fopen("w")`, which follows symlinks (as root that truncates whatever the link
  points at), and was never unlinked, so it kept naming a PID another process may
  later recycle. Now `open(..., O_NOFOLLOW)` plus removal on SIGTERM/SIGINT and at
  orderly exit.
* **BUG-007 · Low · portability** — the pid was printed with `%d`, undefined for a
  `pid_t` that is not `int`. Now formatted as `long`.

### New capabilities, all opt-in (default behaviour untouched)

* `-t <seconds>` — idle timeout; `0` disables. The post-EOF grace window that
  catches a late printer reply is deliberately **not** configurable, since it is
  what guarantees a job terminates. (U7)
* `-u <user>` / `-g <group>` — drop privileges once the lock file, pid file and
  listening socket exist. Names or numeric ids; unknown targets are a hard error.
  (U2)
* `-DDEVICE_ALLOWLIST` — refuse any printer device outside a build-time list,
  closing the `-b` + `-f` arbitrary read/append hole (CVE-2018-10123 class) for
  builds that opt in. (U1, design A)

### Verification that was previously only claimed

* **libwrap** is now compiled, linked and run rather than skipped: allow path
  serves byte-exactly, deny path closes the connection with nothing delivered and
  keeps serving, and no descriptor leaks. (U5/U8)
* **Portability** gained `-m32` and musl-gcc build gates. arm/mips cross
  compilation still needs a CI runner — recorded, not claimed. (U6)

## Third pass — five more defects, and two coverage gaps closed

### More defects fixed

* **BUG-008 · High · availability** — the unidirectional copy loop had no timeout
  of its own and ignored `-t` entirely, so a client that opened a connection and
  then sent nothing held the daemon forever; since one job is served at a time,
  that stopped every other host from printing, and connecting needs no
  authentication. The idle timer is now armed in both directions: 5 s by default,
  settable with `-t`, disabled by `-t 0`. (An interim revision armed it only when
  `-t` was given; by the operator's decision it is on by default, so the exposure
  is closed without anyone having to know about it. A client that legitimately
  pauses mid-job for longer than 5 s now needs `-t` raised.) A job is only closed
  when nothing is buffered, so no byte already accepted can be discarded.
* **BUG-009 · Medium · latency** — the post-EOF grace window that catches a late
  printer reply was hard-wired to 5 s and ignored `-t`, so every bidirectional job
  on a printer that never reports end-of-stream (any real parallel or USB printer)
  paid ~5 s on top of its transfer time, against 0.97 which finished as soon as the
  last byte was delivered. The window now follows `-t`, falling back to the
  compile-time default for `-t 0` so a job is always bounded.
* **BUG-010 · Medium · security** — `-u`/`-g` accepted any numeric value `strtol()`
  could parse and cast it to `uid_t`/`gid_t`. The cast truncates silently and
  truncating to 0 means root, so `-u 4294967296` reported "running as uid=0 gid=0"
  and kept full privileges. Values that do not survive a round trip through the
  target type are now refused.
* **BUG-011 · Low · build** — the Makefile rebuilt `CFLAGS` as
  `$(filter-out -W%, $(CFLAGS)) -Wall -Wextra`, discarding every `-W` switch the
  caller passed; distribution flags routinely include `-Werror=format-security`,
  so a package build silently lost the check it asked for. The project's warnings
  are now appended instead, and `install` sets modes explicitly.
* **BUG-012 · Low · build** — the Makefile honoured only `USE_WRAP` while the macro
  it defines is `USE_LIBWRAP`, so `make USE_LIBWRAP=1` succeeded and produced a
  binary with no tcpwrappers support: hosts.allow/hosts.deny silently unenforced.
  Both spellings are accepted. The man page names the alias.

### Coverage gaps closed (no behaviour change)

* `inetd_one_job_serves_connection` — every case used to pin stdin to `/dev/null`,
  so `is_standalone()` always chose `server()` and the `one_job()` path used under
  (x)inetd was never executed. It is now driven for real.
* `transfer_10mb_sha256` — a job that crosses the ring buffer a few thousand times.

### Verified, no defect (details in BUGS.md)

`ensure_parent_dir()` on relative paths (hypothesis tested and disproved), the
`gcc -fanalyzer` fd-leak report at `dup_fd_below_fdsetsize()` (false positive),
clang-tidy, and the `-u`/`-g` ordering.

## Compatibility

No breaking change. Command line (`-f -i -b -d -v`, `[0-9]`, plus the opt-in
`-t -u -g`), inetd/standalone detection, lockfile semantics, exit codes and log
messages are unchanged. Behaviour differs in exactly three situations, all of them
previously unbounded or wrong: a lock/pid directory that used to abort startup is
now created; a printer device that is missing no longer lets one accepted
connection block the daemon; and an idle job is now torn down after the idle
timeout (5 s by default, `-t` to taste, `-t 0` for the old "never time out"
behaviour) instead of holding the daemon forever.
Memory footprint grows by one 256-byte stack frame in two cold paths plus two
`struct timeval` in the unidirectional loop; no new allocation, no new dependency.
