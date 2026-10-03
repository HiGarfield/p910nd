#!/usr/bin/env python3
"""Count how many times a byte stream changes value.

Used to decide whether two print jobs reached one device one after the other
(the stream is then A...A B...B, i.e. a single change) or interleaved (many
changes).  Kept as a file rather than an inline heredoc so the cases stay
readable and the shell quoting cannot go wrong.
"""

import sys


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: stream_transitions.py FILE\n")
        return 2
    try:
        with open(sys.argv[1], "rb") as fh:
            data = fh.read()
    except OSError as exc:
        sys.stderr.write("cannot read %s: %s\n" % (sys.argv[1], exc))
        return 1
    prev = None
    changes = 0
    for byte in data:
        if prev is not None and byte != prev:
            changes += 1
        prev = byte
    print("%d %d" % (changes, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
