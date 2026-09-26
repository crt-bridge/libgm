/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_uso_fallback.c — the split-send FAILURE paths (UDP_SEND_MSG_SIZE),
 * impossible to provoke on a real loopback socket: a read-back of the
 * option that lies or fails, a refused reset to zero, a segment refused
 * by gm_set_mtu, a refused or transiently failing super-send.
 *
 * Pattern: test_select_nfds.c. gm_socket.c is INCLUDED here, after macros
 * that replace setsockopt, getsockopt, sendto and WSAGetLastError with
 * stand-ins -- production code stays untouched, no test hook is added to
 * it. gm.c and gm_proto.c are linked as-is (their Makefile objects) and
 * call this unit's gm_socket_*.
 *
 * The sendto stand-in behaves like the system: if a segment is set and a
 * send exceeds it, it SPLITS it into datagrams of that segment size. This
 * is what makes the underlying bug visible: a stale segment re-splits the
 * original path, and the recorded "wire" no longer matches the model.
 *
 * Windows only: UDP_SEND_MSG_SIZE is a Windows mechanism.
 */
#ifndef _WIN32
#include <stdio.h>
int main(void) { printf("test_uso_fallback : skipped outside Windows\n"); return 0; }
#else

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#ifndef UDP_SEND_MSG_SIZE
#define UDP_SEND_MSG_SIZE 2
#endif

/* --- Stand-in state --------------------------------------------------------------------- */
static DWORD f_seg;          /* REAL value of the option on the socket                    */
static int   f_set_fail;     /* != 0: setting a non-zero segment fails with this code      */
static int   f_reset_fail;   /* 1: resetting the option to 0 fails (WSAEINVAL)             */
static int   f_get_fail;     /* 1: reading the option back fails                           */
static int   f_get_lie;      /* 1: a non-zero segment is read back +1                      */
static DWORD f_get_forced;   /* != 0: the read-back returns THIS value (takes priority over */
                              /* f_get_lie) when a non-zero segment is set                  */
static int   f_send_err;     /* != 0: a send over f_fail_above bytes fails                 */
static int   f_send_count;   /* number of failures left to inject (-1 = always)            */
static int   f_fail_above = 3800;
static int   f_last_err;
static int   f_sndbuf;
static int   f_dg[8192];     /* datagram sizes "on the wire"                               */
static int   f_ndg;

static int fake_setsockopt(SOCKET s, int level, int opt, const char *v, int len)
{
    (void)s; (void)len;
    if (level == IPPROTO_UDP && opt == UDP_SEND_MSG_SIZE) {
        DWORD d; memcpy(&d, v, sizeof d);
        if (d == 0 && f_reset_fail) { f_last_err = WSAEINVAL; return SOCKET_ERROR; }
        if (d != 0 && f_set_fail)   { f_last_err = f_set_fail; return SOCKET_ERROR; }
        f_seg = d;
        return 0;
    }
    if (level == SOL_SOCKET && opt == SO_SNDBUF) { memcpy(&f_sndbuf, v, sizeof f_sndbuf); return 0; }
    return 0;
}

static int fake_getsockopt(SOCKET s, int level, int opt, char *v, int *len)
{
    (void)s;
    if (level == IPPROTO_UDP && opt == UDP_SEND_MSG_SIZE) {
        if (f_get_fail) { f_last_err = WSAEINVAL; return SOCKET_ERROR; }
        DWORD d = (f_get_forced && f_seg != 0) ? f_get_forced : (f_get_lie && f_seg != 0) ? f_seg + 1u : f_seg;
        memcpy(v, &d, sizeof d); *len = (int)sizeof d;
        return 0;
    }
    if (level == SOL_SOCKET && opt == SO_SNDBUF) { memcpy(v, &f_sndbuf, sizeof f_sndbuf); *len = (int)sizeof f_sndbuf; return 0; }
    return 0;
}

