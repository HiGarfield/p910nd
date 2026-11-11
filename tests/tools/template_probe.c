/*
 * Unit probe for the PRINTERFILE template validator.
 *
 * p910nd.c is included so printer_template_ok() is reachable.  Exactly one
 * %c conversion is legal: the daemon passes a single int to snprintf().
 * Any other conversion, a second conversion, or no conversion at all must
 * be rejected instead of reaching snprintf() with a bogus argument.
 */
#define main p910nd_main_unused
#include "p910nd.c"
#undef main

int main(void)
{
	int ok = 1;

	ok &= printer_template_ok("/dev/lp%c") == 1;
	ok &= printer_template_ok("/dev/usb/lp%c-tail") == 1;
	ok &= printer_template_ok("100%%lp%c") == 1;
	ok &= printer_template_ok("/dev/lp%s") == 0;
	ok &= printer_template_ok("/dev/lp%c%c") == 0;
	ok &= printer_template_ok("/dev/lp") == 0;
	ok &= printer_template_ok("%%") == 0;
	printf("template_checks=%d\n", ok);
	return ok ? 0 : 1;
}
