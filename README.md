# p910nd

p910nd is a small printer daemon intended for diskless platforms that does not spool to disk but passes the job directly to the printer. Normally a lpr daemon on a spooling host connects to it with a TCP connection on port 910n (where n=0, 1, or 2 for lp0, 1 and 2 respectively). p910nd is particularly useful for diskless platforms. Common Unix Printing System (CUPS) supports this protocol, it's called the AppSocket protocol and has the scheme socket://. LPRng also supports this protocol and the syntax is lp=remotehost%9100 in /etc/printcap.

The repository at https://sourceforge.net/projects/p910nd/ is being phased out. Packagers please use this and not Sourceforge as the upstream repository now.

## Version

1.0

Note that this is the same version as the last released version from 2014. No features have been added, nor any bugs fixed from that version. Several distributions have packaged this software ready to use. If you are not a developer there is no advantage to cloning this repository. The move to Github is to facilitate any submission of bug fixes and contributions.

## Bug fixes (0.99)

Second round of fixes. All changes are confined to `p910nd.c`. The command
line, the default device `/dev/lp%c`, the port numbering 9100+n and the
semantics of both data streams are unchanged. New tunables (override with
`-D` at build time): `PRINTER_STALL_TIMEOUT` (60s), `PRINTER_FLUSH_TIMEOUT`
(10s), `PRINTER_DRAIN_TIMEOUT` (5s), `LOCK_WAIT` (3s), `JOB_LOCK_WAIT` (30s),
`SILENT_TIMEOUT` (10s), `PRINTER_EOF_ZERO_READS` (20), `NO_PROGRESS_USLEEP`
(20000µs), `DRAIN_TIMEOUT` (3s), `DRAIN_MAX_BYTES` (256KiB). The action of
round one is preserved: SIGPIPE ignored, `accept()` keeps serving on
`ECONNABORTED`/`EPROTO`/`EPERM`/`EINTR`, non-blocking printer fd, no `%m`,
64-bit byte counters, `-v` exits, and the pid/lock files are cleaned up.

**P0: the daemon could hang forever**

1. **R1 a stalled printer hung the daemon permanently.** A blocking `write()`
to a printer that is off line, out of paper or simply full never returns, so
the daemon left the `select()` loop, sat in `pipe_write`/`usblp_write` and
never accepted another connection. The printer fd is now opened non-blocking
(`O_WRONLY|O_NONBLOCK` / `O_RDWR|O_NONBLOCK`) and writes are driven by the
`select()` writable event; `flush_buffer()` waits on `select()` too. A new
`PRINTER_STALL_TIMEOUT` abandons the job if no byte reaches the device for
that long, after which the connection is closed and the daemon returns to
`accept()`.
2. **R2 a second instance hung on `F_SETLKW`.** The instance lock used a
blocking `F_SETLKW`, so `service p910nd restart` (and any second start) hung
with no message. It now uses `F_SETLK` and retries for `LOCK_WAIT` seconds,
then logs "another p910nd is already running for this printer" and exits
non-zero.

**P1: data loss**

3. **R3 the idle timeout fired on a momentarily paused printer.** The
documented "only when both buffers are empty" rule was never implemented; any
pause longer than `IDLE_TIMEOUT` (30s) truncated the job. The idle timer now
only advances when `networkToPrinterBuffer.bytes == 0` (and, bidirectionally,
`printerToNetworkBuffer.bytes == 0`) and is suppressed once the client has
half closed (`eof_read`); a stalled device (data pending but unwritten) is
handled by the separate `PRINTER_STALL_TIMEOUT` instead.
4. **R4 the connection was torn down with RST.** On timeout/error the socket
was closed without `shutdown()`, so the client (CUPS/LPRng) got
`ECONNRESET` and any in-flight status data was lost. `close_connection()` now
sends `SHUT_WR` (FIN), drains the receive side with a time and byte cap, then
closes. Bidirectional jobs also flush the printer's reply before closing.
5. **R5 the device was closed before it had drained.** The 0.97 change log
requires waiting until the printer is no longer busy before closing, otherwise
the driver may drop the last write. `handle_connection()` now calls
`wait_printer_idle()` (bounded by `PRINTER_DRAIN_TIMEOUT`) before `close(lp)`,
skipped when the job itself failed (a stalled device is not going to drain).

**P2: CPU spinning**