static int fake_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    (void)s; (void)buf; (void)flags; (void)to; (void)tolen;
    if (f_send_err && f_send_count != 0 && len > f_fail_above) {
        if (f_send_count > 0) f_send_count--;
        f_last_err = f_send_err;
        return SOCKET_ERROR;
    }
    if (f_seg != 0 && len > (int)f_seg) {          /* the system splits it */
        int off = 0;
        while (off < len) {
            int c = (len - off > (int)f_seg) ? (int)f_seg : len - off;
            if (f_ndg < 8192) f_dg[f_ndg++] = c;
            off += c;
        }
    } else if (f_ndg < 8192) {
        f_dg[f_ndg++] = len;
    }
    return len;
}

static int fake_WSAGetLastError(void) { return f_last_err; }

#define setsockopt      fake_setsockopt
#define getsockopt      fake_getsockopt
#define sendto          fake_sendto
#define WSAGetLastError fake_WSAGetLastError
#include "gm_socket.c"
#undef setsockopt
#undef getsockopt
#undef sendto
#undef WSAGetLastError

#include "gm.h"

/* --- Tools -------------------------------------------------------------------------------- */
static int fails;
static uint8_t payload[65536];

static void check(const char *name, const char *what, int ok)
{
    if (!ok) { fprintf(stderr, "FAIL [%s] %s\n", name, what); fails++; }
}

static void reset_fakes(void)
{
    f_seg = 0; f_set_fail = 0; f_reset_fail = 0; f_get_fail = 0; f_get_lie = 0; f_get_forced = 0;
    f_send_err = 0; f_send_count = 0; f_fail_above = 3800; f_last_err = 0; f_ndg = 0;
}

static gm_handle *open_handle(void)
{
    SetEnvironmentVariableA("GROOVY_USO", NULL);   /* default: split send requested */
    gm_handle *h = gm_init("127.0.0.1", 9);
    f_ndg = 0;
    return h;
}

/* Is the wire of a raw gm_send_blit of n bytes at MTU mtu (classic session)
 * exactly: an 11-byte header, then chunks of mtu, then the remainder? */
static int wire_matches(int n, int mtu)
{
    int i = 0;
    if (f_ndg < 1 || f_dg[0] != 11) return 0;
    i = 1;
    while (n > 0) {
        int c = n > mtu ? mtu : n;
        if (i >= f_ndg || f_dg[i] != c) return 0;
        i++; n -= c;
    }
    return i == f_ndg;
}

static void dump(const char *name)
{
    fprintf(stderr, "  [%s] wire (%d datagrams):", name, f_ndg);
    for (int i = 0; i < f_ndg && i < 12; i++) fprintf(stderr, " %d", f_dg[i]);
    fprintf(stderr, "%s\n", f_ndg > 12 ? " ..." : "");
}

static int blit(gm_handle *h, int n)
{
    f_ndg = 0;
    return gm_send_blit(h, 1, 0, payload, (size_t)n);
}

/* --- Cases -------------------------------------------------------------------------------- */

/* Path 2: setsockopt accepted, lying read-back. The socket must keep no
 * segment at all, and a 3800 MTU must go out in intact 3800 chunks. */
static void case_readback_lies(void)
{
    const char *c = "lying read-back on open";
    reset_fakes(); f_get_lie = 1;
    gm_handle *h = open_handle();
    check(c, "handle open", h != NULL);
    if (!h) return;
    check(c, "split-send path inactive", gm_send_offload_state(h, NULL) == 0);
    check(c, "no segment left set on the socket", f_seg == 0);
    check(c, "gm_set_mtu(3800) accepted", gm_set_mtu(h, 3800) == 0);
    check(c, "blit 7600 -> 3800 + 3800 intact", blit(h, 7600) == 0 && wire_matches(7600, 3800));
    if (fails) dump(c);
    gm_close(h);
}

