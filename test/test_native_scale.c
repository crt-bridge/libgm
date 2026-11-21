/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_native_scale.c -- gm_set_native_scale (CMD_NATIVE_SCALE, 0x09) on the wire.
 *
 * Loopback only, port 42410 (never the bench's 32100). A plain UDP socket plays the
 * receiver and reads what the handle sends:
 *   - classic session: refused, nothing sent;
 *   - padded session: [09 kx ky 00 00 00 00 00] right away;
 *   - same factors again: nothing; other factors: a new 0x09;
 *   - CMD_SWITCHRES then repeats the 0x09 right behind its own 26 bytes;
 *   - (1, 1) is announced too, and a SWITCHRES after it adds nothing.
 * POSIX only (the Windows bench has no compiler of its own for tests). */
#ifdef _WIN32
#include <stdio.h>
int main(void) { printf("test_native_scale : skipped on Windows\n"); return 0; }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "gm.h"
#include "gm_internal.h"

static int g_fail = 0;
#define EXPECT(cond, label) do { \
    if (cond) printf("PASS: %s\n", label); \
    else { printf("FAIL: %s\n", label); g_fail++; } \
} while (0)

/* One datagram, or -1 on a 200 ms silence. */
static int rx(int s, unsigned char *b, size_t cap)
{
    const ssize_t n = recv(s, b, cap, 0);
    return n < 0 ? -1 : (int)n;
}

