# p910nd audit — defect register

Target: `p910nd` 0.97-lineage tree (`HiGarfield/p910nd`, same author as
`kenyapcomau/p910nd`). Language level enforced throughout: **ANSI C89**
(`-std=c89 -Wall -Wextra -Wpedantic`, gcc and clang, zero warnings).

Method: full source read-out, upstream `kenyapcomau/p910nd` diff, the Debian
`0.97-1` patch series pulled from `salsa.debian.org/debian/p910nd`, then an
end-to-end harness (`tests/run.sh`) covering build gates, static analysis, unit
tests, functional traffic, sanitizers and valgrind. Every claim below is backed
by either a source line or a test that fails without the fix.

Severity = exploitation/impact × likelihood, not code size.

| ID | Location | Category | Trigger | Severity | Root cause | Fix | Test | Status |
|---|---|---|---|---|---|---|---|---|
| BUG-001 | `get_lock()` p910nd.c:676, `server()` pidfile:1728 | Robustness / startup | Host without the lock (or `/var/run`) directory, e.g. Debian/Ubuntu and any read-only image | **High** | `open(LOCKFILE, O_CREAT...)` assumed every directory component already existed; `ENOENT` made `get_lock()` fail and the daemon `exit(1)` immediately. This is Debian #634225 and it was never merged here. Debian works around it by moving `LOCKFILE` to `/var/lock/p910nd` (patch 50); this tree keeps the documented path instead. | `ensure_parent_dir()` creates each leading component, then the existing `open()` proceeds unchanged | `t_lock_dir_created` | **Fixed** da6f66b |
| BUG-002 | `get_lock()` p910nd.c:694 | Concurrency / lock correctness | Any signal delivered while `F_SETLKW` blocks waiting for a held printer lock | **Medium** | `fcntl(F_SETLKW)` returns `-1/EINTR` when interrupted; the code treated *any* failure as fatal, so the daemon abandoned a lock it could have taken a moment later — either exiting for no reason or proceeding without the mutual exclusion the lock exists to provide. Reachable today for any future signal handler and on libc whose `signal()` does not set `SA_RESTART`. | Loop the wait; only an error other than `EINTR` fails | `tests/test_lock_eintr_retry.c` (mutation-verified) | **Fixed** a94b8cb |
| BUG-003 | `tests/test_write_eof_deadcode_removed.c:115` | Portability / C89 | Building the test suite with `-std=c89 -Wpedantic` | **Low** | `for (size_t i = 0; ...)` — C99-only declaration inside a `for` initialiser, which ISO C90 forbids | Hoist the declaration | `gate-gcc-c89` / `gate-clang-c89` | **Fixed** 4e9b8c1 |
| BUG-004 | `tests/test_bidir_late_response_after_net_eof.c:108` | Portability / C89 | same | **Low** | `size_t total` / `int saw_data` declared after statements | Hoist the declarations (`dl` already sat at the head of its own block and was legal) | same | **Fixed** 6fc0dc8 |

## Verified — no defect found

Each item was examined and, where it can be observed, exercised by a test.

