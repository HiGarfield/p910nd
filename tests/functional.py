#!/usr/bin/env python3
#
# End-to-end functional tests for p910nd (GPLv2).
#
# Every case starts the real daemon in foreground mode (-d) against a fake
# printer device and talks to it over TCP, verifying byte-level integrity with
# sha256.  No third-party framework: only the Python 3 standard library and
# POSIX facilities already used by the daemon.
#
# Usage: functional.py [--bin PATH] [--filter REGEX]
# Prints "PASS <name>" / "FAIL <name>: reason" and exits non-zero on failure.

import argparse
import errno
import hashlib
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import termios
import time
import tty

PORT_BASE = 9100
IPV4_HOST = "127.0.0.1"
IPV6_HOST = "::1"

# Set from --bin-deeplock: a build whose compiled-in lock directory does not
# exist, used to prove the daemon can create it.
DEEP_LOCK_BIN = None
# Directory the purpose-built binary writes its pid file into.
PIDFILE_DIR = None
# Build compiled with DEVICE_ALLOWLIST.
ALLOW_BIN = None
# Build compiled with -DUSE_LIBWRAP, and the preloadable deny stub.
LIBWRAP_BIN = None
LIBWRAP_STUB = None
_failures = []
_passes = []
_skips = []


def payload(n):
    """Deterministic, cheap-to-generate payload of exactly n bytes."""
    base = bytes(range(256))
    return (base * (n // 256 + 1))[:n]


def sha(b):
    return hashlib.sha256(b).hexdigest()


def record(name, ok, detail=""):
    if ok:
        _passes.append(name)
        print("PASS %s" % name)
    else:
        _failures.append(name)
        print("FAIL %s: %s" % (name, detail))
    sys.stdout.flush()


def skip(name, why):
    _skips.append(name)
    print("SKIP %s: %s" % (name, why))
    sys.stdout.flush()


class Daemon:
    """Runs p910nd in the foreground (-d) so we control its lifetime."""

    def __init__(self, binpath, dev, n=0, bidir=False, ipv6=False, extra=None,
                 foreground=True, preload=None):
        self.proc = None
        # The daemon calls setsid() itself, so it leaves our process group;
        # without tracking its real pid, teardown would leave it running and
        # holding the port for the next case.
        self.daemon_pid = 0
        self.n = n
        self.ipv6 = ipv6
        self.port = PORT_BASE + n
        self.logpath = tempfile.mktemp(prefix="p910nd-log-")
        args = [binpath]
        if foreground:
            args.append("-d")
        if bidir:
            args.append("-b")
        args += ["-f", dev]
        if ipv6:
            args += ["-i", IPV6_HOST]
        if extra:
            args += list(extra)
        args.append(str(n))
        self.logfile = open(self.logpath, "w+b")
        # stdin is pinned to /dev/null on purpose.  is_standalone() decides
        # between the inetd and standalone code paths by calling
        # getsockname(0): when the test runner itself hands the process a
        # socket as descriptor 0, that succeeds and the daemon silently takes
        # the inetd path (one_job, no pid file) however it was started.
        # /dev/null is not a socket, so standalone is chosen deterministically.
        self.devnull = open(os.devnull, "r+b")
        env = None
        if preload:
            env = dict(os.environ)
            env["LD_PRELOAD"] = preload
        self.proc = subprocess.Popen(args, stdin=self.devnull,
                                     stdout=self.logfile,
                                     stderr=subprocess.STDOUT,
                                     env=env, preexec_fn=os.setsid)
        self.path = binpath

    def wait_ready(self, timeout=10.0):
        deadline = time.time() + timeout
        host = IPV6_HOST if self.ipv6 else IPV4_HOST
        while time.time() < deadline:
            if self.proc.poll() is not None:
                return False
            try:
                s = socket.create_connection((host, self.port), timeout=0.5)
                s.close()
                return True
            except (socket.error, OSError):
                time.sleep(0.05)
        return False

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def wait_pidfile(self, path, timeout=10.0):
        """Return the pid recorded in `path`, or -1 if it never appears."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                with open(path, "r") as f:
                    pid = int(f.read().strip())
                self.daemon_pid = pid
                return pid
            except (IOError, OSError, ValueError):
                time.sleep(0.05)
        return -1

    @staticmethod
    def wait_pid_gone(pid, timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not os.path.exists("/proc/%d" % pid):
                return True
            time.sleep(0.05)
        return False

    def cpu_seconds(self):
        """Total CPU time consumed so far, from /proc/<pid>/stat."""
        try:
            with open("/proc/%d/stat" % self.proc.pid, "r") as f:
                fields = f.read().rsplit(")", 1)[1].split()
            ticks = os.sysconf("SC_CLK_TCK")
            return (int(fields[11]) + int(fields[12])) / float(ticks)
        except (IOError, OSError, IndexError, ValueError):
            return -1.0

    def log(self):
        try:
            self.logfile.flush()
            with open(self.logpath, "r", errors="replace") as f:
                return f.read()
        except IOError:
            return ""

    def kill(self):
        if self.proc is None:
            return
        if getattr(self, "daemon_pid", 0):
            try:
                os.kill(self.daemon_pid, signal.SIGKILL)
            except OSError:
                pass
            self.wait_pid_gone(self.daemon_pid, 5.0)
            self.daemon_pid = 0
        if self.proc.poll() is None:
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except OSError:
                try:
                    self.proc.kill()
                except OSError:
                    pass
        try:
            self.proc.wait(timeout=5)
        except Exception:
            pass
        try:
            self.logfile.close()
        except Exception:
            pass
        try:
            self.devnull.close()
        except Exception:
            pass
        try:
            os.unlink(self.logpath)
        except OSError:
            pass


def reset_sink(path):
    """Truncate the sink file.

    The daemon opens the printer device fresh for every job with plain
    O_WRONLY (no O_APPEND), exactly as it would a real printer.  On a regular
    file that means each job starts writing at offset 0, so consecutive jobs
    overwrite rather than append.  Every verification therefore compares one
    job's bytes against a sink that was emptied just before that job.
    """
    with open(path, "wb"):
        pass


def client_send(data, port, ipv6=False, half_close=True, chunk=None,
                delay=0.0, rst=False, recv_timeout=30.0, abort_after=None):
    """Send `data` to the daemon.  Returns bytes received before EOF."""
    host = IPV6_HOST if ipv6 else IPV4_HOST
    s = socket.create_connection((host, port), timeout=recv_timeout)
    received = b""
    try:
        if rst:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                         b"\x01\x00\x00\x00\x00\x00\x00\x00")
        if chunk:
            off = 0
            while off < len(data):
                piece = data[off:off + chunk]
                s.sendall(piece)
                off += len(piece)
                if delay:
                    time.sleep(delay)
        else:
            if data:
                s.sendall(data)
        if abort_after is not None:
            time.sleep(abort_after)
            return received
        if half_close:
            s.shutdown(socket.SHUT_WR)
        s.settimeout(recv_timeout)
        while True:
            try:
                b = s.recv(65536)
            except socket.timeout:
                break
            if not b:
                break
            received += b
    finally:
        try:
            s.close()
        except socket.error:
            pass
    return received


def read_file_bytes(path, expected=None, timeout=15.0):
    """Read the printer sink file, polling until it reaches `expected` bytes."""
    deadline = time.time() + timeout
    data = b""
    while time.time() < deadline:
        try:
            with open(path, "rb") as f:
                data = f.read()
        except IOError:
            pass
        if expected is None or len(data) >= expected:
            return data
        time.sleep(0.05)
    return data


# --------------------------------------------------------------------------
# Test cases
# --------------------------------------------------------------------------

def run_transfer(binpath, tmpdir, name, size, n=0, chunk=None, delay=0.0):
    """Generic unidirectional integrity test: file sink, sha256 comparison."""
    dev = os.path.join(tmpdir, "printer-%s" % name)
    open(dev, "wb").close()
    d = Daemon(binpath, dev, n)
    try:
        if not d.wait_ready():
            record(name, False, "daemon did not start; log=%r" % d.log()[:400])
            return
        data = payload(size)
        client_send(data, d.port)
        got = read_file_bytes(dev, size)
        if len(got) != size:
            record(name, False, "printer got %d bytes, expected %d"
                   % (len(got), size))
            return
        if sha(got) != sha(data):
            record(name, False, "sha256 mismatch (%s vs %s)"
                   % (sha(got)[:16], sha(data)[:16]))
            return
        record(name, True)
    finally:
        d.kill()


def t_transfer_1byte(binpath, tmpdir):
    run_transfer(binpath, tmpdir, "transfer_1byte", 1, n=0)


def t_boundaries(binpath, tmpdir):
    """Ring-buffer wrap boundaries around BUFFER_SIZE (8192)."""
    for size in (8191, 8192, 8193):
        run_transfer(binpath, tmpdir, "boundary_%d" % size, size, n=0)


def t_transfer_1mb(binpath, tmpdir):
    run_transfer(binpath, tmpdir, "transfer_1mb_sha256", 1024 * 1024, n=0)


def t_slow_chunks(binpath, tmpdir):
    run_transfer(binpath, tmpdir, "transfer_slow_chunks", 20000, n=0,
                 chunk=137, delay=0.002)


def t_half_close_tail(binpath, tmpdir):
    """Client closes only its write side; the tail must still be delivered."""
    size = 65536
    dev = os.path.join(tmpdir, "printer-halfclose")
    open(dev, "wb").close()
    d = Daemon(binpath, dev, 0)
    try:
        if not d.wait_ready():
            record("half_close_tail_delivered", False, "daemon did not start")
            return
        data = payload(size)
        client_send(data, d.port, half_close=True)
        got = read_file_bytes(dev, size)
        if len(got) != size or sha(got) != sha(data):
            record("half_close_tail_delivered", False,
                   "tail lost: got %d/%d bytes" % (len(got), size))
            return
        record("half_close_tail_delivered", True)
    finally:
        d.kill()


def t_client_rst_midtransfer(binpath, tmpdir):
    """Client aborts with an RST mid-transfer; daemon must survive."""
    dev = os.path.join(tmpdir, "printer-rst")
    open(dev, "wb").close()
    d = Daemon(binpath, dev, 0)
    try:
        if not d.wait_ready():
            record("client_rst_daemon_survives", False, "daemon did not start")
            return
        data = payload(300000)
        try:
            client_send(data, d.port, chunk=1024, delay=0.001,
                        rst=True, abort_after=0.4)
        except socket.error:
            pass
        time.sleep(1.0)
        if not d.alive():
            record("client_rst_daemon_survives", False,
                   "daemon died after RST; log=%r" % d.log()[:400])
            return
        # The daemon must still serve the next job.  Empty the sink first:
        # each job re-opens the device and writes from offset 0.
        reset_sink(dev)
        size = 4096
        data2 = payload(size)
        client_send(data2, d.port)
        got = read_file_bytes(dev, size, timeout=10.0)
        if len(got) != size:
            record("client_rst_daemon_survives", False,
                   "follow-up job incomplete: got %d bytes, expected %d"
                   % (len(got), size))
            return
        if got != data2:
            record("client_rst_daemon_survives", False,
                   "follow-up job content mismatch")
            return
        record("client_rst_daemon_survives", True)
    finally:
        d.kill()


def t_client_disconnect_midtransfer(binpath, tmpdir):
    """Client vanishes mid-transfer; daemon must recover and serve again."""
    dev = os.path.join(tmpdir, "printer-disconnect")
    open(dev, "wb").close()
    d = Daemon(binpath, dev, 0)
    try:
        if not d.wait_ready():
            record("client_disconnect_recovers", False, "daemon did not start")
            return
        try:
            client_send(payload(400000), d.port, chunk=512, delay=0.001,
                        abort_after=0.3)
        except socket.error:
            pass
        time.sleep(0.8)
        if not d.alive():
            record("client_disconnect_recovers", False, "daemon died")
            return
        reset_sink(dev)
        size = 2048
        data = payload(size)
        client_send(data, d.port)
        got = read_file_bytes(dev, size, timeout=10.0)
        if len(got) != size:
            record("client_disconnect_recovers", False,
                   "follow-up job incomplete: got %d bytes, expected %d"
                   % (len(got), size))
            return
        if got != data:
            record("client_disconnect_recovers", False,
                   "follow-up job content mismatch")
            return
        record("client_disconnect_recovers", True)
    finally:
        d.kill()


def t_sequential_jobs(binpath, tmpdir):
    """Several jobs in sequence over separate connections."""
    name = "sequential_jobs"
    for num in range(3):
        size = 1000 * (num + 1)
        run_transfer(binpath, tmpdir, "%s_%d" % (name, num), size, n=0)


def t_simultaneous_two(binpath, tmpdir):
    """Two simultaneously opened connections must both be served."""
    dev = os.path.join(tmpdir, "printer-simul")
    open(dev, "wb").close()
    d = Daemon(binpath, dev, 0)
    name = "simultaneous_two_connections"
    try:
        if not d.wait_ready():
            record(name, False, "daemon did not start")
            return
        a = payload(5000)
        b = payload(7000)
        got_b = b""
        sa = socket.create_connection((IPV4_HOST, d.port), timeout=20)
        sb = socket.create_connection((IPV4_HOST, d.port), timeout=20)
        try:
            sa.sendall(a)
            sa.shutdown(socket.SHUT_WR)
            # Drain A to completion first (the daemon serves one job at a
            # time, so B stays queued in the listen backlog meanwhile).
            deadline = time.time() + 20.0
            while time.time() < deadline:
                if not sa.recv(4096):
                    break
            got_a = read_file_bytes(dev, len(a))
            if got_a != a:
                record(name, False, "first job corrupted (%d/%d bytes)"
                       % (len(got_a), len(a)))
                return
            sa.close()
            # Empty the sink: job B re-opens the device from offset 0.
            reset_sink(dev)
            sb.sendall(b)
            sb.shutdown(socket.SHUT_WR)
            deadline = time.time() + 20.0
            while time.time() < deadline:
                if not sb.recv(4096):
                    break
            got_b = read_file_bytes(dev, len(b))
        finally:
            for s in (sa, sb):
                try:
                    s.close()
                except Exception:
                    pass
        if got_b != b:
            record(name, False, "second job corrupted (%d/%d bytes)"
                   % (len(got_b), len(b)))
            return
        record(name, True)
    finally:
        d.kill()


def t_bidir_response(binpath, tmpdir):
    """Bidirectional: printer -> network response must arrive intact."""
    name = "bidir_printer_response_integrity"
    master, slave = os.openpty()
    try:
        tty.setraw(master)
        tty.setraw(slave)
    except (termios.error, AttributeError):
        os.close(master)
        os.close(slave)
        skip(name, "cannot configure pty")
        return
    dev = os.ttyname(slave)
    d = Daemon(binpath, dev, 0, bidir=True)
    try:
        if not d.wait_ready():
            record(name, False, "daemon did not start; log=%r" % d.log()[:400])
            return
        job = payload(4096)
        resp = b"PRINTER-REPLY-0123456789abcdef"
        deadline = time.time() + 20.0
        got = b""
        s = socket.create_connection((IPV4_HOST, d.port), timeout=20)
        try:
            s.sendall(job)
            s.shutdown(socket.SHUT_WR)
            # Read the job off the pty master and answer.
            while len(got) < len(job) and time.time() < deadline:
                import select as pyselect
                r, _, _ = pyselect.select([master], [], [], 0.5)
                if r:
                    got += os.read(master, 65536)
            if sha(got) != sha(job):
                record(name, False, "printer received %d/%d bytes"
                       % (len(got), len(job)))
                return
            os.write(master, resp)
            received = b""
            s.settimeout(20.0)
            while True:
                try:
                    piece = s.recv(65536)
                except socket.timeout:
                    break
                if not piece:
                    break
                received += piece
                if len(received) >= len(resp):
                    break
        finally:
            try:
                s.close()
            except Exception:
                pass
        if received != resp:
            record(name, False, "expected %r, got %r" % (resp, received[:64]))
            return
        record(name, True)
    finally:
        d.kill()
        for fd in (master, slave):
            try:
                os.close(fd)
            except OSError:
                pass


def t_ipv6(binpath, tmpdir):
    name = "ipv6_loopback_transfer"
    dev = os.path.join(tmpdir, "printer-ipv6")
    open(dev, "wb").close()
    d = Daemon(binpath, dev, 0, ipv6=True)
    try:
        if not d.wait_ready():
            skip(name, "no usable IPv6 loopback (%r)" % d.log()[:200])
            return
        size = 30000
        data = payload(size)
        client_send(data, d.port, ipv6=True)
        got = read_file_bytes(dev, size)
        if len(got) != size or sha(got) != sha(data):
            record(name, False, "got %d/%d bytes" % (len(got), size))
            return
        record(name, True)
    finally:
        d.kill()


def t_no_zombies(binpath, tmpdir):
    """After the daemon exits it must leave no child processes behind."""
    name = "no_residual_child_processes"
    dev = os.path.join(tmpdir, "printer-zombie")
    open(dev, "wb").close()
    d = Daemon(binpath, dev, 0)
    try:
        if not d.wait_ready():
            record(name, False, "daemon did not start")
            return
        data = payload(4096)
        client_send(data, d.port)
        read_file_bytes(dev, len(data))
        pid = d.proc.pid
        children = []
        try:
            with open("/proc/%d/task/%d/children" % (pid, pid), "r") as f:
                children = f.read().split()
        except IOError:
            children = []
        if children:
            record(name, False, "daemon has live children: %s" % children)
            return
        record(name, True)
    finally:
        d.kill()


def t_lock_dir_created(binpath, tmpdir):
    """Debian #634225: a missing lock directory must not stop the daemon.

    Uses a purpose-built binary whose compiled-in LOCKFILE_DIR does not exist
    yet, passed in as DEEP_LOCK_BIN.
    """
    name = "lock_dir_created_automatically"
    if not DEEP_LOCK_BIN:
        skip(name, "no deep-lock binary supplied (--bin-deeplock)")
        return
    dev = os.path.join(tmpdir, "printer-lockdir")
    open(dev, "wb").close()
    d = Daemon(DEEP_LOCK_BIN, dev, 0)
    try:
        started = d.wait_ready(timeout=5.0)
        if not started:
            record(name, False,
                   "daemon refused to start with a missing lock directory"
                   " (log=%r)" % d.log()[:300])
            return
        record(name, True)
    finally:
        d.kill()


def t_printer_stall_no_spin(binpath, tmpdir):
    """A blocked printer must make the daemon sleep, not burn CPU."""
    name = "printer_stall_no_cpu_spin"
    fifo = os.path.join(tmpdir, "printer-fifo-stall")
    os.mkfifo(fifo)
    # Open the read side but never read: the pipe will fill up and the
    # daemon's writes will return EAGAIN.
    rfd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
    d = Daemon(binpath, fifo, 0)
    try:
        if not d.wait_ready():
            record(name, False, "daemon did not start")
            return
        before = d.cpu_seconds()
        if before < 0:
            skip(name, "cannot read CPU time")
            return
        # Push much more than the pipe + ring buffer can absorb.
        try:
            client_send(payload(1024 * 1024), d.port, recv_timeout=2.0)
        except socket.error:
            pass
        time.sleep(2.5)
        after = d.cpu_seconds()
        if after < 0:
            skip(name, "daemon vanished")
            return
        used = after - before
        if used > 1.0:
            record(name, False, "burned %.2fs CPU while blocked on the printer"
                   % used)
            return
        record(name, True)
    finally:
        d.kill()
        try:
            os.close(rfd)
        except OSError:
            pass


def t_printer_disappears(binpath, tmpdir):
    """The printer going away mid-job must not kill the daemon."""
    name = "printer_disappears_midjob"
    fifo = os.path.join(tmpdir, "printer-fifo-gone")
    os.mkfifo(fifo)
    rfd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
    d = Daemon(binpath, fifo, 0)
    try:
        if not d.wait_ready():
            record(name, False, "daemon did not start")
            return
        # Read a little, then close: further writes get EPIPE.
        s = socket.create_connection((IPV4_HOST, d.port), timeout=10)
        try:
            s.sendall(payload(600000))
            s.shutdown(socket.SHUT_WR)
        except socket.error:
            pass
        deadline = time.time() + 3.0
        try:
            while time.time() < deadline:
                piece = os.read(rfd, 4096)
                if not piece:
                    break
        except OSError as e:
            if e.errno != errno.EAGAIN:
                pass
        os.close(rfd)
        rfd = -1
        try:
            s.settimeout(5.0)
            while s.recv(4096):
                pass
        except socket.error:
            pass
        finally:
            try:
                s.close()
            except Exception:
                pass
        time.sleep(0.5)
        if not d.alive():
            record(name, False,
                   "daemon died when the printer disappeared; log=%r"
                   % d.log()[:300])
            return
        # A new job must still be accepted (to the same, now-broken device the
        # daemon will keep retrying, so just check it is still listening).
        try:
            c = socket.create_connection((IPV4_HOST, d.port), timeout=5)
            c.close()
        except socket.error as e:
            record(name, False, "not accepting connections: %s" % e)
            return
        record(name, True)
    finally:
        d.kill()
        if rfd >= 0:
            try:
                os.close(rfd)
            except OSError:
                pass


def _run_raw(binpath, args, timeout=5.0):
    """Run the binary to completion and return (returncode, combined output)."""
    import subprocess as _sp
    try:
        p = _sp.run([binpath] + list(args), stdout=_sp.PIPE,
                    stderr=_sp.STDOUT, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except Exception as e:  # noqa: BLE001
        return None, "exception: %r" % (e,)


def t_idle_timeout_disconnects(binpath, tmpdir):
    """-t N must tear the job down after N idle seconds (bidirectional mode)."""
    name = "idle_timeout_option_disconnects"
    dev = os.path.join(tmpdir, "printer-idle")

    open(dev, "wb").close()
    d = Daemon(binpath, dev, 0, bidir=True, extra=["-t", "1"])
    try:
        if not d.wait_ready():
            record(name, False, "daemon did not start")
            return
        s = socket.create_connection((IPV4_HOST, d.port), timeout=20)
        started = time.time()
        closed = False
        try:
            # Send nothing: the job must expire on its own.
            while True:
                piece = s.recv(4096)
                if not piece:
                    closed = True
                    break
        except socket.error:
            closed = True
        elapsed = time.time() - started
        try:
            s.close()
        except Exception:
            pass
        if not closed:
            record(name, False, "connection was never closed")
            return
        if elapsed < 0.8:
            # Closing instantly would mean the idle window was not honoured.
            record(name, False,
                   "closed after %.2fs, before the 1s idle window elapsed"
                   % elapsed)
            return
        # Bound is deliberately below the 5s compile-time default: if -t were
        # ignored the job would only end after ~5s and this would fail.
        if elapsed > 3.5:
            record(name, False,
                   "took %.2fs for a 1s idle timeout (-t appears ignored)"
                   % elapsed)
            return
        record(name, True)
    finally:
        d.kill()


def t_idle_timeout_zero_keeps_open(binpath, tmpdir):
    """-t 0 must disable the idle timer: an idle client is not dropped."""
    name = "idle_timeout_zero_keeps_connection"
    dev = os.path.join(tmpdir, "printer-idle0")
    open(dev, "wb").close()
    d = Daemon(binpath, dev, 0, bidir=True, extra=["-t", "0"])
    try:
        if not d.wait_ready():
            record(name, False, "daemon did not start")
            return
        s = socket.create_connection((IPV4_HOST, d.port), timeout=20)
        s.settimeout(6.0)
        still_open = True
        try:
            if not s.recv(4096):
                still_open = False
        except socket.timeout:
            still_open = True
        except socket.error:
            still_open = False
        try:
            s.close()
        except Exception:
            pass
        if not still_open:
            record(name, False,
                   "connection closed even though the idle timer was disabled")
            return
        record(name, True)
    finally:
        d.kill()


def t_invalid_idle_timeout_rejected(binpath, tmpdir):
    """Non-numeric, negative and overflowing -t values must be refused."""
    name = "invalid_idle_timeout_rejected"
    dev = os.path.join(tmpdir, "printer-bad-t")
    open(dev, "wb").close()
    for bad in ("abc", "-1", "999999999999999999999", ""):
        rc, out = _run_raw(binpath, ["-d", "-f", dev, "-t", bad, "0"],
                           timeout=5.0)
        if rc is None:
            record(name, False, "'-t %s' did not terminate (%s)" % (bad, out))
            return
        if rc == 0:
            record(name, False, "'-t %s' was accepted (rc=0)" % bad)
            return
        if "invalid idle timeout" not in out:
            record(name, False,
                   "'-t %s' rejected without a clear message: %r"
                   % (bad, out[:120]))
            return
    record(name, True)


def _pid_path():
    return os.path.join(PIDFILE_DIR, "p9100d.pid") if PIDFILE_DIR else ""


def t_pidfile_removed_on_termination(binpath, tmpdir):
    """A pid file must be written while running and removed when we stop."""
    name = "pidfile_removed_on_termination"
    if not PIDFILE_DIR:
        skip(name, "no --pidfile-dir supplied")
        return
    dev = os.path.join(tmpdir, "printer-pidterm")
    open(dev, "wb").close()
    pidpath = _pid_path()
    if os.path.lexists(pidpath):
        os.unlink(pidpath)
    # No -d: run backgrounded, which is the only mode that uses a pid file.
    d = Daemon(binpath, dev, 0, foreground=False)
    try:
        try:
            d.proc.wait(timeout=10)  # starter forks and exits
        except Exception:
            pass
        pid = d.wait_pidfile(pidpath)
        if pid < 0:
            record(name, False, "pid file %s was not created" % pidpath)
            return
        if not os.path.exists("/proc/%d" % pid):
            record(name, False, "pid file names a non-existent process %d" % pid)
            return
        os.kill(pid, signal.SIGTERM)
        if not Daemon.wait_pid_gone(pid, timeout=10.0):
            record(name, False, "daemon ignored SIGTERM")
            return
        if os.path.exists(pidpath):
            record(name, False,
                   "pid file survived termination (stale pid, later reused)")
            return
        record(name, True)
    finally:
        d.kill()


def t_pidfile_symlink_refused(binpath, tmpdir):
    """A symlinked pid file must not be followed, so its target is safe."""
    name = "pidfile_symlink_refused"
    if not PIDFILE_DIR:
        skip(name, "no --pidfile-dir supplied")
        return
    dev = os.path.join(tmpdir, "printer-pidsym")
    open(dev, "wb").close()
    victim = os.path.join(tmpdir, "victim")
    precious = "precious\n"
    with open(victim, "w") as f:
        f.write(precious)
    pidpath = _pid_path()
    if os.path.lexists(pidpath):
        os.unlink(pidpath)
    os.symlink(victim, pidpath)
    d = Daemon(binpath, dev, 0, foreground=False)
    try:
        # The starter always forks and exits 0, so its status says nothing;
        # what matters is whether the daemon came up and what happened to the
        # file the symlink points at.
        try:
            d.proc.wait(timeout=10)
        except Exception:
            pass
        time.sleep(1.5)
        with open(victim, "r") as f:
            content = f.read()
        if content != precious:
            record(name, False,
                   "followed the symlink and clobbered its target (%r)"
                   % content[:40])
            return
        try:
            s = socket.create_connection((IPV4_HOST, 9100), timeout=1.0)
            s.close()
            record(name, False,
                   "daemon started anyway with a symlinked pid file")
            return
        except socket.error:
            pass  # refused, as required
        record(name, True)
    finally:
        # If the daemon did start (which is the failure we are looking for) it
        # left no pid file to track, so reap it by its unique device path.
        try:
            subprocess.run(["pkill", "-9", "-f", dev], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=10)
        except Exception:
            pass
        d.kill()
        if os.path.lexists(pidpath):
            try:
                os.unlink(pidpath)
            except OSError:
                pass


def t_privilege_drop(binpath, tmpdir):
    """-u/-g must be accepted and the daemon must still serve a job."""
    name = "privilege_drop_to_current_identity"
    try:
        import pwd
        import grp
        uname = pwd.getpwuid(os.getuid()).pw_name
        gname = grp.getgrgid(os.getgid()).gr_name
    except Exception as e:  # noqa: BLE001
        skip(name, "cannot resolve current identity (%r)" % (e,))
        return
    dev = os.path.join(tmpdir, "printer-privdrop")
    open(dev, "wb").close()
    d = Daemon(binpath, dev, 0, extra=["-u", uname, "-g", gname])
    try:
        if not d.wait_ready():
            record(name, False, "daemon did not start with -u/-g; log=%r"
                   % d.log()[:300])
            return
        size = 20000
        data = payload(size)
        client_send(data, d.port)
        got = read_file_bytes(dev, size)
        if got != data:
            record(name, False,
                   "job corrupted after dropping privileges (%d/%d bytes)"
                   % (len(got), size))
            return
        if "running as uid=" not in d.log():
            record(name, False,
                   "no evidence privileges were actually dropped; log=%r"
                   % d.log()[:300])
            return
        record(name, True)
    finally:
        d.kill()


def t_unknown_privilege_targets_rejected(binpath, tmpdir):
    """-u/-g naming a non-existent account must be refused, not ignored."""
    name = "unknown_user_or_group_rejected"
    dev = os.path.join(tmpdir, "printer-baduser")
    open(dev, "wb").close()
    rc, out = _run_raw(binpath,
                       ["-d", "-f", dev, "-u", "nosuchuser_zzz", "0"],
                       timeout=5.0)
    if rc is None:
        record(name, False, "did not terminate (%s)" % out[:120])
        return
    if rc == 0:
        record(name, False, "bogus -u was accepted")
        return
    if "unknown user" not in out:
        record(name, False, "unexpected message for bogus -u: %r" % out[:160])
        return
    rc, out = _run_raw(binpath,
                       ["-d", "-f", dev, "-g", "nosuchgroup_zzz", "0"],
                       timeout=5.0)
    if rc == 0 or "unknown group" not in out:
        record(name, False, "bogus -g not handled (rc=%r, out=%r)"
               % (rc, out[:160]))
        return
    record(name, True)


def t_missing_printer_not_accepted_first(binpath, tmpdir):
    """A missing printer must not let a connection be accepted and then hang."""
    name = "missing_printer_not_accepted_first"
    dev = os.path.join(tmpdir, "late-printer")
    # Deliberately not created yet.
    d = Daemon(binpath, dev, 0)
    try:
        if not d.wait_ready():
            record(name, False, "daemon did not start with a missing device")
            return
        size = 4096
        data = payload(size)
        s = socket.create_connection((IPV4_HOST, d.port), timeout=30)
        try:
            s.sendall(data)
            s.shutdown(socket.SHUT_WR)
            # While the device is absent nothing may be accepted: an accepted
            # connection the daemon cannot serve is exactly the old behaviour.
            time.sleep(2.0)
            # NB: match a distinctive log phrase, not "accepted" alone --
            # this case's scratch directory name contains that word.
            if "Connection from" in d.log():
                record(name, False,
                       "connection was accepted while the printer was missing"
                       " (log=%r)" % d.log()[:200])
                return
            if os.path.exists(dev):
                record(name, False, "device appeared unexpectedly")
                return
            # Make the printer available; the job must then complete.
            with open(dev, "wb"):
                pass
            got = read_file_bytes(dev, size, timeout=40.0)
        finally:
            try:
                s.close()
            except Exception:
                pass
        if got != data:
            record(name, False,
                   "job not served after the printer appeared (%d/%d bytes)"
                   % (len(got), size))
            return
        if "Connection from" not in d.log():
            record(name, False, "connection never accepted once the device"
                                " existed (log=%r)" % d.log()[:200])
            return
        record(name, True)
    finally:
        d.kill()


def t_device_allowlist(binpath, tmpdir):
    """A build with DEVICE_ALLOWLIST must refuse devices outside the list."""
    name = "device_allowlist_enforced"
    if not ALLOW_BIN:
        skip(name, "no allowlist build supplied (--bin-allowlist)")
        return
    sink = os.path.join(tmpdir, "sink")
    open(sink, "wb").close()
    rc, out = _run_raw(ALLOW_BIN, ["-d", "-f", sink, "0"], timeout=5.0)
    if rc is None:
        record(name, False, "did not terminate (%s)" % out[:120])
        return
    if rc == 0:
        record(name, False, "-f outside the allowlist was accepted")
        return
    if "not permitted by this build" not in out:
        record(name, False, "unexpected message: %r" % out[:160])
        return
    # A listed device must still be accepted; /dev/lp0 need not exist for the
    # daemon to start listening (it retries the open).
    d = Daemon(ALLOW_BIN, "/dev/lp0", 0)
    try:
        if not d.wait_ready(timeout=5.0):
            record(name, False, "a listed device was refused; log=%r"
                   % d.log()[:200])
            return
    finally:
        d.kill()
    record(name, True)


def t_default_build_still_accepts_any_device(binpath, tmpdir):
    """The default build is unchanged: -f keeps pointing anywhere."""
    name = "default_build_accepts_any_device"
    sink = os.path.join(tmpdir, "sink")
    open(sink, "wb").close()
    d = Daemon(binpath, sink, 0)
    try:
        if not d.wait_ready():
            record(name, False, "default build refused an arbitrary -f device")
            return
        size = 1024
        data = payload(size)
        client_send(data, d.port)
        got = read_file_bytes(sink, size)
        if got != data:
            record(name, False, "job corrupted (%d/%d bytes)" % (len(got), size))
            return
        record(name, True)
    finally:
        d.kill()


def t_libwrap_denies(binpath, tmpdir):
    """A libwrap build must reject, close and keep serving (U5/U8)."""
    name = "libwrap_rejection_closes_connection"
    if not LIBWRAP_BIN or not LIBWRAP_STUB:
        skip(name, "no libwrap build or stub supplied")
        return
    dev = os.path.join(tmpdir, "printer-libwrap")
    open(dev, "wb").close()
    # Preloaded stub denies everything, so the daemon's rejection branch runs
    # without needing root access to /etc/hosts.deny.
    d = Daemon(LIBWRAP_BIN, dev, 0, preload=LIBWRAP_STUB)
    try:
        if not d.wait_ready():
            record(name, False, "libwrap build did not start; log=%r"
                   % d.log()[:300])
            return
        size = 4096
        data = payload(size)
        # Rejected: closed with nothing delivered.  Closing with unread data
        # pending makes the kernel send RST, so both a clean EOF and
        # ECONNRESET count as "closed without an answer".
        got = b""
        s = None
        try:
            s = socket.create_connection((IPV4_HOST, d.port), timeout=10)
            s.sendall(data)
            s.shutdown(socket.SHUT_WR)
            while True:
                piece = s.recv(4096)
                if not piece:
                    break
                got += piece
        except socket.error:
            got = b""
        finally:
            if s is not None:
                try:
                    s.close()
                except Exception:
                    pass
        if got:
            record(name, False, "denied client received %d bytes" % len(got))
            return
        time.sleep(0.5)
        with open(dev, "rb") as f:
            delivered = f.read()
        if delivered:
            record(name, False,
                   "denied client's %d bytes reached the printer"
                   % len(delivered))
            return
        if "rejected" not in d.log():
            record(name, False, "rejection not logged; log=%r" % d.log()[:300])
            return
        # Still alive and still listening after the rejection.
        try:
            s = socket.create_connection((IPV4_HOST, d.port), timeout=5)
            s.close()
        except socket.error as e:
            record(name, False, "daemon stopped serving after a rejection: %s" % e)
            return
        record(name, True)
    finally:
        d.kill()


def t_libwrap_allows(binpath, tmpdir):
    """With no rule denying it, a libwrap build must serve normally."""
    name = "libwrap_build_serves_normally"
    if not LIBWRAP_BIN:
        skip(name, "no libwrap build supplied")
        return
    dev = os.path.join(tmpdir, "printer-libwrap-ok")
    open(dev, "wb").close()
    d = Daemon(LIBWRAP_BIN, dev, 0)
    try:
        if not d.wait_ready():
            record(name, False, "libwrap build did not start; log=%r"
                   % d.log()[:300])
            return
        size = 20000
        data = payload(size)
        client_send(data, d.port)
        got = read_file_bytes(dev, size)
        if got != data:
            record(name, False, "job corrupted (%d/%d bytes)"
                   % (len(got), size))
            return
        record(name, True)
    finally:
        d.kill()


CASES = [
    t_transfer_1byte,
    t_boundaries,
    t_transfer_1mb,
    t_slow_chunks,
    t_half_close_tail,
    t_client_rst_midtransfer,
    t_client_disconnect_midtransfer,
    t_sequential_jobs,
    t_simultaneous_two,
    t_bidir_response,
    t_ipv6,
    t_no_zombies,
    t_lock_dir_created,
    t_printer_stall_no_spin,
    t_printer_disappears,
    t_idle_timeout_disconnects,
    t_idle_timeout_zero_keeps_open,
    t_invalid_idle_timeout_rejected,
    t_pidfile_removed_on_termination,
    t_pidfile_symlink_refused,
    t_privilege_drop,
    t_unknown_privilege_targets_rejected,
    t_missing_printer_not_accepted_first,
    t_device_allowlist,
    t_default_build_still_accepts_any_device,
    t_libwrap_allows,
    t_libwrap_denies,
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="./p910nd")
    ap.add_argument("--bin-deeplock", default="")
    ap.add_argument("--pidfile-dir", default="")
    ap.add_argument("--bin-allowlist", default="")
    ap.add_argument("--bin-libwrap", default="")
    ap.add_argument("--libwrap-stub", default="")
    ap.add_argument("--filter", default="")
    args = ap.parse_args()

    global DEEP_LOCK_BIN, PIDFILE_DIR, ALLOW_BIN, LIBWRAP_BIN, LIBWRAP_STUB
    DEEP_LOCK_BIN = args.bin_deeplock
    PIDFILE_DIR = args.pidfile_dir
    ALLOW_BIN = args.bin_allowlist
    LIBWRAP_BIN = args.bin_libwrap
    LIBWRAP_STUB = args.libwrap_stub
    binpath = os.path.abspath(args.bin)
    if not os.path.exists(binpath):
        print("FATAL: daemon binary %s not found" % binpath)
        return 2

    tmpdir = tempfile.mkdtemp(prefix="p910nd-func-")
    pattern = re.compile(args.filter) if args.filter else None
    for case in CASES:
        if pattern and not pattern.search(case.__name__):
            continue
        sub = os.path.join(tmpdir, case.__name__)
        os.makedirs(sub, exist_ok=True)
        try:
            case(binpath, sub)
        except Exception as e:  # noqa: BLE001 - report, never mask
            record(case.__name__, False, "exception: %r" % (e,))

    print("")
    print("functional summary: %d passed, %d failed, %d skipped"
          % (len(_passes), len(_failures), len(_skips)))
    return 1 if _failures else 0


if __name__ == "__main__":
    sys.exit(main())
