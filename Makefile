# Define USE_WRAP if you want to compile with
# libwrap (hosts.{allow,deny} access control)
ifneq ($(USE_WRAP),)
LIBS += -lwrap
DEFINES += -DUSE_LIBWRAP
endif

# If you don't have it in /var/log/subsys, uncomment and define
# (override is needed: CFLAGS carries the override flag below, and a plain
# assignment to such a variable is ignored)
#override CFLAGS += -DLOCKFILE_DIR=\"/var/log\"

# GNU target string
CROSS = 

CC = $(CROSS)gcc
STRIP = $(CROSS)strip

override CFLAGS += -O2  -Wall -Wextra

# The daemon is written to strict ISO C89 (ANSI X3.159-1989): no C99 types or
# library calls, no declarations after a statement, no // comments.  These
# switches make every violation visible; -pedantic is used instead of
# -pedantic-errors so that a toolchain whose own headers emit noise still
# builds.  Violations are warnings, not errors - treat them as such and keep
# the build output clean (the CI builds must stay warning free).
#
# override is required here: a plain += is discarded whenever CFLAGS is given
# on the command line (the CI calls make CFLAGS="..." for every cross build),
# which would silently drop these checks.  Note that += keeps whatever the
# caller passed, it merely appends - so the CI still gets -static and friends.
# Once a variable carries the override flag every later assignment to it must
# use override too, or it is ignored; keep that in mind before adding any new
# CFLAGS line.
override CFLAGS += -std=c89
override CFLAGS += -pedantic
override CFLAGS += -Wdeclaration-after-statement

PROG = p910nd
CONFIG = aux/p910nd.conf
INITSCRIPT = aux/p910nd.init
MANPAGE = p910nd.8
INSTALL = install
BINDIR = /usr/sbin
CONFIGDIR = /etc/sysconfig
SCRIPTDIR = /etc/init.d
MANDIR = /usr/share/man/man8

all: $(PROG)

$(PROG):	p910nd.c
	$(CC) -o $@ $^ $(CFLAGS) $(DEFINES) $(LIBS)

strip: $(PROG)
	$(STRIP) -s $(PROG)

install: $(PROG) $(CONFIG) $(INITSCRIPT) $(MANPAGE)
	mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(CONFIGDIR) \
		$(DESTDIR)$(SCRIPTDIR) $(DESTDIR)$(MANDIR)
	$(INSTALL) $(PROG) $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 644 $(CONFIG) $(DESTDIR)$(CONFIGDIR)/$(PROG)
	$(INSTALL) $(INITSCRIPT) $(DESTDIR)$(SCRIPTDIR)/$(PROG)
	$(INSTALL) -m 644 $(MANPAGE) $(DESTDIR)$(MANDIR)

.PHONY: all install strip clean
clean:
	rm -f *.o $(PROG)
