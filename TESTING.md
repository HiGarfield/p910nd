# TESTING — how to reproduce the verification

One command, from a clean clone:

```
sh tests/run.sh          # or: make check
```

Everything is dependency-light: a C89 compiler, POSIX shell and Python 3. Tools
that are absent are SKIPped rather than failed (`tcpd.h`, `cppcheck`,
`valgrind`). Expect roughly **10–15 minutes**; two unit tests deliberately sleep
through dozens of idle-timeout windows.

## Layout

| File | Role |
|---|---|
| `tests/run.sh` | Entry point. Build gates, static analysis, unit tests, functional tests, sanitizers, valgrind. Prints a PASS/FAIL/SKIP table and exits non-zero if anything failed. |
| `tests/functional.py` | End-to-end daemon tests. Starts the real binary and verifies bytes with sha256. |
| `tests/test_*.c` | Unit tests. Each one `#include`s `../p910nd.c` with `main` renamed, so it can reach the daemon's internal functions directly. |
| `Makefile` | `make check` runs the suite; `make` builds normally. |

## Phases

**A — build gates.** Every configuration must compile producing **no
diagnostics at all** (an empty stderr is part of the assertion):

```
gcc   -std=c89 -Wall -Wextra -Wpedantic -O2
clang -std=c89 -Wall -Wextra -Wpedantic -O2
gcc   -std=c89 -Wall -Wextra -Wpedantic -DTESTING
gcc   -std=c89 ... -fsanitize=address,undefined -fno-omit-frame-pointer
gcc   -DUSE_LIBWRAP -DUSE_GETPROTOBYNAME ... -lwrap    (needs tcpd.h)
musl-gcc -std=c89 ...                                  (second C library)
gcc   -m32 -std=c89 ...                                (second word size)
```

