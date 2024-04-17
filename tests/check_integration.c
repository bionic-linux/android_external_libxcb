#include <check.h>
#include <stdlib.h>
#include "check_suites.h"
#include "xcb.h"
#include "xcbext.h"

START_TEST(request_check_hang)
{
	// Other tests mess with the environment. Try to get us an X11 server.
	putenv("DISPLAY=:0");

	xcb_connection_t *connection = xcb_connect(NULL, NULL);
	ck_assert_uint_eq(0, xcb_connection_has_error(connection));

	// Regression test for https://gitlab.freedesktop.org/xorg/lib/libxcb/-/issues/53
	xcb_void_cookie_t cookie = xcb_no_operation_checked(connection);
	xcb_flush(connection);
	xcb_get_input_focus(connection);

	// The following call once upon a time hung
	xcb_request_check(connection, cookie);

	xcb_disconnect(connection);
}
END_TEST

Suite *integration_suite(void)
{
	Suite *s = suite_create("Integration");
	suite_add_test(s, request_check_hang, "regression test: hand in xcb_request_check()");
	return s;
}
