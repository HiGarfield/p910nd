# p910nd

p910nd is a small printer daemon intended for diskless platforms that does not spool to disk but passes the job directly to the printer. Normally a lpr daemon on a spooling host connects to it with a TCP connection on port 910n (where n=0, 1, or 2 for lp0, 1 and 2 respectively). p910nd is particularly useful for diskless platforms. Common Unix Printing System (CUPS) supports this protocol, it's called the AppSocket protocol and has the scheme socket://. LPRng also supports this protocol and the syntax is lp=remotehost%9100 in /etc/printcap.

## Version

1.1

The authoritative version string lives in `p910nd.c` (`static const char version[]`)
and is what `p910nd -v` prints; this file, `p910nd.8` and `aux/p910nd.spec` are
kept in step with it.

## Authors

**Ken Yap** and others

## License

See the [LICENSE](LICENSE.md) file for license rights and limitations (GPL2).
