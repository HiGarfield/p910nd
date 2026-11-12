#!/usr/bin/env python3
"""Create and drive the test devices p910nd is pointed at with -f.

p910nd is a dumb pipe, so the interesting device properties are: does read()
ever return 0, does read() fail hard, and how fast can it be drained.  This
helper provides exactly those:

  file  PATH                     a regular file (detectEof=1 in p910nd)
  fifo  PATH                     a FIFO; a reader must exist for O_WRONLY
  pty   [--close-after S]        a pty pair; the slave path is printed and the
                                 master is closed after S seconds, which makes
                                 every read()/write() on the slave fail with EIO
  drain --fifo PATH --out FILE   consume a FIFO forever (opened O_RDWR so it
                                 neither blocks on open nor sees EOF) and log
                                 the byte stream with timestamps
  repl  --fifo PATH --data TEXT --delay S [--repeat N]
                                 write TEXT to a FIFO, then sleep S, N times;
                                 emits event=repl-end and a non-zero status if
                                 the reader disappears mid-run (PD-84)

Every mode prints machine readable KEY=VALUE lines on stdout.  The "t="
values are elapsed monotonic seconds, not wall-clock timestamps.
"""

import argparse
import os
import pty
import sys
import time


def do_file(args):
    with open(args.path, "wb"):
        pass
    print("device=%s" % args.path)
    return 0


def do_fifo(args):
    try:
        os.unlink(args.path)
    except FileNotFoundError:
        pass
    os.mkfifo(args.path, 0o600)
    print("device=%s" % args.path)
    return 0


def do_pty(args):
    master, slave = pty.openpty()
    name = os.ttyname(slave)
    print("device=%s" % name)
    sys.stdout.flush()
    if args.hold:
        time.sleep(args.hold)
    # PD-51: a backwards wall-clock step (NTP, VM resume, the fake clock
    # shim used by PD-07) makes "now < deadline" true for hours, so the
    # master is never closed and every read() on the slave keeps succeeding
    # instead of failing with EIO.  Deadlines are elapsed durations.
    if args.close_after is not None:
        deadline = time.monotonic() + args.close_after
        while time.monotonic() < deadline:
            time.sleep(0.05)
        print("event=master-closed", flush=True)
        os.close(master)
    deadline = time.monotonic() + (args.hold_after or 30.0)
    while time.monotonic() < deadline:
        time.sleep(0.1)
    return 0


def do_drain(args):
    fd = os.open(args.fifo, os.O_RDWR)
    if args.out:
        out = open(args.out, "wb")
    else:
        out = None
    t0 = time.monotonic()
    total = 0
    try:
        while True:
            try:
                buf = os.read(fd, 65536)
            except OSError as exc:
                print("event=read-error errno=%s" % exc)
                break
            if not buf:
                continue
            total += len(buf)
            if out:
                out.write(buf)
                out.flush()
            if args.verbose:
                print("event=read t=%.3f bytes=%d total=%d"
                      % (time.monotonic() - t0, len(buf), total))
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        print("event=drain-end total=%d elapsed=%.3f" % (total, time.monotonic() - t0))
        sys.stdout.flush()
        if out:
            out.close()
        os.close(fd)
    return 0


def do_repl(args):
    fd = os.open(args.fifo, os.O_WRONLY)
    payload = args.data.encode("utf-8").decode("unicode_escape").encode("latin-1")
    # PD-62: drain and pty already use monotonic time. An absolute
    # time.time() stamp jumps backwards under an NTP step / VM resume /
    # the fake clock used by PD-07, so the "t=" values of a single repl
    # run were neither ordered nor comparable with the drain log.
    t0 = time.monotonic()
    written = 0
    error = None
    try:
        for i in range(args.repeat):
            # PD-84: log a write only AFTER it succeeded.  Printing first
            # claimed a chunk was delivered even when the reader had gone
            # away; the resulting BrokenPipeError then escaped uncaught,
            # leaving no end event and leaking the FIFO descriptor.
            try:
                os.write(fd, payload)
            except OSError as exc:
                error = exc
                print("event=write-error t=%.3f index=%d errno=%s"
                      % (time.monotonic() - t0, i, exc.strerror))
                break
            written += 1
            print("event=write t=%.3f index=%d bytes=%d"
                  % (time.monotonic() - t0, i, len(payload)))
            sys.stdout.flush()
            if i + 1 < args.repeat:
                time.sleep(args.delay)
    finally:
        os.close(fd)
    print("event=repl-end written=%d requested=%d"
          % (written, args.repeat), flush=True)
    return 1 if error is not None else 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="mode", required=True)

    p = sub.add_parser("file")
    p.add_argument("path")
    p.set_defaults(func=do_file)

    p = sub.add_parser("fifo")
    p.add_argument("path")
    p.set_defaults(func=do_fifo)

    p = sub.add_parser("pty")
    p.add_argument("--close-after", type=float, default=None)
    p.add_argument("--hold", type=float, default=0.0,
                   help="sleep this long before closing the master")
    p.add_argument("--hold-after", type=float, default=30.0)
    p.set_defaults(func=do_pty)

    p = sub.add_parser("drain")
    p.add_argument("--fifo", required=True)
    p.add_argument("--out", default=None)
    p.add_argument("--verbose", action="store_true")
    p.set_defaults(func=do_drain)

    p = sub.add_parser("repl")
    p.add_argument("--fifo", required=True)
    p.add_argument("--data", default="CHUNK")
    p.add_argument("--delay", type=float, default=1.0)
    p.add_argument("--repeat", type=int, default=2)
    p.set_defaults(func=do_repl)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
