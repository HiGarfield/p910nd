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
```

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
| `transfer_1mb_sha256` | 1 MiB payload is byte-identical |
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

Run as a non-root user is fine: port 9100 is unprivileged, and the harness
compiles the daemon with `-DLOCKFILE_DIR` aimed at a scratch directory so it
never touches `/var/lock`. `-d` is used throughout, which keeps the daemon in
the foreground and skips pid-file creation.

Override knobs: `CC_GCC`, `CC_CLANG`, `UNIT_TIMEOUT`, and a positional filter
(`sh tests/run.sh fuzz` selects matching unit tests and is forwarded to
`functional.py`).