/* Path 2: failing read-back -- its return code must be checked.
 * f_get_fail is PERMANENT (not just for the first call): the internal
 * cleanup in gm_socket_set_send_segment (the reset attempt it makes
 * itself when its own confirmation read-back fails) genuinely SUCCEEDS in
 * bringing the socket back to 0 (f_seg == 0 proves it). gm_uso_disarm then
 * calls gm_socket_set_send_segment(0) again, which also succeeds but whose
 * confirmation read-back also fails -- then gm_socket_get_send_segment
 * fails in turn. In this exact case, err stays at 0 (the setsockopt(0)
 * really did succeed, only the read-back is unreadable); gm_uso_disarm now
 * trusts that success and sets uso_seg = 0, as it did before gm_init
 * learned to set uso_seg = mtu defensively on open (uso_seg = mtu only
 * survives a genuine refusal of setsockopt itself, see
 * case_open_readback_and_resets_fail). gm_set_mtu(3800) therefore
 * accepts again, and the wire goes back to 3800 + 3800. */
static void case_readback_fails(void)
{
    const char *c = "failing read-back on open";
    reset_fakes(); f_get_fail = 1;
    gm_handle *h = open_handle();
    if (!h) { check(c, "handle open", 0); return; }
    check(c, "split-send path inactive", gm_send_offload_state(h, NULL) == 0);
    check(c, "the socket genuinely returned to 0 (internal cleanup succeeded)", f_seg == 0);
    check(c, "gm_set_mtu(3800) accepted (setsockopt(0) succeeded, uso_seg confirmed at 0)", gm_set_mtu(h, 3800) == 0);
    check(c, "blit 7600 -> 3800 + 3800 intact", blit(h, 7600) == 0 && wire_matches(7600, 3800));
    gm_close(h);
}

/* Path 2, aggravated: lying read-back AND refused reset to zero. The
 * segment stays; the handle must know it and refuse a larger MTU. */
static void case_readback_lies_reset_refused(void)
{
    const char *c = "lying read-back, refused reset to zero";
    reset_fakes(); f_get_lie = 1; f_reset_fail = 1;
    gm_handle *h = open_handle();
    if (!h) { check(c, "handle open", 0); return; }
    unsigned seg = 0;
    gm_send_offload_stats(h, &seg, NULL, NULL, NULL);
    check(c, "split-send path inactive", gm_send_offload_state(h, NULL) == 0);
    check(c, "the handle knows a segment is still set", seg != 0);
    check(c, "gm_set_mtu(3800) REFUSED", gm_set_mtu(h, 3800) == -1);
    check(c, "blit 7600 at the original MTU, intact", blit(h, 7600) == 0 && wire_matches(7600, 1472));
    gm_close(h);
}

/* Double fault on open: setsockopt(1472) SUCCEEDS, but the read-back AND
 * both resets to zero (the one internal to gm_socket_set_send_segment,
 * then gm_uso_disarm's) all fail. The segment stays GENUINELY set at 1472
 * on the socket; the handle must know it (uso_seg = 1472, the requested
 * segment) instead of believing it has nothing (0, calloc's value) --
 * otherwise a 7600 blit would go out re-split as 1472 1472 856 1472 1472
 * 856 and gm_set_mtu(3800) would wrongly accept. 1472 not being < 1472,
 * the handle still comes out of gm_init. */
static void case_open_readback_and_resets_fail(void)
{
    const char *c = "open: failing read-back and resets";
    reset_fakes(); f_get_fail = 1; f_reset_fail = 1;
    gm_handle *h = open_handle();
    if (!h) { check(c, "handle open", 0); return; }
    unsigned seg = 0;
    gm_send_offload_stats(h, &seg, NULL, NULL, NULL);
    check(c, "split-send path inactive", gm_send_offload_state(h, NULL) == 0);
    check(c, "the handle knows the genuinely set segment is 1472", seg == 1472);
    check(c, "the socket genuinely carries 1472", f_seg == 1472);
    check(c, "gm_set_mtu(1472) accepted (uso_seg == mtu)", gm_set_mtu(h, 1472) == 0);
    check(c, "gm_set_mtu(3800) REFUSED", gm_set_mtu(h, 3800) == -1);
    check(c, "blit 7600 at 1472, intact", blit(h, 7600) == 0 && wire_matches(7600, 1472));
    if (fails) dump(c);
    gm_close(h);
}