int main(void)
{
    const uint16_t port = 42410u;
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) != 0) { printf("FAIL: bind\n"); return 1; }
    struct timeval tv = { 0, 200000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    unsigned char b[64];
    const gm_modeline m = { 20.75, 1280, 1296, 1304, 1320, 240, 240, 243, 262, 0 };

    /* classic session: refused */
    gm_handle *h = gm_init("127.0.0.1", port);
    if (!h) { printf("FAIL: gm_init\n"); return 1; }
    EXPECT(gm_set_native_scale(h, 4u, 1u) == -1, "classic_session_refused");
    EXPECT(rx(s, b, sizeof b) == -1, "classic_session_sends_nothing");
    gm_close(h);

    /* padded session */
    h = gm_init("127.0.0.1", port);
    gm_set_padding(h, 1);
    EXPECT(gm_set_native_scale(h, 0u, 1u) == -1 && gm_set_native_scale(h, 1u, 11u) == -1, "bad_factors_refused");
    EXPECT(gm_set_native_scale(h, 4u, 1u) == 0, "padded_accepted");
    {
        const int n = rx(s, b, sizeof b);
        static const unsigned char want[8] = { 0x09, 4, 1, 0, 0, 0, 0, 0 };
        EXPECT(n == 8 && memcmp(b, want, 8) == 0, "wire_is_09_4_1_and_zero_tail");
    }
    EXPECT(gm_set_native_scale(h, 4u, 1u) == 0 && rx(s, b, sizeof b) == -1, "same_factors_send_nothing");
    EXPECT(gm_set_native_scale(h, 2u, 2u) == 0, "other_factors_accepted");
    {
        const int n = rx(s, b, sizeof b);
        EXPECT(n == 8 && b[0] == 0x09 && b[1] == 2 && b[2] == 2, "wire_is_09_2_2");
    }
    EXPECT(gm_resend_native_scale(h) == 0, "resend_ok");
    {
        const int n = rx(s, b, sizeof b);
        EXPECT(n == 8 && b[0] == 0x09 && b[1] == 2 && b[2] == 2, "resend_repeats_09_2_2");
    }
    EXPECT(gm_send_switchres(h, &m) == 0, "switchres_sent");
    {
        const int n1 = rx(s, b, sizeof b);
        EXPECT(n1 == 26 && b[0] == 0x03, "switchres_is_26_bytes");
        const int n2 = rx(s, b, sizeof b);
        EXPECT(n2 == 8 && b[0] == 0x09 && b[1] == 2 && b[2] == 2, "0x09_repeated_behind_switchres");
    }
    EXPECT(gm_set_native_scale(h, 1u, 1u) == 0, "withdrawn");
    {
        const int n = rx(s, b, sizeof b);
        EXPECT(n == 8 && b[0] == 0x09 && b[1] == 1 && b[2] == 1, "withdrawal_is_announced");
    }
    EXPECT(gm_send_switchres(h, &m) == 0 && rx(s, b, sizeof b) == 26 && rx(s, b, sizeof b) == -1,
           "switchres_after_withdrawal_adds_nothing");
    EXPECT(gm_resend_native_scale(h) == 0 && rx(s, b, sizeof b) == -1, "resend_after_withdrawal_sends_nothing");
    gm_close(h);

    /* CMD_NATIVE_PLACE (0x0A) */
    h = gm_init("127.0.0.1", port);
    gm_set_padding(h, 1);
    EXPECT(gm_set_native_place(h, 0u, 1u, 300u, 200u) == -1 && gm_set_native_place(h, 4u, 11u, 300u, 200u) == -1 &&
           gm_set_native_place(h, 4u, 1u, 0u, 200u) == -1 && gm_set_native_place(h, 4u, 1u, 300u, 0u) == -1 &&
           gm_set_native_place(h, 4u, 1u, 70000u, 200u) == -1, "place_bad_arguments_refused");
    EXPECT(gm_set_native_place(h, 4u, 1u, 368u, 480u) == 0, "place_accepted");
    {
        const int n = rx(s, b, sizeof b);
        static const unsigned char want[8] = { 0x0A, 4, 1, 0x70, 0x01, 0xE0, 0x01, 0 };
        EXPECT(n == 8 && memcmp(b, want, 8) == 0, "place_wire_is_0A_4_1_368_480_zero");
    }
    EXPECT(gm_set_native_place(h, 4u, 1u, 368u, 480u) == 0 && rx(s, b, sizeof b) == -1, "place_same_sends_nothing");
    EXPECT(gm_set_native_place(h, 1u, 1u, 1000u, 200u) == 0, "place_without_factor_accepted");
    {
        const int n = rx(s, b, sizeof b);
        EXPECT(n == 8 && b[0] == 0x0A && b[1] == 1 && b[2] == 1 && b[3] == 0xE8 && b[4] == 0x03 && b[5] == 200u && b[6] == 0,
               "place_wire_1_1_1000_200");
    }
    EXPECT(gm_resend_native_scale(h) == 0, "place_resend_ok");
    EXPECT(rx(s, b, sizeof b) == 8 && b[0] == 0x0A, "place_resend_repeats_0A");
    EXPECT(gm_send_switchres(h, &m) == 0, "place_switchres_sent");
    {
        const int n1 = rx(s, b, sizeof b);
        EXPECT(n1 == 26 && b[0] == 0x03, "place_switchres_is_26_bytes");
        const int n2 = rx(s, b, sizeof b);
        EXPECT(n2 == 8 && b[0] == 0x0A && b[3] == 0xE8, "place_0A_repeated_behind_switchres");
    }
    EXPECT(gm_set_native_scale(h, 1u, 1u) == 0, "place_withdrawn_by_scale");
    {
        const int n = rx(s, b, sizeof b);
        EXPECT(n == 8 && b[0] == 0x09 && b[1] == 1 && b[2] == 1, "place_withdrawal_is_0x09_1_1");
    }
    EXPECT(gm_resend_native_scale(h) == 0 && rx(s, b, sizeof b) == -1, "place_resend_after_withdrawal_sends_nothing");
    EXPECT(gm_set_native_place(h, 10u, 2u, 256u, 240u) == 0, "place_x10_2_accepted");

    /* FLAG_NATIVE_OK: read from the last status of the peer */
    {
        struct sockaddr_in me; socklen_t ml = sizeof me;
        getsockname(h->sock, (struct sockaddr *)&me, &ml);
        EXPECT(gm_peer_native_ok(h) == 0, "peer_native_ok_no_status_yet");
        unsigned char st[13]; memset(st, 0, sizeof st);
        st[12] = 0x03;   /* ready | end of frame, no native bit: an older receiver */
        sendto(s, st, sizeof st, 0, (struct sockaddr *)&me, sizeof me);
        gm_status gs;
        EXPECT(gm_recv_status(h, &gs, 200) == 0 && gm_peer_native_ok(h) == 0, "peer_native_ok_old_receiver");
        st[12] = 0x83;   /* + FLAG_NATIVE_OK */
        sendto(s, st, sizeof st, 0, (struct sockaddr *)&me, sizeof me);
        EXPECT(gm_recv_status(h, &gs, 200) == 0 && gm_peer_native_ok(h) == 1, "peer_native_ok_new_receiver");
    }
    gm_close(h);

    /* a classic session never reports it, even with the bit set */
    h = gm_init("127.0.0.1", port);
    (void)gm_send_switchres(h, &m);   /* binds the socket */
    (void)rx(s, b, sizeof b);
    {
        struct sockaddr_in me; socklen_t ml = sizeof me;
        getsockname(h->sock, (struct sockaddr *)&me, &ml);
        unsigned char st[13]; memset(st, 0, sizeof st); st[12] = 0x83;
        sendto(s, st, sizeof st, 0, (struct sockaddr *)&me, sizeof me);
        gm_status gs;
        EXPECT(gm_recv_status(h, &gs, 200) == 0 && gm_peer_native_ok(h) == 0, "peer_native_ok_classic_session_never");
    }
    gm_close(h);
    close(s);

    printf("%s (test_native_scale)\n", g_fail ? "SOME CASES FAILED" : "ALL CASES PASSED");
    return g_fail ? 1 : 0;
}
#endif
