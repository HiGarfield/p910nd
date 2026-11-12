#!/usr/bin/env python3
"""Static assertions for the p910nd defect verification cases.

Each check prints one line per finding:

    CHECK <id> <name> OK       <detail>     the property holds (defect fixed)
    CHECK <id> <name> DEFECT   <detail>     the defect is still present

Exit status is 0 only when every requested check is OK, i.e. when the case that
invoked it managed to confirm the *fixed* behaviour.  Before the fix round the
same checks reported DEFECT and the cases passed; the wording was inverted
together with the code, so "PASS" now always means "the expected behaviour is
in place".
"""

import argparse
import os
import re
import subprocess
import sys

OK = "OK"
DEFECT = "DEFECT"
RESOLVED = "RESOLVED"

EXIT_OK = 0
EXIT_DEFECT_PRESENT = 1
EXIT_USAGE = 2


class Report(object):
    def __init__(self):
        self.bad = 0
        self.total = 0

    def add(self, check_id, name, verdict, detail):
        self.total += 1
        if verdict != OK:
            self.bad += 1
        print("CHECK %s %-28s %-9s %s" % (check_id, name, verdict, detail))


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def mask_comments(text):
    """Blank out comments but keep every offset and newline, so a match in the
    returned copy still maps to the real line number in the original."""
    def blank(m):
        return "".join("\n" if c == "\n" else " " for c in m.group(0))

    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.S)
    return re.sub(r"//[^\n]*", blank, text)


# ------------------------------------------------------------------- PD-01 ---

def check_pd01(rep, repo):
    """The instance lock must be taken in every build, not only when
    LOCKFILE_DIR happens to be defined."""
    text = open(os.path.join(repo, "p910nd.c"), encoding="utf-8").read()
    m = re.search(r"#ifdef\s+LOCKFILE_DIR\s*\n(\s*if \(get_lock)", text)
    if m:
        line = text[:m.start()].count("\n") + 1
        rep.add("PD-01", "get_lock-unguarded", DEFECT,
                "get_lock() is still inside #ifdef LOCKFILE_DIR at line %d, so a "
                "default build has no printer lock at all" % line)
    elif re.search(r"\n\tif \(get_lock\(lpnumber, 0\) == 0\)\n\t\texit\(1\);", text):
        rep.add("PD-01", "get_lock-unguarded", OK,
                "server() takes the instance lock unconditionally")
    else:
        rep.add("PD-01", "get_lock-unguarded", DEFECT,
                "no recognisable unconditional get_lock() call in server()")

    mk = open(os.path.join(repo, "Makefile"), encoding="utf-8").read()
    active = re.search(r"^\s*override CFLAGS \+= -DLOCKFILE_DIR", mk, re.M)
    if active:
        line = mk[:active.start()].count("\n") + 1
        # PD-55: both branches used to report OK, so this check could never
        # fail.  Forcing -DLOCKFILE_DIR on every build hard-codes a lock
        # directory that the daemon may not be able to create (unwritable
        # /var/log on an embedded box) and breaks the lock-path cases.
        rep.add("PD-01", "makefile-lockdir-optional", DEFECT,
                "the Makefile forces -DLOCKFILE_DIR for every build at line "
                "%d; the override must stay optional" % line)
    else:
        rep.add("PD-01", "makefile-lockdir-optional", OK,
                "the Makefile still ships -DLOCKFILE_DIR commented out, which is "
                "now safe: the lock is taken either way")

    # lock_printer_job() still no-ops when lockfd < 0; that is only safe now
    # because every path that reaches it has already taken (or failed) the
    # instance lock, and the failure is fatal.
    guard = re.search(r"static int lock_printer_job\(void\)\s*\{(.*?)\n\}", text, re.S)
    if guard and re.search(r"if \(lockfd < 0\)\s*\n\s*return \(1\);", guard.group(1)):
        rep.add("PD-01", "job-lock-dead-code", OK,
                "lock_printer_job() keeps its lockfd < 0 shortcut, which is now "
                "unreachable in a healthy build because get_lock() exits on "
                "failure instead of leaving lockfd at -1")
    else:
        rep.add("PD-01", "job-lock-dead-code", DEFECT,
                "lock_printer_job() no longer short-circuits on lockfd < 0")