6. **R6 an EOF device spun at 100% CPU.** A character device that only ever
returns 0 bytes (regular file, `/dev/null`, a wedged USB device) made
`select()` report readable forever. `readBuffer()` now counts consecutive
zero reads and, after `PRINTER_EOF_ZERO_READS`, marks the printer direction
`eof_read` so it is no longer polled; every iteration that moves no data also
pauses for `NO_PROGRESS_USLEEP`, so the loop can never busy-poll.

**P3: throughput / starvation**

7. **R7 one job blocked the whole daemon.** `server()` ran everything
serially, so a slow printer or a client that did not close starved every
later connection. Each connection is now served by its own `fork()`ed child;
the parent reaps children (`SIGCHLD`), and the per-printer lock (F_SETLK,
byte 1) keeps writes serialized so the GLOP (one printer, one writer) is
preserved. The instance lock (byte 0) is held for the daemon's whole life.
8. **R8 opening the printer blocked the accept loop.** `open_printer_retry()`
could sleep for up to `OPEN_PRINTER_MAX_WAIT` (30s) inside the accept loop.
With R7 the retry now happens in the per-job child, so the parent never
blocks on a missing printer.
9. **R9 a client that finished sending but did not close cost an extra 30s.**
The unidirectional path now releases a silent-but-half-closed client via
`SILENT_TIMEOUT` (10s) once `totalin > 0`; a connection that never sent a
byte still waits the full `IDLE_TIMEOUT`, and a half-closed client just waits
for the buffer to drain, never the idle timer.

**P4: misc**

