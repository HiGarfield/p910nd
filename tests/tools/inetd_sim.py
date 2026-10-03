#!/usr/bin/env python3
"""Reproduce the (x)inetd invocation of p910nd and report the client's view.

(x)inetd hands the accepted connection to the service on fd 0, 1 *and* 2 --
all three descriptors are the same socket.  p910nd decides between "I am a
daemon, listen on a port" (server()) and "I am serving one connection"
(one_job()) from is_standalone(), plus an unconditional `log_to_stdout ||`.

This helper sets up exactly that fd layout and then reports, from the client's
side:

  * how many bytes the service pushed back and what they look like
  * how the connection ended: fin (clean close) / rst / timeout
  * whether the printer device actually received the job

The client connection is established before the fork, so the process stays
single threaded and the fork is safe.

Usage:
    inetd_sim.py --port 19199 --target ./p910nd --send-repeat 512 --device /tmp/out.bin \\
                 -- -i 127.0.0.1 0
"""

import argparse
import os
import socket
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True,
                    help="port the fake inetd listens on (any free port)")
    ap.add_argument("--target", required=True)
    ap.add_argument("--send-repeat", type=int, default=0)
    ap.add_argument("--send-byte", default="0x41")
    ap.add_argument("--half-close", action="store_true")
    ap.add_argument("--expect-reply", type=int, default=65536)
    ap.add_argument("--read-timeout", type=float, default=8.0)
    ap.add_argument("--device", default=None)
    ap.add_argument("--preview", type=int, default=200)
    ap.add_argument("--extra-connect", type=int, default=0,
                    help="after sending, open a second connection to this port; "
                         "used to make a daemon that logs to stdout emit a line")
    ap.add_argument("--extra-delay", type=float, default=0.5)
    ap.add_argument("--extra-send", type=int, default=1)
    ap.add_argument("argv", nargs="*", help="arguments after -- for the target")
    args = ap.parse_args()
    send_byte = int(args.send_byte, 0) & 0xFF

    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.host, args.port))
    listener.listen(1)

    # The client connects first: the kernel completes the handshake into the
    # accept queue, so no second thread is needed to drive it.
    try:
        client = socket.create_connection((args.host, args.port), 5.0)
    except OSError as exc:
        print("RESULT how=connect-error:%s" % exc)
        return 2
    client.settimeout(0.5)

    listener.settimeout(10.0)
    try:
        conn, _peer = listener.accept()
    except socket.timeout:
        print("RESULT how=accept-timeout")
        return 2
    finally:
        listener.close()

    pid = os.fork()
    if pid == 0:
        # child: become the service with the socket on fd 0, 1 and 2
        try:
            os.dup2(conn.fileno(), 0)
            os.dup2(conn.fileno(), 1)
            os.dup2(conn.fileno(), 2)
            if conn.fileno() > 2:
                conn.close()
            os.execv(args.target, [args.target] + args.argv)
        except Exception as exc:  # noqa: BLE001
            os.write(2, ("exec failed: %s\n" % exc).encode())
        os._exit(127)

    conn.close()

    sent = 0
    how = "open"
    got = b""
    payload = bytes([send_byte]) * args.send_repeat
    try:
        if payload:
            client.sendall(payload)
            sent = len(payload)
        if args.half_close:
            client.shutdown(socket.SHUT_WR)
        if args.extra_connect:
            # A second client makes a daemon that logs to stdout produce a log
            # line; where that line lands is the point of the test.
            time.sleep(args.extra_delay)
            try:
                extra = socket.create_connection((args.host, args.extra_connect), 5.0)
                if args.extra_send:
                    extra.sendall(b"X" * args.extra_send)
                    extra.shutdown(socket.SHUT_WR)
                extra.settimeout(1.0)
                try:
                    extra.recv(16)
                except OSError:
                    pass
                extra.close()
                print("extra_connect=sent")
            except OSError as exc:
                print("extra_connect=failed %s" % exc)
        deadline = time.time() + args.read_timeout
        while len(got) < args.expect_reply and time.time() < deadline:
            try:
                buf = client.recv(65536)
            except socket.timeout:
                continue
            except ConnectionResetError:
                how = "rst"
                break
            except OSError as exc:
                how = "oserror:%s" % exc
                break
            if not buf:
                how = "fin"
                break
            got += buf
        if how == "open":
            how = "timeout"
    except (BrokenPipeError, ConnectionResetError):
        how = "rst"
    except OSError as exc:
        how = "oserror:%s" % exc
    finally:
        try:
            client.close()
        except OSError:
            pass

    status = None
    deadline = time.time() + 5.0
    while time.time() < deadline:
        wpid, wstatus = os.waitpid(pid, os.WNOHANG)
        if wpid == pid:
            status = os.waitstatus_to_exitcode(wstatus)
            break
        time.sleep(0.05)
    else:
        os.kill(pid, 15)
        try:
            os.waitpid(pid, 0)
        except OSError:
            pass
        status = "killed-after-timeout"

    dev_bytes = ""
    if args.device and os.path.exists(args.device):
        dev_bytes = str(os.path.getsize(args.device))

    printable = "".join(chr(c) if 32 <= c < 127 else "." for c in got[:args.preview])
    print("device=%s" % (args.device or "-"))
    print("device_bytes=%s" % (dev_bytes or "-"))
    print("client_sent=%d" % sent)
    print("client_received=%d" % len(got))
    print("client_received_preview=%s" % printable)
    print("RESULT how=%s received=%d exit=%s" % (how, len(got), status))
    return 0


if __name__ == "__main__":
    sys.exit(main())