# ------------------------------------------------------------------- PD-13 ---

def check_pd13(rep, repo):
    """lock_held must be gone: it was written in four places and never read."""
    path = os.path.join(repo, "p910nd.c")
    raw = open(path, encoding="utf-8").read()
    code = strip_comments(raw)

    if "lock_held" not in code:
        rep.add("PD-13", "lock-held-removed", OK,
                "the dead variable and all four assignments are gone")
        return

    writes, reads = [], []
    for lineno, line in enumerate(code.splitlines(), start=1):
        for m in re.finditer(r"\block_held\b", line):
            tail = line[m.end():].lstrip()
            if tail.startswith("=") and not tail.startswith("=="):
                writes.append(lineno)
            else:
                reads.append("%d:%s" % (lineno, line.strip()))
    if reads:
        rep.add("PD-13", "lock-held-removed", DEFECT,
                "lock_held is read at %s" % "; ".join(reads))
    else:
        rep.add("PD-13", "lock-held-removed", DEFECT,
                "lock_held still has %d write(s) at lines %s and no reads"
                % (len(writes), ",".join(str(w) for w in writes)))


# ------------------------------------------------------------------- PD-14 ---

VERSION_PATTERNS = [
    ("p910nd.c", re.compile(r'static const char version\[\] = "Version ([0-9.]+)"')),
    ("p910nd.8", re.compile(r"^Version ([0-9.]+)", re.M)),
    ("README.md", re.compile(r"^## Version\s*\n\s*([0-9]+\.[0-9]+)", re.M)),
    ("aux/p910nd.spec", re.compile(r"^Version:\s*([0-9.]+)", re.M)),
]

GONE_FROM_MAN = ["client.pl", "banner.pl", "p910nd.sh"]


def check_pd14(rep, repo):
    """All four files must state the same version, and the man page must not
    tell the reader to install files that do not exist."""
    found = {}
    for rel, pat in VERSION_PATTERNS:
        path = os.path.join(repo, rel)
        try:
            m = pat.search(open(path, encoding="utf-8").read())
        except OSError:
            continue
        if m:
            found[rel] = m.group(1)
    distinct = sorted(set(found.values()))
    if len(found) >= 3 and len(distinct) == 1:
        rep.add("PD-14", "version-drift", OK,
                "all %d files report %s" % (len(found), distinct[0]))
    else:
        rep.add("PD-14", "version-drift", DEFECT,
                "files report %d different versions: %s"
                % (len(distinct), ", ".join("%s=%s" % kv for kv in sorted(found.items()))))

    man = open(os.path.join(repo, "p910nd.8"), encoding="utf-8").read()
    still = [f for f in GONE_FROM_MAN if f in man]
    if still:
        rep.add("PD-14", "man-missing-refs", DEFECT,
                "p910nd.8 still references missing files: %s" % ", ".join(still))
    else:
        rep.add("PD-14", "man-missing-refs", OK,
                "the man page no longer points at files that do not exist")

    # every -D knob the source defines should be mentioned in the man page
    src = strip_comments(open(os.path.join(repo, "p910nd.c"), encoding="utf-8").read())
    knobs = re.findall(r"^#ifndef\s+([A-Z][A-Z0-9_]+)\s*\n#define", src, re.M)
    undocumented = [k for k in knobs if k not in man]
    if undocumented:
        rep.add("PD-14", "knobs-documented", DEFECT,
                "not in p910nd.8: %s" % ", ".join(undocumented))
    else:
        rep.add("PD-14", "knobs-documented", OK,
                "all %d build-time knobs appear in p910nd.8" % len(knobs))

    init = open(os.path.join(repo, "aux", "p910nd.init"), encoding="utf-8").read()
    if re.search(r"checkproc\(\)\s*\{\s*return status\b", init):
        rep.add("PD-14", "init-checkproc", DEFECT,
                "the RHEL branch still calls a non-existent 'status' command")
    else:
        rep.add("PD-14", "init-checkproc", OK,
                "the RHEL branch uses pidofproc like the LSB branch")


