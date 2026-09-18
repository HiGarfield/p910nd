# Define USE_WRAP if you want to compile with
# libwrap (hosts.{allow,deny} access control).  The source's own macro is
# USE_LIBWRAP, so spelling it that way on the command line is accepted too:
# without the alias the build silently produced a binary with no access
# control, while the operator believed hosts.{allow,deny} were being enforced.
ifneq ($(strip $(USE_WRAP)$(USE_LIBWRAP)),)
  LIBS += -lwrap
  DEFINES += -DUSE_LIBWRAP
endif

# If you don't have it in /var/log/subsys, uncomment and define
#CFLAGS += -DLOCKFILE_DIR=\"/var/log\"

# GNU target string
CROSS ?= 

CC ?= $(CROSS)gcc
STRIP ?= $(CROSS)strip

# Append this project's warning flags to whatever the caller supplied instead
# of replacing them.  A distribution's CFLAGS routinely carry hardening
# switches -- Ubuntu's dpkg-buildflags adds -Werror=format-security, for one --
# and filtering every -W* out here would silently disable exactly the checks a
# package build expects to run.
override CFLAGS := $(CFLAGS) -Wall -Wextra

PROG = p910nd
CONFIG = aux/p910nd.conf
INITSCRIPT = aux/p910nd.init
MANPAGE = p910nd.8
INSTALL = install
BINDIR = /usr/sbin
CONFIGDIR = /etc/sysconfig
SCRIPTDIR = /etc/init.d
MANDIR = /usr/share/man/man8

$(PROG):	p910nd.c
	$(CC) $(CFLAGS) $(DEFINES) $^ $(LDFLAGS) $(LIBS) -o $@

strip: $(PROG)
	$(STRIP) -s $(PROG)

install: $(PROG) $(CONFIG) $(INITSCRIPT) $(MANPAGE)
	mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(CONFIGDIR) \
			 $(DESTDIR)$(SCRIPTDIR) $(DESTDIR)$(MANDIR)
	$(INSTALL) -m 755 $(PROG) $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 644 $(CONFIG) $(DESTDIR)$(CONFIGDIR)/$(PROG)
	$(INSTALL) -m 755 $(INITSCRIPT) $(DESTDIR)$(SCRIPTDIR)/$(PROG)
	$(INSTALL) -m 644 $(MANPAGE) $(DESTDIR)$(MANDIR)

.PHONY: check
check:
	sh tests/run.sh

.PHONY: clean
clean:
	rm -f *.o $(PROG)