| Area | Conclusion | Evidence |
|---|---|---|
| Ring buffer wrap / off-by-one | **No issue** | All four index cases are handled: empty resets both indices, full → `avail = 0`, unwrapped → `BUFFER_SIZE - endidx`, wrapped → `startidx - endidx`; `writeBuffer()` splits a contiguous chunk at the wrap. `endidx == startidx` ambiguity cannot arise because it implies `bytes == 0` or `bytes == BUFFER_SIZE`, both handled earlier. Exercised at 8191/8192/8193 plus 1 MiB with sha256. |
| `select()` `nfds` / `FD_SETSIZE` overflow | **No issue** | Every `FD_SET`/`FD_ISSET`/`FD_CLR` is guarded by `FD_VALID`/explicit range checks; out-of-range descriptors are duplicated into range by `dup_fd_below_fdsetsize()` before use. `tests/test_dup_fd_leak.c`, `test_prep_writefds.c`. |
| `timeval` reused across `select()` calls | **No issue** | The bidirectional loop re-arms `timeout` every iteration (p910nd.c:1127); the classic 0.96 bug is not present. |
| `ssize_t` / `size_t` and signed mix-ups | **No issue** | All raw `read()`/`write()` results are `ssize_t` and only used after a `> 0`/`< 0` test; never compared as unsigned. |
| `-f` with an over-long path | **No issue** | The argument is used as a pointer, never copied into the fixed-size `lpname[]`, so there is nothing to truncate or overflow. |
| Two-digit port number breaking `LOCKFILE`/`PIDFILE` | **No issue — already fixed here** | `main()` accepts only a *single* digit (`argv[0][1] == '\0'`) and `server()` re-checks `0..9`. Debian patch `40-fix-port-number-bigger-than-9` adds the same guard with `strlen() > 1`; this tree's version is stricter (it also rejects non-digits). |
| `progname` rewrite corrupting the syslog identity | **No issue — hypothesis disproved** | `progname` is set to the *basename* before `strstr("p910n")` runs, so a directory component containing `p910n` is untouched. Confirmed empirically: running `/tmp/p910nd-dir/p910nd` rewrites only the final component (`.../p910nd-dir/p9100d`). |
| Zombie / orphaned children | **No issue** | Jobs are served serially with no per-job `fork()`; the daemon is its own session leader. `no_residual_child_processes` checks `/proc/<pid>/task/<pid>/children`. |
| Descriptor leak across jobs | **No issue** | valgrind over ten sequential jobs leaves no connected socket open and reports `definitely lost: 0`. The still-reachable 4 KiB is glibc's `stdout` buffer from `-d` logging, not daemon state. |
| CPU spin when the printer stalls | **No issue** | Both descriptors are non-blocking and the unidirectional loop blocks in `select()` on writability; `printer_stall_no_cpu_spin` measures < 1 s CPU across 2.5 s of blocked writing. |
| Tail lost on half-close (0.97 "wait until not busy") | **No issue** | `mark_eof_if_drained()` completes the job without waiting for another write opportunity; `half_close_tail_delivered` verifies the last bytes arrive. |
| Printer→network not driven by `select()` (OpenWrt #2444) | **No issue** | Both directions are prepared into the same `readfds`/`writefds` each iteration. |
| Whole-daemon deadlock on a missing printer | *See UNRESOLVED U3* | The retry loop is entered **after** `accept()`, so one client can hold the daemon while the device is absent. This is upstream's documented choice, so it was deliberately left alone. |
| Memory safety overall | **No issue observed** | ASan + UBSan clean over the functional subset; valgrind reports zero errors and zero definite leaks. |
| libwrap (`-DUSE_LIBWRAP`) | **Unverified** | `tcpd.h` is absent here, so the build gate is SKIPped. No Debian patch touches it. See UNRESOLVED U5. |
| Portability beyond glibc | **Clean** | `-std=c89` builds warning-free under gcc, clang and musl-gcc; the file also passes a C++98 syntax check. Cross toolchains for arm/mips are not installed. |

## Reference: Debian `0.97-1` series vs this tree

Pulled from `salsa.debian.org/debian/p910nd` (`debian/patches/series`):

| Debian patch | Relevant upstream? | State here |
|---|---|---|
| `10-replace-sysconfig-etc_default.patch` | No — Debian packaging | n/a |
| `20-use-debian.init-script.patch` | No — Debian packaging | n/a |
| `30-disable-start-by-default.patch` | No — Debian policy | n/a |
| `40-fix-port-number-bigger-than-9.patch` | Yes | Already satisfied, more strictly (BUGS row above) |
| `50-use-var-lock-p910nd-instead-off-var-lock-subsys.patch` | Yes | Solved differently by BUG-001: the directory is created instead of moving the path, so the documented `/var/lock/subsys/p9100d` FILES entry and every existing deployment keep working |

No Debian patch addresses libwrap, which matches finding U5.
