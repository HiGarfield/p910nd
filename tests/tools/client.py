#!/usr/bin/env python3
"""Drive one client connection against a running p910nd.

The point of this helper is the *termination mode* of the connection, because
that is what tells a caller whether a job succeeded or failed:

  fin      recv() returned 0          -- clean FIN, the client is told the job
                                          printed (CUPS will not retry)
  rst      ConnectionResetError       -- the daemon sent RST, the client is told
                                          the job failed (CUPS retries)
  eof      orderly shutdown by peer
  complete all --expect-reply bytes arrived while the peer stays open
  no-read  no reply was requested
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
    ap.add_argument("--read-timeout", type=float, default=8.0,
                    help="seconds to wait for the reply, also honoured for "
                         "short values (the socket timeout follows the deadline)")
    ap.add_argument("--out", default=None, help="write received bytes here")
    ap.add_argument("--label", default="client")
    args = ap.parse_args()

    payload = b""
    try:
        if args.send is not None:
            payload = (args.send.encode("utf-8")
                       .decode("unicode_escape").encode("latin-1"))
        elif args.send_repeat:
            payload = bytes([parse_byte(args.send_byte)]) * args.send_repeat
    except (UnicodeEncodeError, UnicodeDecodeError, ValueError) as exc:
        # PD-88: --send accepts python escapes, but \uXXXX may decode to a
        # code point above 255, and --send-byte may not be a number at all.
        # Both used to raise out of main() before any socket work: no RESULT
        # line, exit 1 looking like a failed print job (PD-48/PD-83).
        print("%s: bad send argument: %s" % (args.label, exc))
        print("RESULT how=connect-error:bad-send sent=0 received=0 elapsed=0.000")
        return 2

    # monotonic: this tool must keep working even when a case preloads a shim
    # that fakes the wall clock
    started = time.monotonic()
    sent = 0
    received = 0
    how = "none"
    chunks = []
    out_error = None

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
                # PD-57: the fixed 1.0 s socket timeout made a short
                # --read-timeout meaningless (0.2 s took 1.003 s). Wake up
                # no later than the deadline, still capped at one second so
                # a long read keeps polling the loop.
                sock.settimeout(min(
                    1.0, max(0.01, deadline - time.monotonic())))
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
        if how == "none":
            # PD-54: the loop also ends when every expected byte arrived
            # while the peer still holds the connection open.  Leaving how
            # as "none" reported an undocumented termination mode, so a
            # case checking how= could not tell a complete reply from a
            # broken one.
            how = ("no-read" if args.expect_reply == 0 else "complete")
    except (BrokenPipeError, ConnectionResetError) as exc:
        how = "rst"
        print("%s: send failed: %s" % (args.label, exc))
    except OSError as exc:
        how = "oserror:%s" % exc
    finally:
        # PD-83: close the socket FIRST.  Writing --out used to be the first
        # statement here; an unwritable path raised straight out of the
        # finally block, so no RESULT line was printed, the exit status was
        # indistinguishable from a failed print job, and the connection was
        # leaked.
        try:
            sock.close()
        except OSError:
            pass

    if args.out is not None:
        try:
            with open(args.out, "wb") as fh:
                fh.write(b"".join(chunks))
        except OSError as exc:
            out_error = "%s: cannot write --out %s: %s" % (
                args.label, args.out, exc)
            print(out_error)

    print("%s: sent=%d received=%d" % (args.label, sent, received))
    print("RESULT how=%s sent=%d received=%d elapsed=%.3f"
          % (how, sent, received, time.monotonic() - started))
    # PD-83: a tool that could not save the reply is not a failed print job.
    # Give it a distinct status (as connect-error already has) so a case
    # cannot silently pass on a missing file; RESULT is printed either way.
    if out_error is not None:
        return 2
    # A timeout or an OS error means the daemon never confirmed the job:
    # reporting success (exit 0) lets a case that checks only the exit
    # status pass without a single byte of confirmation.  Only a clean FIN,
    # an orderly EOF or "no read requested" are successes (PD-48).
    if how in ("rst", "connect-error", "timeout") or how.startswith("oserror:"):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