/* Variant: the read-back returns a SHORT value (1400, not just
 * "different"), and the reset to zero is refused. The genuinely set
 * segment (1400) would re-split every send on the original path (MTU
 * 1472) with no guard refusing it -- gm_init must refuse to open rather
 * than return a handle that is already mis-split. */
static void case_open_short_readback_reset_refused(void)
{
    const char *c = "open: short read-back, refused reset";
    reset_fakes(); f_get_forced = 1400; f_reset_fail = 1;
    gm_handle *h = open_handle();
    check(c, "gm_init refuses (segment stays at 1400, shorter than the 1472 MTU)", h == NULL);
    if (h) gm_close(h);
}

/* Path 1: super-send refused by a code that proves the option's refusal.
 * Cutoff, AND segment removed from the socket. */
static void case_super_send_refused(void)
{
    const char *c = "super-send refused (WSAEMSGSIZE)";
    reset_fakes();
    gm_handle *h = open_handle();
    if (!h) { check(c, "handle open", 0); return; }
    check(c, "split-send path active on open", gm_send_offload_state(h, NULL) == 1);
    f_send_err = WSAEMSGSIZE; f_send_count = -1;
    check(c, "failing blit returns -1", blit(h, 7600) == -1);
    f_send_err = 0;
    int cut = 0; unsigned seg = 1;
    gm_send_offload_stats(h, &seg, NULL, NULL, &cut);
    check(c, "split-send path cut off", gm_send_offload_state(h, NULL) == 0);
    check(c, "cutoff code = WSAEMSGSIZE", cut == WSAEMSGSIZE);
    check(c, "segment removed from the socket", f_seg == 0 && seg == 0);
    check(c, "gm_set_mtu(3800) accepted", gm_set_mtu(h, 3800) == 0);
    check(c, "blit 7600 -> 3800 + 3800 intact", blit(h, 7600) == 0 && wire_matches(7600, 3800));
    if (fails) dump(c);
    gm_close(h);
}

/* Path 1, aggravated: super-send refused AND reset to zero refused. */
static void case_super_send_refused_reset_refused(void)
{
    const char *c = "super-send refused, refused reset to zero";
    reset_fakes();
    gm_handle *h = open_handle();
    if (!h) { check(c, "handle open", 0); return; }
    f_send_err = WSAEMSGSIZE; f_send_count = -1; f_reset_fail = 1;
    (void)blit(h, 7600);
    f_send_err = 0;
    check(c, "split-send path cut off", gm_send_offload_state(h, NULL) == 0);
    check(c, "gm_set_mtu(3800) REFUSED (segment 1472 still set)", gm_set_mtu(h, 3800) == -1);
    check(c, "blit 7600 at 1472, intact", blit(h, 7600) == 0 && wire_matches(7600, 1472));
    check(c, "gm_set_mtu(1000) accepted (segment longer than the MTU)", gm_set_mtu(h, 1000) == 0);
    check(c, "blit 7600 at 1000, intact", blit(h, 7600) == 0 && wire_matches(7600, 1000));
    gm_close(h);
}

/* Path 3: gm_set_mtu, segment refused AND reset to zero refused. uso_seg
 * must not fall to 0 in memory. */
static void case_set_mtu_segment_refused(void)
{
    const char *c = "gm_set_mtu: segment refused, refused reset to zero";
    reset_fakes();
    gm_handle *h = open_handle();
    if (!h) { check(c, "handle open", 0); return; }
    f_set_fail = WSAEINVAL; f_reset_fail = 1;
    check(c, "gm_set_mtu(1000) accepted", gm_set_mtu(h, 1000) == 0);
    check(c, "split-send path cut off", gm_send_offload_state(h, NULL) == 0);
    check(c, "blit 7600 at 1000, intact", blit(h, 7600) == 0 && wire_matches(7600, 1000));
    check(c, "gm_set_mtu(3800) REFUSED", gm_set_mtu(h, 3800) == -1);
    check(c, "blit 7600 still at 1000, intact", blit(h, 7600) == 0 && wire_matches(7600, 1000));
    if (fails) dump(c);
    gm_close(h);
}

