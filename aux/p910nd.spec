Summary: p910nd is a small daemon that copies any data received to the corresponding printer port.
Name: p910nd
Version: 1.1
Release: 1
URL: http://etherboot.sourceforge.net/p910nd
Vendor: Ken Yap
License: GPL v2
# Upstream tarballs stop at 0.97; build from the git checkout instead.
Source0: %{name}-%{version}.tar.gz
# PD-87: do not hard-code BuildArchitectures.  rpmbuild infers the target
# architecture from the build host; pinning i386 x86_64 here made an RPM for
# any other target impossible even though the CI matrix cross-builds
# aarch64, arm, mips, powerpc, riscv64, s390x and more.
BuildRoot: %{_tmppath}/%{name}-%{version}-build
Group: Networking

%description
p910nd implements the port 9100 network printer protocol which simply
copies any incoming data on the port to the printer (and in the reverse
direction, if bidirectional mode is enabled). Both parallel and USB
printers are supported. This protocol was used in HP's printers and is
called JetDirect (probably TM). p910nd is particularly useful for
diskless hosts and embedded devices because it does not require any disk
space for spooling as this is done at the sending host.

%pre

%prep
rm -fr %{buildroot}
%setup -n p910nd-%{version}

%build
make

%install
%makeinstall

%post
insserv /etc/init.d/p910nd

%preun
insserv -r /etc/init.d/p910nd

%postun

%clean
rm -fr %{buildroot}

%files
# %defattr applies to every listed file without an explicit %attr, so 755
# used to make the config file and the compressed man page world executable.
# Default regular files to 0644 (directories to 0755) and mark only the two
# real executables as 0755.
%defattr(644, root, root, 755)
/etc/sysconfig/p910nd
%attr(755, root, root) /etc/init.d/p910nd
%attr(755, root, root) /usr/sbin/p910nd
%{_mandir}/man8/p910nd.8.gz

%changelog
* Fri Oct 03 2026 Higarfield <higarfield@gmail.com>
- 1.1: Take the printer lock in every build (it was compiled out unless
  LOCKFILE_DIR was defined), create the lock directory when missing, use
  CLOCK_MONOTONIC for every timeout, report a failed job with RST on every
  path, and act on a printer read error in bidirectional mode.

* Fri Jan 04 2008 Ken Yap <greenpossum@users.sourceforge.net>
- 0.92: First spec file
* Mon Feb 09 2009 Ken Yap <greenpossum@users.sourceforge.net>
- 0.93: Fix up open with mode for call with O_CREAT
* Thu Jan 09 2014 Ken Yap <greenposum@users.sourceforge.net>
- 0.96: Logging messages to stdout for debugging with -d, bidirectional buffer handling fixes
* Thu Jan 16 2014 Ken Yap <greenposum@users.sourceforge.net>
- 0.97: Wait until printer has received all data before closing job as USB printer driver is asynchronous in recent kernels