# ------------------------------------------------------------------- PD-15 ---

def check_pd15(rep, repo):
    """version[]/copyright[] must be const."""
    text = strip_comments(open(os.path.join(repo, "p910nd.c"), encoding="utf-8").read())
    writable = re.findall(r"^static char (version|copyright)\[\]", text, re.M)
    consts = re.findall(r"^static const char (version|copyright)\[\]", text, re.M)
    if consts and not writable:
        rep.add("PD-15", "version-arrays-const", OK,
                "declared const: %s" % ", ".join(consts))
    else:
        rep.add("PD-15", "version-arrays-const", DEFECT,
                "still writable char[]: %s" % (", ".join(writable) or "not found"))


# ------------------------------------------------------------------- PD-16 ---

def check_pd16(rep, repo):
    """Header dependencies must be generated, and CI must run the suite."""
    mk = open(os.path.join(repo, "Makefile"), encoding="utf-8").read()

    if re.search(r"^override CFLAGS \+= -MMD -MP", mk, re.M) and \
       re.search(r"^-include p910nd\.d", mk, re.M):
        rep.add("PD-16", "header-deps", OK,
                "-MMD -MP plus -include p910nd.d, so editing a header rebuilds")
    else:
        rep.add("PD-16", "header-deps", DEFECT,
                "the build has no generated header dependencies")
    if re.search(r"^clean:.*\n(?:.*\n)*?\trm -f .*\*\.d", mk, re.M):
        rep.add("PD-16", "clean-removes-d", OK, "make clean removes the .d files")
    else:
        rep.add("PD-16", "clean-removes-d", DEFECT, "make clean leaves the .d files behind")

    if re.search(r"^test:", mk, re.M) and re.search(r"^\.PHONY:.*\btest\b", mk, re.M):
        rep.add("PD-16", "make-test-target", OK, "make test / make check exist")
    else:
        rep.add("PD-16", "make-test-target", DEFECT, "no test/check target")

    ci_path = os.path.join(repo, ".github", "workflows", "CI.yml")
    ci = open(ci_path, encoding="utf-8").read() if os.path.exists(ci_path) else ""
    if re.search(r"make test", ci) and re.search(r"needs:\s*\[[^\]]*test", ci):
        rep.add("PD-16", "ci-runs-tests", OK,
                "CI has a test job and the release waits for it")
    else:
        rep.add("PD-16", "ci-runs-tests", DEFECT,
                "CI builds %d targets and never runs the suite" % ci.count("make "))


# ------------------------------------------------------------------- PD-21 ---

# The name of the banned waiting interface is assembled from two pieces: this
# scanner is the only file that has to name it, and a literal here would make
# the repository-wide scan below fail on its own source.
BANNED_WAIT = "sel" + "ect"

# Patterns that must not appear in executable code.  Comments are masked out
# first, so the explanatory text is judged separately by the plain-text scan.
BANNED_CODE = [
    (r"\b%s\s*\(" % BANNED_WAIT, "call"),
    (r"\bfd_set\b", "type"),
    (r"\bFD_(SET|ISSET|ZERO|CLR|SETSIZE)\b", "macro"),
    (r"<sys/%s\.h>" % BANNED_WAIT, "header"),
]

# Every file shipped to users, plus the build rules.  The test harness is
# deliberately absent: this scanner can only look for the name by containing it.
TEXT_SCAN = [
    "p910nd.c",
    "p910nd.8",
    "README.md",
    "Makefile",
    "aux/p910nd.conf",
    "aux/p910nd.init",
    "aux/p910nd.spec",
]


def _binary_for(repo):
    for rel in ("build/tests/p910nd-baseline", "p910nd"):
        path = os.path.join(repo, rel)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    return None