/* Path 3, simple: segment refused, reset to zero accepted. */
static void case_set_mtu_segment_refused_reset_ok(void)
{
    const char *c = "gm_set_mtu: segment refused, accepted reset to zero";
    reset_fakes();
    gm_handle *h = open_handle();
    if (!h) { check(c, "handle open", 0); return; }
    f_set_fail = WSAEINVAL;
    check(c, "gm_set_mtu(3800) accepted", gm_set_mtu(h, 3800) == 0);
    check(c, "segment removed", f_seg == 0);
    check(c, "blit 7600 -> 3800 + 3800 intact", blit(h, 7600) == 0 && wire_matches(7600, 3800));
    gm_close(h);
}

/* A transient error does not cut off the split path; the next frame goes
 * out split again. */
static void case_transient_failure(int code, const char *name)
{
    char c[96];
    snprintf(c, sizeof c, "transient failure %s", name);
    reset_fakes();
    gm_handle *h = open_handle();
    if (!h) { check(c, "handle open", 0); return; }
    f_send_err = code; f_send_count = 1;
    check(c, "failing blit returns -1", blit(h, 7600) == -1);
    uint32_t sup = 0, tr = 0; int cut = -1;
    gm_send_offload_stats(h, NULL, &sup, &tr, &cut);
    check(c, "split-send path KEPT", gm_send_offload_state(h, NULL) == 1);
    check(c, "a transient failure counts, no cutoff", tr == 1 && cut == 0);
    check(c, "segment still set", f_seg == 1472);
    check(c, "next blit intact", blit(h, 7600) == 0 && wire_matches(7600, 1472));
    gm_send_offload_stats(h, NULL, &sup, NULL, NULL);
    check(c, "the next blit went out as a super-send", sup == 1);
    gm_close(h);
}

/* Every refusal code cuts off the split path. */
static void case_refusal_code(int code, const char *name)
{
    char c[96];
    snprintf(c, sizeof c, "refusal %s", name);
    reset_fakes();
    gm_handle *h = open_handle();
    if (!h) { check(c, "handle open", 0); return; }
    f_send_err = code; f_send_count = 1;
    (void)blit(h, 7600);
    int cut = 0;
    gm_send_offload_stats(h, NULL, NULL, NULL, &cut);
    check(c, "split-send path cut off", gm_send_offload_state(h, NULL) == 0 && cut == code);
    check(c, "segment removed", f_seg == 0);
    gm_close(h);
}

int main(void)
{
    for (size_t i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(i * 131u + 7u);

    case_readback_lies();
    case_readback_fails();
    case_readback_lies_reset_refused();
    case_open_readback_and_resets_fail();
    case_open_short_readback_reset_refused();
    case_super_send_refused();
    case_super_send_refused_reset_refused();
    case_set_mtu_segment_refused();
    case_set_mtu_segment_refused_reset_ok();
    case_transient_failure(WSAENOBUFS,      "WSAENOBUFS");
    case_transient_failure(WSAEWOULDBLOCK,  "WSAEWOULDBLOCK");
    case_transient_failure(WSAENETUNREACH,  "WSAENETUNREACH");
    case_transient_failure(WSAEHOSTUNREACH, "WSAEHOSTUNREACH");
    case_transient_failure(WSAEADDRNOTAVAIL, "WSAEADDRNOTAVAIL");
    case_refusal_code(WSAEMSGSIZE,    "WSAEMSGSIZE");
    case_refusal_code(WSAEINVAL,      "WSAEINVAL");
    case_refusal_code(WSAEOPNOTSUPP,  "WSAEOPNOTSUPP");
    case_refusal_code(WSAENOPROTOOPT, "WSAENOPROTOOPT");

    if (fails) { fprintf(stderr, "test_uso_fallback : %d failure(s)\n", fails); return 1; }
    printf("test_uso_fallback : ok (18 cases)\n");
    return 0;
}
#endif
