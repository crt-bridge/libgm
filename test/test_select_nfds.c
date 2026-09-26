/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_select_nfds.c — the bug this test guards is SILENT: select() with a
 * null first argument is the Winsock idiom, that argument is ignored there.
 * On POSIX it must be sock + 1, or select() returns 0 forever: the binary
 * compiles, starts, and NEVER receives anything -- no error, no trace.
 *
 * A standalone C binary, no framework. gm_socket.c is INCLUDED directly:
 * gm_select_nfds is static, and exporting it for the
 * sole convenience of a test would widen the internal API.
 *
 * NO socket is opened here. This test does not measure that reception
 * works -- it measures the one value whose error shows up nowhere else.
 * The other, free safety net is status_rtt_ms.n in gmsource's JSON: a
 * broken select leaves it at 0 forever.
 */
#include <stdio.h>
#include "gm_socket.c"

static int fails;

static void eq(const char *name, int got, int expected)
{
    if (got != expected) {
        fprintf(stderr, "FAIL %s: got %d, expected %d\n", name, got, expected);
        fails++;
    }
}

int main(void)
{
#ifdef _WIN32
    eq("gm_select_nfds(5) under Winsock", gm_select_nfds(5), 0);
    eq("gm_select_nfds(0) under Winsock", gm_select_nfds(0), 0);
#else
    eq("gm_select_nfds(0)",    gm_select_nfds(0),    1);
    eq("gm_select_nfds(5)",    gm_select_nfds(5),    6);
    eq("gm_select_nfds(1023)", gm_select_nfds(1023), 1024);
#endif
    if (fails) { fprintf(stderr, "test_select_nfds: %d failure(s)\n", fails); return 1; }
    printf("test_select_nfds : ok\n");
    return 0;
}
