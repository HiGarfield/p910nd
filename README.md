# p910nd

p910nd is a small printer daemon intended for diskless platforms that does not spool to disk but passes the job directly to the printer. Normally a lpr daemon on a spooling host connects to it with a TCP connection on port 910n (where n=0, 1, or 2 for lp0, 1 and 2 respectively). p910nd is particularly useful for diskless platforms. Common Unix Printing System (CUPS) supports this protocol, it's called the AppSocket protocol and has the scheme socket://. LPRng also supports this protocol and the syntax is lp=remotehost%9100 in /etc/printcap.

The repository at https://sourceforge.net/projects/p910nd/ is being phased out. Packagers please use this and not Sourceforge as the upstream repository now.

## Version

0.99

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
(`lock_held` guards against removing another instance's file).
12. **R12 dual-stack bind edge case.** With `net.ipv6.bindv6only=1` the IPv6
wildcard socket rejected IPv4 clients. The socket is now explicitly put into
dual-stack mode (`IPV6_V6ONLY=0`, `#ifdef` guarded).

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