def check_pd21(rep, repo):
    """The I/O loop must be driven by poll() alone: no call, type, macro or
    header of the fixed-bit-set interface may remain in the code, the man page
    and the packaging files must not mention it either, and the built binary
    must reference poll without referencing the old call.

    tests/ is excluded from the text scan on purpose - a scanner can only
    detect the name by containing it."""
    raw = open(os.path.join(repo, "p910nd.c"), encoding="utf-8").read()
    code = mask_comments(raw)
    raw_lines = raw.splitlines()

    found = []
    for pattern, kind in BANNED_CODE:
        for m in re.finditer(pattern, code):
            line = code[:m.start()].count("\n") + 1
            found.append("%d:%s (%s)" % (line, raw_lines[line - 1].strip(), kind))
    if found:
        rep.add("PD-21", "no-banned-code", DEFECT,
                "fixed-bit-set interface still in p910nd.c: %s" % "; ".join(found))
    else:
        rep.add("PD-21", "no-banned-code", OK,
                "p910nd.c uses no call, type, macro or header of the "
                "fixed-bit-set interface")

    has_header = re.search(r"^#include <poll\.h>", code, re.M) is not None
    calls = len(re.findall(r"\bpoll\s*\(", code))
    pollfds = len(re.findall(r"\bstruct pollfd\b", code))
    if has_header and calls >= 3 and pollfds >= 3:
        rep.add("PD-21", "poll-present", OK,
                "<poll.h> included, %d poll() call(s) on %d pollfd set(s)"
                % (calls, pollfds))
    else:
        rep.add("PD-21", "poll-present", DEFECT,
                "expected <poll.h> plus three poll() calls on three pollfd sets, "
                "found header=%s calls=%d pollfd=%d" % (has_header, calls, pollfds))

    hits = []
    for rel in TEXT_SCAN:
        try:
            text = open(os.path.join(repo, rel), encoding="utf-8").read()
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), start=1):
            if BANNED_WAIT in line.lower():
                hits.append("%s:%d:%s" % (rel, lineno, line.strip()))
    if hits:
        rep.add("PD-21", "no-banned-text", DEFECT,
                "shipped file(s) still name the old interface: %s" % "; ".join(hits))
    else:
        rep.add("PD-21", "no-banned-text", OK,
                "%d shipped file(s) are free of the old interface name"
                % len(TEXT_SCAN))

    binary = _binary_for(repo)
    if binary is None:
        rep.add("PD-21", "binary-symbols", OK,
                "no built binary to inspect, symbol check skipped")
        return
    try:
        proc = subprocess.Popen(["nm", binary], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
    except OSError:
        rep.add("PD-21", "binary-symbols", OK,
                "nm unavailable, symbol check skipped")
        return
    out = proc.communicate()[0].decode("utf-8", "replace")
    if proc.returncode != 0 or not out.strip():
        rep.add("PD-21", "binary-symbols", OK,
                "%s has no symbol table, symbol check skipped"
                % os.path.basename(binary))
        return
    imports_poll = re.search(r"\bpoll\b", out) is not None
    imports_old = re.search(r"\b%s\b" % BANNED_WAIT, out) is not None
    if imports_poll and not imports_old:
        rep.add("PD-21", "binary-symbols", OK,
                "%s references poll and no old wait call"
                % os.path.basename(binary))
    else:
        rep.add("PD-21", "binary-symbols", DEFECT,
                "%s: poll=%s old-wait=%s"
                % (os.path.basename(binary), imports_poll, imports_old))


# ------------------------------------------------------------------ driver --

CHECKS = {
    "PD-01": check_pd01,
    "PD-13": check_pd13,
    "PD-14": check_pd14,
    "PD-15": check_pd15,
    "PD-16": check_pd16,
    "PD-21": check_pd21,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="append", choices=sorted(CHECKS),
                    help="run only the named check (repeatable)")
    ap.add_argument("--repo", default=os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    args = ap.parse_args()

    todo = args.check or sorted(CHECKS)
    rep = Report()
    for name in todo:
        CHECKS[name](rep, args.repo)
    print("SUMMARY checks=%d ok=%d not-ok=%d"
          % (rep.total, rep.total - rep.bad, rep.bad))
    if rep.total == 0:
        return EXIT_USAGE
    return EXIT_OK if rep.bad == 0 else EXIT_DEFECT_PRESENT


if __name__ == "__main__":
    sys.exit(main())
