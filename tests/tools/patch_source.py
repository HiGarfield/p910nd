#!/usr/bin/env python3
"""Apply the test-only patches to a *copy* of p910nd.c.

The production source file must never be edited: the defect cases that need a
different constant (a port outside the real 9100+n range, scaled-down wall
clock timeouts) get a throw-away copy of p910nd.c which is patched here,
compiled, and deleted again.

Every patch is an exact string replacement with an asserted hit count, so a
patch that stops matching (because upstream renamed a macro) fails loudly
instead of silently producing a binary that behaves like the shipping build.

Usage:
    patch_source.py --profile fast --port 19100 [--log FILE] COPY_OF_P910ND_C
"""

import argparse
import sys

# profile -> list of (needle, replacement, expected_hits, note)
# The needles use real TAB characters; p910nd.c aligns its macros with tabs.
PROFILES = {
    # Only move the listening port.  Everything else keeps shipping values.
    "ports": [
        ("BASEPORT\t9100", "BASEPORT\t{port}", 1, "listen port moved off 9100+n"),
    ],
    # Everything the "fast" profile needs to keep a case down to a few
    # seconds.  The ratios between the timeouts are preserved so the
    # qualitative behaviour under test is unchanged.
    "fast": [
        ("BASEPORT\t9100", "BASEPORT\t{port}", 1, "listen port moved off 9100+n"),
        ("IDLE_TIMEOUT\t30", "IDLE_TIMEOUT\t3", 1, "idle probe timeout 30s -> 3s"),
        ("PRINTER_STALL_TIMEOUT\t120", "PRINTER_STALL_TIMEOUT\t6", 1, "printer stall 120s -> 6s"),
        ("PRINTER_REPLY_WINDOW\t60", "PRINTER_REPLY_WINDOW\t6", 1, "printer reply window 60s -> 6s"),
        ("SILENT_TIMEOUT\t\t120", "SILENT_TIMEOUT\t\t12", 1, "silent client timeout 120s -> 12s"),
        ("PRINTER_READ_PACE_US\t100000", "PRINTER_READ_PACE_US\t10000", 1, "printer read pace 100ms -> 10ms"),
        ("NO_PROGRESS_USLEEP\t20000", "NO_PROGRESS_USLEEP\t2000", 1, "idle loop pause 20ms -> 2ms"),
    ],
}


def apply_profile(text, profile, port):
    if profile not in PROFILES:
        raise SystemExit("unknown profile %r" % profile)
    log = []
    for needle, repl, want, note in PROFILES[profile]:
        new = repl.format(port=port)
        hits = text.count(needle)
        if hits != want:
            raise SystemExit(
                "patch %r matched %d time(s), expected %d -- the source layout "
                "changed, refusing to build a mis-patched binary"
                % (needle, hits, want)
            )
        text = text.replace(needle, new)
        log.append("  %-28s -> %-28s (%s)" % (needle, new, note))
    return text, log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("--profile", default="fast", choices=sorted(PROFILES))
    ap.add_argument("--port", type=int, default=19100)
    ap.add_argument("--log", default=None)
    args = ap.parse_args()

    with open(args.source, "r", encoding="utf-8") as fh:
        text = fh.read()

    patched, log = apply_profile(text, args.profile, args.port)

    with open(args.source, "w", encoding="utf-8") as fh:
        fh.write(patched)

    header = "patched %s (profile=%s port=%d)" % (args.source, args.profile, args.port)
    if args.log:
        with open(args.log, "a", encoding="utf-8") as fh:
            fh.write(header + "\n")
            fh.write("\n".join(log) + "\n")
    sys.stderr.write(header + "\n")
    sys.stderr.write("\n".join(log) + "\n")


if __name__ == "__main__":
    main()
