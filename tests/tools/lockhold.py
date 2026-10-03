#!/usr/bin/env python3
"""Hold (or inspect) a byte range of the p910nd lock file.

p910nd uses fcntl() record locks on one file:

    byte 0   the instance lock, held for the daemon's whole lifetime
    byte 1   the job lock, taken by every job child

This helper can pre-take either range so a case can create the "one job is
stuck, everybody else queues behind it" situation, and it reports whether the
lock could be taken at all -- which is how PD-09 asks "can an unprivileged
process take this lock?".

Usage:
    lockhold.py hold  --file /path/p9100d --byte 1 [--seconds 3600]
    lockhold.py probe --file /path/p9100d --byte 0
"""

import argparse
import fcntl
import os
import sys
import time


def take_lock(path, byte_offset, seconds):
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o666)
    fcntl.lockf(fd, fcntl.LOCK_EX, 1, byte_offset, os.SEEK_SET)
    sys.stdout.write("LOCKED file=%s byte=%d fd=%d mode=%s\n"
                     % (path, byte_offset, fd, oct(os.fstat(fd).st_mode & 0o7777)))
    sys.stdout.flush()
    deadline = time.time() + seconds if seconds > 0 else None
    while True:
        if deadline is not None and time.time() >= deadline:
            break
        time.sleep(0.2)
    return 0


def probe(path, byte_offset):
    if not os.path.exists(path):
        print("MISSING file=%s" % path)
        return 2
    st = os.stat(path)
    print("file=%s mode=%s uid=%d gid=%d size=%d"
          % (path, oct(st.st_mode & 0o7777), st.st_uid, st.st_gid, st.st_size))
    try:
        fd = os.open(path, os.O_RDWR)
    except OSError as exc:
        print("open_rdwr=denied errno=%s" % exc.strerror)
        return 1
    try:
        fcntl.lockf(fd, fcntl.LOCK_EX, 1, byte_offset, os.SEEK_SET)
        print("open_rdwr=ok lock=acquired byte=%d" % byte_offset)
        fcntl.lockf(fd, fcntl.LOCK_UN, 1, byte_offset, os.SEEK_SET)
        return 0
    except OSError as exc:
        print("open_rdwr=ok lock=denied errno=%s" % exc.strerror)
        return 1
    finally:
        os.close(fd)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["hold", "probe"])
    ap.add_argument("--file", required=True)
    ap.add_argument("--byte", type=int, default=0)
    ap.add_argument("--seconds", type=float, default=0.0)
    args = ap.parse_args()

    if args.mode == "hold":
        return take_lock(args.file, args.byte, args.seconds)
    return probe(args.file, args.byte)


if __name__ == "__main__":
    sys.exit(main())