10. **R10 the signal handler was not async-signal-safe.** It called `exit()`
from the handler, risking a re-entrant deadlock. The handler now only sets a
`volatile sig_atomic_t` flag; `server()` checks it at the top of the accept
loop and exits cleanly (running `atexit()`/`cleanup_and_exit()`). `SIGHUP` is
explicitly ignored.
11. **R11 the lock file was left behind.** `free_lock()` only closed the fd,
leaving `/var/lock/p9100d`. The owner now unlinks the lock file on exit
(`lock_held` guards against removing another instance's file). **Note:** the
third round (T6) removed that `unlink()` again — see below. The lock file is
now kept for the process lifetime so its inode stays stable and mutual
exclusion between instances (inetd mode) is preserved.
12. **R12 dual-stack bind edge case.** With `net.ipv6.bindv6only=1` the IPv6
wildcard socket rejected IPv4 clients. The socket is now explicitly put into
dual-stack mode (`IPV6_V6ONLY=0`, `#ifdef` guarded).

## Third round fixes (1.0)

Third round of fixes. All changes are still confined to `p910nd.c`. New
tunables (override with `-D` at build time): `PRINTER_REPLY_WINDOW` (10s, the
bi-directional reply wait), `MAX_CHILDREN` (12, cap on simultaneously forked
job children). Changed defaults: `SILENT_TIMEOUT` 10s -> 30s, `JOB_LOCK_WAIT`
30s -> 3s, `PRINTER_STALL_TIMEOUT` 60s -> 30s. The action of the first two
rounds is preserved (non-blocking I/O, `shutdown()`+drain before `close()`,
`wait_printer_idle()`, fork-per-connection, `SIGCHLD` reaping, `IPV6_V6ONLY`,
signal-only flags, lock-file ownership, `F_SETLK` with a bounded wait).

1. **T1 bi-directional jobs held the connection and the byte-1 job lock for 120s
   on every job (measured: 37/40 concurrent jobs rejected).** Root cause: the
   completion test `printerToNetworkBuffer.eof_read || now - eof_done >=
   PRINTER_REPLY_TIMEOUT(120)`. For character devices (`/dev/lpX`, `usblp`)
   `eof_read` is essentially never set (it needs 20 consecutive zero reads and
   the `FD_ISSET(lp,readfds)` branch to be reached), so every job waited the
   full fixed 120s before the connection and lock were released. Fix: the fixed
   `PRINTER_REPLY_TIMEOUT` is gone, replaced by `PRINTER_REPLY_WINDOW` (default
   10s). Once the client has half-closed and **both** buffers are drained, the
   job ends as soon as there has been no printer->network progress for that
   window, while any in-flight reply keeps flowing (the buffers are non-empty
   while data moves). The window is measured from the *last real printer->network
   movement*, so an actively talking device is never cut off and a silent one is
   released within seconds. Normal completion (regular-file EOF) and reply
   timeout are logged distinctly.

2. **T2 a slow client was silently truncated and told success (measured: second
   4KB segment dropped, `Finished job: 4096/4096`).** Root cause: the unidir
   branch used `SILENT_TIMEOUT` (10s) the moment `totalin > 0`. Real printing
   pauses often (CUPS building pages, congestion, slow link) so 10s is far too
   aggressive. Fix: `SILENT_TIMEOUT` default raised to 30s; the unidir idle
   check now uses `IDLE_TIMEOUT` only for a connection that has sent *no* byte
   (a probe) and `SILENT_TIMEOUT` for one that has streamed real data, so a
   mid-job pause is no longer mistaken for a dead client. If the no-progress
   timeout still fires with `totalout < totalin`, the job is marked failed and
   logged as incomplete, not as a misleading `Finished job`.

3. **T3 `zero_reads` was uninitialised and never cleared on a successful read,
   so the printer->network direction could be disabled (measured: status/reply
   data permanently dropped).** Root cause: the field is on the stack; if the
   garbage value was large it counted as 20 zero reads immediately and set
   `eof_read`. Fix: `initBuffer()` sets `zero_reads = 0` and `readBuffer()`
   resets it to 0 on every `result > 0` read, incrementing only on a genuine
   empty read.

4. **T4 a stalled printer cost ~70s and still reported success (measured: 50-80s
   hold, `Finished job: 65536/73728`, 8KB lost).** Root cause: the stall path
   set only a local flag and `flush_buffer()` gave up after `PRINTER_FLUSH_TIMEOUT`
   without marking an error, so `copy_stream()` returned 0 and the caller treated
   the job as done. Fix: `flush_buffer()` now sets `WRITE_ERR` when it gives up
   still holding bytes; `PRINTER_STALL_TIMEOUT` is 30s and its comment notes it
   shares the budget with `PRINTER_FLUSH_TIMEOUT`, so the worst case is ~40s not
   70s. Both the bi-directional and unidirectional tails log `Job incomplete`
   (with bytes sent/total) at `LOG_ERR` when `err` is set, and `copy_stream()`
   returns -1 so the connection is closed and the job lock released at once
   instead of being held for minutes.

5. **T5 fork-per-connection had no concurrency cap and could be amplified into a
   DoS (measured: 40 connections -> 40 children all blocking 30s on the lock).**
   Root cause: `server()` `fork()`ed unconditionally and `JOB_LOCK_WAIT` was 30s,
   so a flood of connections became a pile of blocking children. Fix:
   `MAX_CHILDREN` (default 12) bounds in-flight children; past the limit the
   connection is logged and refused (backpressure) instead of forking.
   `JOB_LOCK_WAIT` default is 3s so a contended printer fails the new job fast.
   The dead `got_sigchld` variable is replaced by a live `inflight_children`
   counter plus a `child_pids[]` table; the `SIGCHLD` handler reaps and keeps
   both in sync, and `server()` applies the backpressure check before `fork()`.

6. **T6 the lock file was unlinked, breaking mutual exclusion in inetd mode
   (instance B holding inode1's lock and instance C holding a freshly created
   inode2 could run at once, interleaving output).** Root cause: `free_lock()`
   `unlink()`ed the lock file while another instance might hold a lock on the
   same inode. Fix: `free_lock()` no longer unlinks the lock file, so its inode
   stays stable and every instance `fcntl()`-locks the same inode (serialised
   correctly). `one_job()` and `handle_connection()` now log (at `LOG_ERR`, with
   client address) when the instance lock or the job lock cannot be acquired,
   instead of silently closing the connection. A clean FIN is kept rather than a
   RST: AppSocket has no application-layer acknowledgement, so a RST would only
   risk discarding in-flight data while the client still believes the job
   succeeded (see trade-off note in `p910nd.8`).

7. **T7 in non `-d` mode every `LOG_DEBUG` message was dropped, so a fault could
   not be diagnosed from syslog.** Fix: the genuinely useful "why did the job
   stop" messages are raised above `LOG_DEBUG` — "printer sent no data, stop
   reading from printer" and "network write error, discarding further printer
   data" are now `LOG_INFO`. The per-chunk read/write traces stay `LOG_DEBUG`
   to avoid flooding.

8. **T8 `select()` was used with `fd`/`lp` never checked against `FD_SETSIZE`, so
   a high descriptor number overran the `fd_set` (undefined behaviour: random
   corruption or crash).** Fix: `copy_stream()` refuses the job when either
   descriptor is `< 0` or `>= FD_SETSIZE`, and `server()` closes and skips the
   connection after `accept()` if the new `fd >= FD_SETSIZE` (both logged at
   `LOG_ERR`).

9. **T9 after `eof_sent` the loop kept arming the printer-write `fd_set` and
   re-logging "write: eof" ~10x/s for the whole window (CPU burn and log
   flood).** Fix: `prepBuffer()` no longer arms the write fd for a buffer whose
   EOF has already been forwarded, and `writeBuffer()` only marks/sends EOF
   once. Combined with the T1 bounded window the loop goes quiet once drained.

10. **T10 cleanup.** (a) `got_sigchld` was a dead variable; replaced by the live
    `inflight_children` counter (see T5). (b) a terminating daemon left printing
    children as orphans (`init` would adopt them and the init script could
    unmount the device mid-job); `server()` now calls `reap_children_and_exit()`
    which signals every tracked child with `SIGTERM` and waits up to 5s, logging
    how many jobs were in progress. (c) rejected jobs (busy printer / no
    instance lock) now log `LOG_ERR` with the client address (see T6).

## Bug fixes (0.98)

All changes are confined to `p910nd.c` (plus `aux/p910nd.init`). The command line, the default device `/dev/lp%c`, the port numbering 9100+n and the semantics of both data streams are unchanged.

**A. The daemon used to be killed or to exit completely**

1. **A1 SIGPIPE was not handled.** In bidirectional mode a client that went away while printer data was on its way back killed the process with SIGPIPE. `SIGPIPE` is now ignored and `EPIPE`/`ECONNRESET` are treated as "peer is gone" (debug log, printer to network direction dropped) instead of a fatal error.
2. **A2 one failed `accept()` terminated the daemon.** `ECONNABORTED` (aborted TCP handshake, very common with port scanners and printer status probes), `EMFILE`, `ENOBUFS`, `EPROTO`, `EINTR`... all ended in `exit(1)`. The daemon now continues on per-connection and transient errors (with a short backoff on resource exhaustion) and only leaves the loop when the listening socket itself is unusable (`EBADF`/`ENOTSOCK`/`EINVAL`).
3. **A3 no `EINTR` retry.** `accept()`, `select()`, `read()` and `write()` returned `-1` with `errno == EINTR` were handled as errors. All of them are now retried.
4. **A4 the daemonizing close loop could be endless.** `for (fd = 0; fd < resourcelimit.rlim_max; ++fd) close(fd);` never terminated when `RLIMIT_NOFILE` was `RLIM_INFINITY` and wasted up to a million `close()` calls otherwise. It now uses `rlim_cur`, falls back to `sysconf(_SC_OPEN_MAX)` and caps the scan, so startup is immediate.

**B. The daemon could hang without recovering**

5. **B1 no timeout at all in unidirectional mode.** A client that connected and neither sent data nor closed blocked the single threaded daemon in `read()` forever and starved every later job. The socket now gets `SO_RCVTIMEO`/`SO_SNDTIMEO`, the unidirectional copy is driven by `select()` and a job is dropped when nothing has moved for `IDLE_TIMEOUT` seconds (30 by default, `-DIDLE_TIMEOUT=n` at build time).
6. **B2 no `SO_KEEPALIVE`, no `TCP_NODELAY`.** Half open connections (power loss, unplugged cable) were never detected. Accepted sockets now get `SO_KEEPALIVE` plus `TCP_KEEPIDLE`/`TCP_KEEPINTVL`/`TCP_KEEPCNT` where available, and `TCP_NODELAY` so status queries are answered at once.
7. **B3 infinite retry when the printer cannot be opened.** `while (open_printer() == -1) sleep(10);` looped forever (logging every 10 s) on `ENOENT`/`ENODEV`/`EACCES` and stopped accepting connections completely. Only transient errors (`EBUSY`, `ENOMEM`, ...) are retried now, with exponential backoff and a hard limit (`OPEN_PRINTER_MAX_WAIT`); a permanent failure closes just that connection and the daemon keeps listening.
8. **B4 the "30 s without network data" timer killed large jobs.** It was refreshed only by network reads, so a client that had finished sending and waited for the printer status (the normal bidirectional use) was cut off. The two 30 s/10 s timers are replaced by one idle timeout that only counts when both directions and both buffers are idle, and buffered data is flushed before the job is abandoned.

**C. Data could be lost or errors were handled wrongly**

9. **C1 `readBuffer()` reset the ring indices while `bytes` kept its old value** when `err` was already set, so `writeBuffer()` later wrote stale or duplicated data. The ring is only reset when it is really empty. `writeBuffer()` also no longer writes past the end of a wrapped ring buffer.
10. **C2 a read error discarded data already buffered.** The unidirectional loop exited as soon as `READ_ERR` was set; it now writes out what was received first and only then stops.
11. **C3 a network write error aborted the whole bidirectional transfer.** The comment said "discard further printer data", but `break` left the loop and truncated the print job. Only the printer to network direction is disabled now (`outfd = -1`) and the job keeps flowing to the printer.
12. **C4 `EAGAIN`/`EWOULDBLOCK` was treated as a fatal error.** With the printer opened `O_NONBLOCK` in bidirectional mode a full printer buffer aborted the job. It is now "try again later" and `select()` waits.
13. **C5 the "10 s after a network read error" break** discarded buffered data and was too short for slow devices; it is covered by the idle timeout and the final flush.
14. **C6 `copy_stream()` only reported errors of the network to printer direction**, so a failure of the status return path looked like success. Both directions are now part of the return value and the log names the client.
15. **C7 dangling `device` pointer.** `open_printer()` made the global `device` point to a stack buffer; every later `open(device, ...)` read a dead stack frame (undefined behaviour). The generated name now lives in `static` storage.
16. **C8 `need_clear_lp` was never set to 1**, so the three branches that cleared the printer to network buffer were dead code. The variable and its branches were removed; the pacing timer still works as before.

**D. Logging, portability and other defects**

17. **D1 `%m` is a GNU extension and is unsupported on musl (OpenWrt)**; it was also used where `errno` had no meaning (`getaddrinfo()`, after `copy_stream()`). All messages use `strerror(errno)`, `getaddrinfo()` uses `gai_strerror()` and logs the address and port.
18. **D2 `open_printer()` logged twice on failure** and the first `vsyslog()` could change `errno` before the second `%m`. `errno` is saved and exactly one message is logged.
19. **D3 `get_ip_str()` returned NULL** for unknown address families and `strncpy()` did not guarantee the terminating NUL; the result is passed to `%s` and to libwrap's `hosts_ctl()`. It now always returns a NUL terminated string.
20. **D4 `clientlen` was not reset in the accept loop**, so an IPv4 client after an IPv6 one got its address truncated. It is now reset (and the address zeroed) on every iteration.
21. **D5 `p910nd -v` printed the version and then started the daemon.** It exits with status 0 now.
22. **D6 `openlog()` could be called with NULL** when the program name did not contain `p910n`. It falls back to `progname`.
23. **D7 `char service[sizeof(BASEPORT+lpnumber-'0')+1]`** is `sizeof(int)+1`, i.e. 5, and only worked because "9100" happens to be 4 characters. It is `char service[16]` with `snprintf()`.
24. **D8 `totalin`/`totalout` were `int`** and overflowed (negative byte counts) for jobs larger than 2 GB. They are `uint64_t` now and are printed with `%llu`.
25. **D9 the pid file was never removed**, so a killed daemon left `/var/run/p910nd.pid` behind and start scripts believed it was still running. `SIGTERM`/`SIGINT` and `atexit()` remove the pid file and release the lock.
26. **D10 a superfluous `result` argument** was passed to `dolog()` in the "network write error" message.
27. **D11 `aux/p910nd.init` called `startproc`**, a SUSE-only command, while the script defines `start_daemon`/`killproc`. Starting failed with "command not found" on Debian/Ubuntu. It now probes for `start_daemon`, falls back to `start-stop-daemon` or a plain invocation, and `stop` removes leftover pid files.

Build note: `make` is warning free with `gcc -Wall -Wextra` (the previously unchecked `chdir()` and `dup()` return values are checked as well).

## Authors

**Ken Yap** and others

## License

See the [LICENSE](LICENSE.md) file for license rights and limitations (GPL2).
