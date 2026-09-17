# UNRESOLVED — needs your decision

These were deliberately **not** changed. Each either alters documented 0.97
behaviour (and therefore needs your approval), cannot be verified in this
environment, or is a new feature rather than a fix. Nothing here is broken by
the commits in this branch.

---

## U1 — CVE-2018-10123: `-f` plus `-b` gives remote read/append of any file · High · **design choice needed**

`-f` names an arbitrary path and `-b` opens it `O_RDWR`, so `p910nd -b -f /etc/shadow`
lets any client that can reach the port read that file **and append to it**. This
is the enabling primitive of CVE-2018-10123. The behaviour is upstream's by design
and some deployments genuinely need it (a real bidirectional printer not at
`/dev/lpN`), so changing the default would break them.

Two designs; both are **off by default**:

* **A — allowlist (recommended).** New compile-time list `-DDEVICE_ALLOWLIST='"/dev/lp%c:/dev/usblp%c"'`
  (or a runtime `-A <pattern>` option). `-f` is accepted only when the resolved
  path matches; otherwise refuse to start. No behaviour change unless you build
  with it, and it also catches typos like `p910nd -b -f /etc/shadow`.
* **B — gate `-b` on `-f`.** Refuse the combination unless a new
  `--allow-arbitrary-device` flag is given, or drop the effective uid and refuse
  when running as root. Smaller diff, but it forbids legitimate setups unless the
  operator passes the new flag.

The man page now carries a SECURITY section describing the exposure, which went
in regardless of which you pick. **Tell me A, B, or neither.**

---

## U2 — No privilege dropping · Medium · **new feature, default off**

The daemon runs as root and never drops privileges. Adding `-u`/`-g` (or setuid
support) is a feature, and there is a real ordering subtlety: the printer device
is **re-opened for every job**, so simply dropping to a non-root uid after bind
would make every later job fail to open `/dev/lpN`. A correct implementation must
either keep the device descriptor open across jobs (changing the 0.91 hotplug
behaviour by design) or rely on group permissions on the device node plus
`setgroups()`. Both are behaviour changes; both want your call on which semantics
you want before I write them.

---

## U3 — Missing printer blocks the whole daemon · Medium · **documented behaviour, recommend changing**

`server()` calls `accept()` and *then* `while ((lp = open_printer(...)) == -1) sleep(10);`.
If the device is missing, that accepted connection is never answered and **no
other client is accepted at all** — one client with a bad `-f` argument or an
unplugged USB printer is enough to make the daemon unreachable until the device
reappears. It sleeps, so there is no CPU cost; it is an availability weakness, not
a spin.

This is upstream's explicit choice — see the Liakakis vs Bartoszko discussion at
the top of `p910nd.c` — so I left it alone rather than silently changing a
documented semantic. Options:

* keep as-is;
* open the printer **before** `accept()` so the daemon keeps accepting and simply
  waits for the device between jobs (my recommendation — it preserves "retry
  forever" while removing the head-of-line block);
* bound the retry per connection and refuse the job, matching Bartoszko's variant.

---

## U4 — PID file: stale after exit, and `fopen("w")` follows symlinks · Low · **risk trade-off**

Two separable issues. (a) The pid file is never unlinked, so `/var/run/p9100d.pid`
points at a dead process after the daemon stops; scripts that read it can act on
the wrong pid. Fixing it cleanly means installing a signal handler and an exit
path, which is a small amount of new machinery. (b) `fopen(pidfilename, "w")`
truncates through symlinks, so as root a pre-planted symlink could clobber an
unrelated file. `O_NOFOLLOW` fixes it but would **break** anyone who legitimately
symlinks the pid file — unlikely, though not impossible on a distro that keeps pid
files elsewhere. Say whether you want either or both.

---

## U5 — libwrap (`-DUSE_LIBWRAP`) is unverified · Medium · **needs a host with `tcpd.h`**

This machine has no `tcpd.h`, so gate `libwrap` is SKIPped and I could not compile
or exercise that path. The checklist's "incomplete conversion to `ip_addr` under
libwrap" concern (the 0.95 patch) therefore remains unconfirmed here. Reading the
code, `hosts_ctl()` is called with the resolved IP string and `STRING_UNKNOWN`
elsewhere, which looks correct, and no Debian patch touches libwrap — but that is
inspection, not evidence. Please build with `-DUSE_LIBWRAP` on a machine that has
the header before shipping this branch.

---

## U6 — Cross-compilation not exercised · Low · **toolchains absent**

No `arm-linux-gnueabi-gcc` or `mips-linux-gnu-gcc` here. As a portability proxy I
did build with **musl-gcc** under `-std=c89 -Wall -Wextra -Wpedantic` and got zero
warnings, and the file also passes a C++98 syntax check. Big-endian targets and
real hardware remain untested.

---

## U7 — There is no `-t` option, and never was · Info · **feature request, not a bug**

The brief mentioned a `-t` timeout. Upstream 0.97 has no such option — `git weave`
through `kenyapcomau/p910nd` confirms `getopt(argc, argv, "bdi:f:v")` — and the
idle timeout here is the compile-time `IDLE_TIMEOUT_SEC` (5 s), which also bounds
the post-EOF grace window used to catch late printer replies. Adding `-t` would
extend the documented CLI, so I did not invent one. If you want it: parse with
`strtol`, reject negative and > `INT_MAX`, allow 0 meaning "disable the idle
timeout" (it should **not** disable the grace window, or late printer replies
would be dropped again), and update `p910nd.8`.

---

## U8 — Denied-connection logging under libwrap · Low · **covered by U5**

Rejected connections are logged and the descriptor is closed before `continue`
(p910nd.c:1912-1919), so there is no leak, but I could not execute it. Folded into
U5.