Plus two Makefile policy gates: `make -n -B CFLAGS="-Werror -O2" p910nd` must show
the caller's `-Werror` in the compile line (BUG-011), and `make -n -B
USE_LIBWRAP=1 p910nd` must show both `-DUSE_LIBWRAP` and `-lwrap` (BUG-012).

**A2 — cppcheck** with `warning,performance,portability`. Findings are reported;
only `error:`-level output fails the gate.

**B — unit tests.** Each `tests/*.c` is compiled `-std=c89 -DTESTING` and run.
Tests that do not pin `LOCKFILE_DIR` themselves receive `-DLOCKFILE_DIR` pointing
into the scratch tree, so nothing is written to the host's `/var/lock`. Two tests
are intentionally slow (`test_fuzz_timing` ≈ 216 s,
`test_bidir_printer_preclear_response` ≈ 158 s) and pass well within the 300 s
per-test timeout.

**C — functional.** The real daemon runs in the foreground (`-d`) against a fake
printer and is spoken to over TCP:

| Case | What it proves |
|---|---|
| `transfer_1byte`, `boundary_8191/8192/8193` | ring-buffer wrap boundaries |
| `transfer_1mb_sha256`, `transfer_10mb_sha256` | 1 MiB / 10 MiB payloads are byte-identical |
| `inetd_one_job_serves_connection` | the (x)inetd path: a socket on descriptor 0 is served and the process exits 0 |
| `uni_idle_timeout_option_closes` | `-t` bounds an idle *unidirectional* job (BUG-008) |
| `uni_idle_default_timeout_applies` | without `-t`, the 5 s default bounds it (BUG-008, U9) |
| `uni_idle_zero_keeps_connection` | `-t 0` disables that timer too (BUG-008, U9) |
| `uni_idle_slow_client_survives` | activity refreshes that timer; no byte is lost (BUG-008) |
| `bidir_grace_follows_idle_timeout` | `-t` bounds the post-EOF grace window (BUG-009) |
| `numeric_id_out_of_range_rejected` | `-u 4294967296` is refused instead of truncating to root (BUG-010) |
| `transfer_slow_chunks` | 137-byte writes with pauses, still byte-identical |
| `half_close_tail_delivered` | `shutdown(SHUT_WR)` must not truncate the tail |
| `client_rst_daemon_survives` | RST mid-transfer; the next job still works |
| `client_disconnect_recovers` | client vanishes mid-transfer; recovery |
| `sequential_jobs_*` | three jobs in a row, each intact |
| `simultaneous_two_connections` | two open connections, both served in order |
| `bidir_printer_response_integrity` | printer→network reply arrives intact (pty) |
| `ipv6_loopback_transfer` | IPv6 path |
| `no_residual_child_processes` | no orphans after a job |
| `lock_dir_created_automatically` | BUG-001 regression: absent lock dir must not stop startup |
| `printer_stall_no_cpu_spin` | blocked printer sleeps instead of spinning |
| `printer_disappears_midjob` | device vanishing must not kill the daemon |

Bidirectional testing uses a **pty** (`os.openpty()`) rather than a FIFO: with a
single FIFO the daemon would read back its own writes, which would make the test
measure the wrong thing.

> **Trap worth knowing.** The daemon opens the printer device fresh for each job
> with plain `O_WRONLY`, exactly as it would a real printer. On a regular-file
> sink that means every job writes from offset 0 and **overwrites** rather than
> appends. An earlier revision of this harness assumed concatenation and reported
> three false failures (`client_rst_*`, `client_disconnect_*`,
> `simultaneous_two_connections`); those were harness bugs, not daemon bugs. Any
> multi-job assertion must empty the sink between jobs (`reset_sink()`).

**D — sanitizers.** The same functional traffic, filtered to the core cases, run
against an `-fsanitize=address,undefined` build with `detect_leaks=1`. Any
`AddressSanitizer`/`LeakSanitizer`/UBSan diagnostic fails the gate.

**E — valgrind.** Ten sequential jobs under
`--leak-check=full --track-fds=yes`. The gate fails only on:

* `definitely`/`indirectly lost` > 0, or
* an accepted connection socket still open at exit (a per-job fd leak).

It deliberately ignores descriptors **inherited from the caller's environment**
(an IDE or CI runner can hand the process dozens of open files) and the two long
lived descriptors the daemon legitimately holds until termination: the listening
socket and the lock file. The remaining `still reachable: 4096 bytes` is glibc's
`stdout` buffer from `-d` logging, not daemon state.

## Environment notes

### Two environment traps that produced false results

Both are now handled by the harness, and are recorded because they cost real
diagnosis time and would mislead anyone running a case by hand.

* **Descriptor 0 may be a socket.** `is_standalone()` decides between the inetd
  and standalone paths by calling `getsockname(0)`. When the runner hands the
  process a socket as stdin, that succeeds and the daemon silently takes the
  inetd path — no pid file, no listening socket — however it was started. The
  harness pins stdin to `/dev/null`. Runs with `-d` never showed this because
  `log_to_stdout` forces the same branch.
* **The daemon escapes the process group.** It calls `setsid()` itself, so killing
  its process group does not stop it; a leftover daemon then holds port 9100 and
  makes later cases fail for unrelated reasons. Teardown tracks the real pid (from
  the pid file, or by `pkill` on the case's unique device path).

### libwrap testing without root

`hosts_access()` consults `/etc/hosts.allow` and `/etc/hosts.deny`, which the
suite must not write to (and cannot, unprivileged). The deny branch is exercised
instead by preloading `tests/hosts_ctl_stub.so`, which replaces `hosts_ctl()` with
one that always denies; the daemon's own rejection code therefore runs for real.
That stub is a shared object rather than a program, so the unit phase skips any
`tests/*.c` without a `main()`.

Run as a non-root user is fine: port 9100 is unprivileged, and the harness
compiles the daemon with `-DLOCKFILE_DIR` aimed at a scratch directory so it
never touches `/var/lock`. `-d` is used throughout, which keeps the daemon in
the foreground and skips pid-file creation.

Override knobs: `CC_GCC`, `CC_CLANG`, `UNIT_TIMEOUT`, and a positional filter
(`sh tests/run.sh fuzz` selects matching unit tests and is forwarded to
`functional.py`).
