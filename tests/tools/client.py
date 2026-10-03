#!/usr/bin/env python3
"""Drive one client connection against a running p910nd.

The point of this helper is the *termination mode* of the connection, because
that is what tells a caller whether a job succeeded or failed:

  fin      recv() returned 0          -- clean FIN, the client is told the job
                                          printed (CUPS will not retry)
  rst      ConnectionResetError       -- the daemon sent RST, the client is told
                                          the job failed (CUPS retries)
  eof      orderly shutdown by peer
  timeout  neither happened within the deadline

It also records how many bytes crossed in each direction, and can optionally
stream the reply to a file so a case can assert on its content.

Usage:
    client.py --port 19100 --send-repeat 4096 --send-byte 0x41 \
              --half-close --expect-reply 1024 --out /tmp/reply.bin
"""

import argparse
import socket
import sys
import time


def parse_byte(text):
    return int(text, 0) & 0xFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--connect-timeout", type=float, default=5.0)
    ap.add_argument("--send", default=None, help="raw bytes to send (python escapes ok)")
    ap.add_argument("--send-repeat", type=int, default=0, help="send N copies of --send-byte")
    ap.add_argument("--send-byte", default="0x41")
    ap.add_argument("--chunk", type=int, default=0, help="send in chunks of this size")
    ap.add_argument("--chunk-delay", type=float, default=0.0)
    ap.add_argument("--half-close", action="store_true",
                    help="shutdown(SHUT_WR) after sending, like a print job ending")
    ap.add_argument("--linger", type=float, default=0.0,
                    help="seconds to keep the connection open without half-closing")
    ap.add_argument("--expect-reply", type=int, default=0,
                    help="read up to this many reply bytes (0 = do not read)")
    ap.add_argument("--read-timeout", type=float, default=8.0)
    ap.add_argument("--out", default=None, help="write received bytes here")
    ap.add_argument("--label", default="client")
    args = ap.parse_args()

    payload = b""
    if args.send is not None:
        payload = args.send.encode("utf-8").decode("unicode_escape").encode("latin-1")
    elif args.send_repeat:
        payload = bytes([parse_byte(args.send_byte)]) * args.send_repeat

    # monotonic: this tool must keep working even when a case preloads a shim
    # that fakes the wall clock
    started = time.monotonic()
    sent = 0
    received = 0
    how = "none"
    chunks = []

    try:
        sock = socket.create_connection((args.host, args.port), args.connect_timeout)
    except OSError as exc:
        print("%s: connect failed: %s" % (args.label, exc))
        print("RESULT how=connect-error sent=0 received=0 elapsed=0.000")
        return 2

    sock.settimeout(1.0)
    try:
        if payload:
            step = args.chunk if args.chunk > 0 else len(payload)
            for off in range(0, len(payload), step):
                sock.sendall(payload[off:off + step])
                sent += len(payload[off:off + step])
                if args.chunk_delay:
                    time.sleep(args.chunk_delay)
        if args.linger:
            time.sleep(args.linger)
        if args.half_close:
            sock.shutdown(socket.SHUT_WR)

        deadline = time.monotonic() + args.read_timeout
        chunks = []
        while args.expect_reply and received < args.expect_reply:
            if time.monotonic() > deadline:
                how = "timeout"
                break
            try:
                buf = sock.recv(min(65536, args.expect_reply - received))
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
            chunks.append(buf)
            received += len(buf)
        if args.expect_reply == 0 and how == "none":
            how = "no-read"
    except (BrokenPipeError, ConnectionResetError) as exc:
        how = "rst"
        print("%s: send failed: %s" % (args.label, exc))
    except OSError as exc:
        how = "oserror:%s" % exc
    finally:
        if args.out is not None:
            with open(args.out, "wb") as fh:
                fh.write(b"".join(chunks))
        try:
            sock.close()
        except OSError:
            pass

    print("%s: sent=%d received=%d" % (args.label, sent, received))
    print("RESULT how=%s sent=%d received=%d elapsed=%.3f"
          % (how, sent, received, time.monotonic() - started))
    if how in ("rst", "connect-error"):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
