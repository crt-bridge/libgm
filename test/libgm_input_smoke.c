/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libgm_input_smoke.exe — host-side proof of the Groovy input channel
 * (UDP/32101).
 *
 * Plays the RECEIVER's role on 127.0.0.1, with no hardware and no test
 * bench: this binary opens a "fake receiver" UDP socket, waits for the
 * hook byte that gm_bind_inputs() sends, learns the resulting source
 * address, then emits datagrams to that address to exercise
 * gm_poll_inputs() -- exactly the role the real receiver would play on
 * the wire (the direction is reversed compared to the video channel: it
 * is the receiver that pushes here).
 *
 * Includes gm_internal.h (reachable via -Isrc, already in CFLAGS) to read
 * the internal rejection counters (in_stale, in_rejected_len,
 * in_rejected_src) and the raw input_sock descriptor -- a deliberate
 * gray-box test, not an abstraction leak toward public callers.
 *
 * Usage: libgm_input_smoke.exe (no argument, no real network address --
 * everything happens on 127.0.0.1/127.0.0.2).
 */

#include "gm_internal.h"

#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;

#define REQUIRE(cond, case_name) do { \
    g_checks++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s] (%s:%d): %s\n", (case_name), __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

/* select(timeout) + recvfrom, on the "fake receiver" side. Same shape as
 * gm_socket_recvfrom_timeout_from, but reimplemented here: this file plays
 * the network peer, it does not call into libgm for its own traffic. */
static int fake_recv(SOCKET s, void *buf, int cap, struct sockaddr_in *from, int timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int rc = select(0, &rfds, NULL, NULL, &tv);
    if (rc <= 0) return -1;

    int from_len = (int)sizeof(*from);
    return recvfrom(s, (char *)buf, cap, 0, (struct sockaddr *)from, &from_len);
}

static size_t build_joy9(uint8_t *buf, uint32_t frame, uint8_t order,
                         uint16_t joy1, uint16_t joy2)
{
    memcpy(buf + 0, &frame, sizeof(uint32_t));
    buf[4] = order;
    memcpy(buf + 5, &joy1, sizeof(uint16_t));
    memcpy(buf + 7, &joy2, sizeof(uint16_t));
    return 9;
}

static size_t build_joy17(uint8_t *buf, uint32_t frame, uint8_t order,
                          uint16_t joy1, uint16_t joy2, const int8_t axes[8])
{
    build_joy9(buf, frame, order, joy1, joy2);
    memcpy(buf + 9, axes, 8);
    return 17;
}

static size_t build_ps2_37(uint8_t *buf, uint32_t frame, uint8_t order,
                           const uint8_t keys[32])
{
    memcpy(buf + 0, &frame, sizeof(uint32_t));
    buf[4] = order;
    memcpy(buf + 5, keys, 32);
    return 37;
}

static size_t build_ps2_41(uint8_t *buf, uint32_t frame, uint8_t order,
                           const uint8_t keys[32], const uint8_t mouse[4])
{
    build_ps2_37(buf, frame, order, keys);
    memcpy(buf + 37, mouse, 4);
    return 41;
}

/* seq_build_* — uniform (buf, frame, order) -> datagram wrappers around the
 * four build_* constructors above, the rest of the payload zeroed. Used by
 * the "sequence" block below: only the (frame, order) pair matters there,
 * so a single loop over these four wrappers exercises all four wire
 * lengths without four hand-duplicated blocks. */
static size_t seq_build_9(uint8_t *buf, uint32_t frame, uint8_t order)
{
    return build_joy9(buf, frame, order, 0u, 0u);
}

static size_t seq_build_17(uint8_t *buf, uint32_t frame, uint8_t order)
{
    int8_t axes_zero[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    return build_joy17(buf, frame, order, 0u, 0u, axes_zero);
}

static size_t seq_build_37(uint8_t *buf, uint32_t frame, uint8_t order)
{
    uint8_t keys[32];
    memset(keys, 0, sizeof(keys));
    return build_ps2_37(buf, frame, order, keys);
}

static size_t seq_build_41(uint8_t *buf, uint32_t frame, uint8_t order)
{
    uint8_t keys[32];
    uint8_t mouse[4] = { 0, 0, 0, 0 };
    memset(keys, 0, sizeof(keys));
    return build_ps2_41(buf, frame, order, keys, mouse);
}

int main(void)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "FAIL [setup]: WSAStartup\n");
        return 1;
    }

    /* --- Step 1: fake receiver, ephemeral port chosen by the OS --- */
    SOCKET fake = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(fake != INVALID_SOCKET, "setup-fake-socket");

    struct sockaddr_in fake_addr;
    memset(&fake_addr, 0, sizeof(fake_addr));
    fake_addr.sin_family      = AF_INET;
    fake_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    fake_addr.sin_port        = 0;
    REQUIRE(bind(fake, (struct sockaddr *)&fake_addr, sizeof(fake_addr)) == 0,
            "setup-bind");

    int fake_addr_len = (int)sizeof(fake_addr);
    REQUIRE(getsockname(fake, (struct sockaddr *)&fake_addr, &fake_addr_len) == 0,
            "setup-getsockname");
    uint16_t fake_port = ntohs(fake_addr.sin_port);
    printf("[setup] fake receiver on 127.0.0.1:%u\n", (unsigned)fake_port);

    /* --- Step 2: gm_init + gm_bind_inputs --- */
    gm_handle *h = gm_init("127.0.0.1", 32100);
    REQUIRE(h != NULL, "setup-gm_init");
    REQUIRE(gm_bind_inputs(h, fake_port) == 0, "setup-gm_bind_inputs");

    /* --- Step 3: the hook fits in a single byte; learn the source
     * address for every later send (the upstream BindInputs pattern). --- */
    struct sockaddr_in emitter_addr;
    uint8_t hello_buf[8];
    int hn = fake_recv(fake, hello_buf, (int)sizeof(hello_buf), &emitter_addr, 1000);
    REQUIRE(hn == 1, "setup-hello-length");
    printf("[setup] hook byte received, emitter on source port %u\n",
           (unsigned)ntohs(emitter_addr.sin_port));

    /* --- Nominal case: three 17-byte datagrams, in order --- */
    {
        uint8_t buf[64];
        size_t  len;

        int8_t axes_zero[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        int8_t axes_nz[8]   = { 1, -1, 2, -2, 3, -3, 4, -4 };

        len = build_joy17(buf, 10u, 0u, 0u, 0u, axes_zero);
        REQUIRE(sendto(fake, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&emitter_addr, sizeof(emitter_addr)) == (int)len,
                "nominal-send-1");

        len = build_joy17(buf, 10u, 1u, 0u, 0u, axes_zero);
        REQUIRE(sendto(fake, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&emitter_addr, sizeof(emitter_addr)) == (int)len,
                "nominal-send-2");

        len = build_joy17(buf, 11u, 0u, 0u, 0u, axes_nz);
        REQUIRE(sendto(fake, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&emitter_addr, sizeof(emitter_addr)) == (int)len,
                "nominal-send-3");

        Sleep(50);
        int accepted = gm_poll_inputs(h);
        printf("[nominal] nominal case (17 bytes x3, in order): %d accepted\n", accepted);
        REQUIRE(accepted == 3, "nominal-accepted-count");

        gm_joy_inputs got;
        REQUIRE(gm_get_joy_inputs(h, &got) == 0, "nominal-get");
        REQUIRE(got.frame == 11u && got.order == 0u, "nominal-final-state");
        REQUIRE(got.joy1_lx == 1 && got.joy2_ry == -4, "nominal-axes-carried");
    }

    /* --- Out-of-order UDP: a fresh handle, (11,0) then (10,1) --- */
    {
        gm_handle *h2 = gm_init("127.0.0.1", 32100);
        REQUIRE(h2 != NULL, "reorder-init");
        REQUIRE(gm_bind_inputs(h2, fake_port) == 0, "reorder-bind");

        struct sockaddr_in emitter_addr2;
        uint8_t hello2[8];
        int hn2 = fake_recv(fake, hello2, (int)sizeof(hello2), &emitter_addr2, 1000);
        REQUIRE(hn2 == 1, "reorder-hello-length");

        uint8_t buf[64];
        size_t  len;
        int8_t  axes_zero[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

        len = build_joy17(buf, 11u, 0u, 0u, 0u, axes_zero);
        REQUIRE(sendto(fake, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&emitter_addr2, sizeof(emitter_addr2)) == (int)len,
                "reorder-send-1");

        len = build_joy17(buf, 10u, 1u, 0u, 0u, axes_zero);
        REQUIRE(sendto(fake, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&emitter_addr2, sizeof(emitter_addr2)) == (int)len,
                "reorder-send-2");

        Sleep(50);
        int accepted2 = gm_poll_inputs(h2);
        printf("[reorder] out-of-order UDP send: %d accepted, in_stale=%u\n",
               accepted2, (unsigned)h2->in_stale);
        REQUIRE(accepted2 == 1, "reorder-accepted-count");

        gm_joy_inputs got2;
        REQUIRE(gm_get_joy_inputs(h2, &got2) == 0, "reorder-get");
        REQUIRE(got2.frame == 11u && got2.order == 0u, "reorder-final-state");
        REQUIRE(h2->in_stale == 1u, "reorder-in_stale");

        gm_close(h2);
    }

    /* ---------------------------------------------------------------------
     * Sequence guard characterization: the "reorder" block above only sends
     * two DIFFERENT `frame` values. This closes that gap: two datagrams
     * with the SAME `frame` and an equal or decreasing `order` must both be
     * rejected, and an increasing `order` at the same frame must be
     * accepted (control case).
     *
     * These are CHARACTERIZATION tests: they MUST pass on the current code.
     * gm_poll_inputs already compares strictly ( > , not >= ) on frame, then
     * on order at equal frame -- this block proves that in place, so a
     * clean-room rewrite ("without the code in front of them") has a
     * complete oracle instead of relying on this file as ground truth
     * without proof.
     *
     * Looped over the four datagram lengths (9/17/37/41 bytes) via the
     * seq_build_* wrappers; a dedicated handle per sub-case, same pattern
     * (fresh handle, hello handshake, Sleep(50) before gm_poll_inputs, named
     * REQUIRE) as the "reorder" block above.
     * --------------------------------------------------------------------- */
    {
        typedef size_t (*seq_build_fn)(uint8_t *, uint32_t, uint8_t);
        struct seq_case { size_t len; seq_build_fn build; };
        static const struct seq_case seq_cases[4] = {
            { 9,  seq_build_9  },
            { 17, seq_build_17 },
            { 37, seq_build_37 },
            { 41, seq_build_41 },
        };

        for (int si = 0; si < 4; si++) {
            size_t       slen   = seq_cases[si].len;
            seq_build_fn sbuild = seq_cases[si].build;
            uint8_t      buf[64];
            char         name[64];

            /* seq-equal-<len> : (100,5) then (100,5) again -> 1 accepted,
             * in_stale +1. Same frame, same order: the strict ">" (not
             * ">=") comparison rejects it. */
            {
                gm_handle *hs = gm_init("127.0.0.1", 32100);
                REQUIRE(hs != NULL, "seq-equal-init");
                REQUIRE(gm_bind_inputs(hs, fake_port) == 0, "seq-equal-bind");

                struct sockaddr_in ea;
                uint8_t hello_e[8];
                int hne = fake_recv(fake, hello_e, (int)sizeof(hello_e), &ea, 1000);
                REQUIRE(hne == 1, "seq-equal-hello-length");

                size_t l1 = sbuild(buf, 100u, 5u);
                REQUIRE(sendto(fake, (const char *)buf, (int)l1, 0,
                               (struct sockaddr *)&ea, sizeof(ea)) == (int)l1,
                        "seq-equal-send-1");
                size_t l2 = sbuild(buf, 100u, 5u);
                REQUIRE(sendto(fake, (const char *)buf, (int)l2, 0,
                               (struct sockaddr *)&ea, sizeof(ea)) == (int)l2,
                        "seq-equal-send-2");

                Sleep(50);
                int acc = gm_poll_inputs(hs);
                snprintf(name, sizeof(name), "seq-equal-%u", (unsigned)slen);
                printf("[sequence] %s : (100,5) then (100,5) : %d accepted, in_stale=%u\n",
                       name, acc, (unsigned)hs->in_stale);
                REQUIRE(acc == 1, name);
                snprintf(name, sizeof(name), "seq-equal-%u-in_stale", (unsigned)slen);
                REQUIRE(hs->in_stale == 1u, name);

                gm_close(hs);
            }

            /* seq-order-down-<len> : (100,5) then (100,4) -> 1 accepted,
             * in_stale +1. Same frame, order strictly DECREASING: rejected,
             * same as the equal case above. */
            {
                gm_handle *hs = gm_init("127.0.0.1", 32100);
                REQUIRE(hs != NULL, "seq-order-down-init");
                REQUIRE(gm_bind_inputs(hs, fake_port) == 0, "seq-order-down-bind");

                struct sockaddr_in ea;
                uint8_t hello_e[8];
                int hne = fake_recv(fake, hello_e, (int)sizeof(hello_e), &ea, 1000);
                REQUIRE(hne == 1, "seq-order-down-hello-length");

                size_t l1 = sbuild(buf, 100u, 5u);
                REQUIRE(sendto(fake, (const char *)buf, (int)l1, 0,
                               (struct sockaddr *)&ea, sizeof(ea)) == (int)l1,
                        "seq-order-down-send-1");
                size_t l2 = sbuild(buf, 100u, 4u);
                REQUIRE(sendto(fake, (const char *)buf, (int)l2, 0,
                               (struct sockaddr *)&ea, sizeof(ea)) == (int)l2,
                        "seq-order-down-send-2");

                Sleep(50);
                int acc = gm_poll_inputs(hs);
                snprintf(name, sizeof(name), "seq-order-down-%u", (unsigned)slen);
                printf("[sequence] %s : (100,5) then (100,4) : %d accepted, in_stale=%u\n",
                       name, acc, (unsigned)hs->in_stale);
                REQUIRE(acc == 1, name);
                snprintf(name, sizeof(name), "seq-order-down-%u-in_stale", (unsigned)slen);
                REQUIRE(hs->in_stale == 1u, name);

                gm_close(hs);
            }

            /* seq-order-up-<len> (control) : (100,5) then (100,6) -> both
             * accepted. Proves the guard is strict but not frozen: a
             * genuinely newer order at the same frame still gets through. */
            {
                gm_handle *hs = gm_init("127.0.0.1", 32100);
                REQUIRE(hs != NULL, "seq-order-up-init");
                REQUIRE(gm_bind_inputs(hs, fake_port) == 0, "seq-order-up-bind");

                struct sockaddr_in ea;
                uint8_t hello_e[8];
                int hne = fake_recv(fake, hello_e, (int)sizeof(hello_e), &ea, 1000);
                REQUIRE(hne == 1, "seq-order-up-hello-length");

                size_t l1 = sbuild(buf, 100u, 5u);
                REQUIRE(sendto(fake, (const char *)buf, (int)l1, 0,
                               (struct sockaddr *)&ea, sizeof(ea)) == (int)l1,
                        "seq-order-up-send-1");
                size_t l2 = sbuild(buf, 100u, 6u);
                REQUIRE(sendto(fake, (const char *)buf, (int)l2, 0,
                               (struct sockaddr *)&ea, sizeof(ea)) == (int)l2,
                        "seq-order-up-send-2");

                Sleep(50);
                int acc = gm_poll_inputs(hs);
                snprintf(name, sizeof(name), "seq-order-up-%u", (unsigned)slen);
                printf("[sequence] %s : (100,5) then (100,6) : %d accepted, in_stale=%u\n",
                       name, acc, (unsigned)hs->in_stale);
                REQUIRE(acc == 2, name);

                gm_close(hs);
            }
        }
    }

    /* --- Unknown lengths: 12, 40, 0 bytes --- */
    {
        uint8_t buf[64];
        memset(buf, 0xAA, sizeof(buf));

        REQUIRE(sendto(fake, (const char *)buf, 12, 0,
                       (struct sockaddr *)&emitter_addr, sizeof(emitter_addr)) == 12,
                "length-send-12");
        REQUIRE(sendto(fake, (const char *)buf, 40, 0,
                       (struct sockaddr *)&emitter_addr, sizeof(emitter_addr)) == 40,
                "length-send-40");
        REQUIRE(sendto(fake, (const char *)buf, 0, 0,
                       (struct sockaddr *)&emitter_addr, sizeof(emitter_addr)) == 0,
                "length-send-0");

        Sleep(50);
        int accepted3 = gm_poll_inputs(h);
        printf("[length] three unknown lengths (12/40/0): %d accepted, "
               "in_rejected_len=%u\n", accepted3, (unsigned)h->in_rejected_len);
        REQUIRE(accepted3 == 0, "length-accepted-count");
        REQUIRE(h->in_rejected_len == 3u, "length-rejected-count");

        gm_joy_inputs still;
        REQUIRE(gm_get_joy_inputs(h, &still) == 0, "length-get-unchanged");
        REQUIRE(still.frame == 11u && still.order == 0u, "length-state-unchanged");
    }

    /* --- 9 bytes: digital joystick only, axes fall back to zero --- */
    {
        uint8_t buf[64];
        uint16_t joy1 = (uint16_t)(GM_JOY_B1 | GM_JOY_UP);
        size_t len = build_joy9(buf, 12u, 0u, joy1, 0u);

        REQUIRE(sendto(fake, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&emitter_addr, sizeof(emitter_addr)) == (int)len,
                "9-bytes-send");

        Sleep(50);
        int accepted4 = gm_poll_inputs(h);
        printf("[9-bytes] short message (joystick with no analog): %d accepted\n",
               accepted4);
        REQUIRE(accepted4 == 1, "9-bytes-accepted-count");

        gm_joy_inputs got4;
        REQUIRE(gm_get_joy_inputs(h, &got4) == 0, "9-bytes-get");
        REQUIRE(got4.frame == 12u && got4.order == 0u, "9-bytes-frame-order");
        REQUIRE(got4.joy1 == joy1, "9-bytes-joy1");
        REQUIRE(got4.joy1_lx == 0 && got4.joy1_ly == 0 && got4.joy1_rx == 0 &&
                got4.joy1_ry == 0 && got4.joy2_lx == 0 && got4.joy2_ly == 0 &&
                got4.joy2_rx == 0 && got4.joy2_ry == 0,
                "9-bytes-axes-zeroed");
    }

    /* --- 37 and 41 bytes: keyboard/mouse channel (ps2) --- */
    {
        uint8_t buf[64];
        uint8_t keys[32];
        for (int i = 0; i < 32; i++) keys[i] = (uint8_t)(0xC0 + i);

        size_t len37 = build_ps2_37(buf, 1u, 0u, keys);
        REQUIRE(sendto(fake, (const char *)buf, (int)len37, 0,
                       (struct sockaddr *)&emitter_addr, sizeof(emitter_addr)) == (int)len37,
                "ps2-send-37");

        Sleep(50);
        int accepted5 = gm_poll_inputs(h);
        printf("[ps2] keyboard only (37 bytes): %d accepted\n", accepted5);
        REQUIRE(accepted5 == 1, "ps2-37-accepted-count");

        gm_ps2_inputs got5;
        REQUIRE(gm_get_ps2_inputs(h, &got5) == 0, "ps2-37-get");
        REQUIRE(memcmp(got5.keys, keys, 32) == 0, "ps2-37-keys");
        REQUIRE(got5.mouse_bits == 0 && got5.mouse_x == 0 && got5.mouse_y == 0 &&
                got5.mouse_z == 0, "ps2-37-mouse-zeroed");

        uint8_t mouse[4] = { 0x08, 0x05, 0xFB, 0x00 };
        size_t len41 = build_ps2_41(buf, 2u, 0u, keys, mouse);
        REQUIRE(sendto(fake, (const char *)buf, (int)len41, 0,
                       (struct sockaddr *)&emitter_addr, sizeof(emitter_addr)) == (int)len41,
                "ps2-send-41");

        Sleep(50);
        int accepted6 = gm_poll_inputs(h);
        printf("[ps2] keyboard + mouse (41 bytes): %d accepted\n", accepted6);
        REQUIRE(accepted6 == 1, "ps2-41-accepted-count");

        gm_ps2_inputs got6;
        REQUIRE(gm_get_ps2_inputs(h, &got6) == 0, "ps2-41-get");
        REQUIRE(memcmp(got6.keys, keys, 32) == 0, "ps2-41-keys");
        REQUIRE(got6.mouse_bits == mouse[0] && got6.mouse_x == mouse[1] &&
                got6.mouse_y == mouse[2] && got6.mouse_z == mouse[3],
                "ps2-41-mouse");
    }

    /* --- Mouse deltas ACCUMULATED at the drain point, signed byte-256
     * decoding (not -255), consumed-and-reset-to-zero by
     * gm_get_mouse_deltas. --- */
    {
        /* Purge: the ps2-41 case above already fed the accumulator
         * (bits=0x08 with no sign bit: dx=+5, dy=+251, dz=0) -- checking it
         * proves the POSITIVE decoding along the way. */
        int dx = 0, dy = 0, dz = 0;
        REQUIRE(gm_get_mouse_deltas(h, &dx, &dy, &dz) == 0, "mouse-purge");
        REQUIRE(dx == 5 && dy == 251 && dz == 0, "mouse-purge-values");

        uint8_t buf[64];
        uint8_t keys[32];
        memset(keys, 0, sizeof(keys));

        /* Two datagrams of the SAME receiver frame (order 0 then 1),
         * drained in the SAME gm_poll_inputs: "last state wins" would lose
         * the first datagram's deltas, and a -255 decoding would turn
         * dx=-1 into 0. Deltas: (-1,-5,-2) then (+5,+2,+1). */
        uint8_t mouse_a[4] = { 0x30, 0xFF, 0xFB, 0xFE };  /* x+y sign bits set */
        uint8_t mouse_b[4] = { 0x00, 0x05, 0x02, 0x01 };
        size_t len = build_ps2_41(buf, 3u, 0u, keys, mouse_a);
        REQUIRE(sendto(fake, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&emitter_addr, sizeof(emitter_addr)) == (int)len,
                "mouse-send-a");
        len = build_ps2_41(buf, 3u, 1u, keys, mouse_b);
        REQUIRE(sendto(fake, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&emitter_addr, sizeof(emitter_addr)) == (int)len,
                "mouse-send-b");

        Sleep(50);
        int accepted_m = gm_poll_inputs(h);
        printf("[mouse] two 41-byte datagrams, same frame, order 0/1: %d accepted\n",
               accepted_m);
        REQUIRE(accepted_m == 2, "mouse-accepted-count");

        REQUIRE(gm_get_mouse_deltas(h, &dx, &dy, &dz) == 0, "mouse-get");
        printf("[mouse] accumulated deltas: dx=%d dy=%d dz=%d (expected 4/-3/-1)\n",
               dx, dy, dz);
        /* dx = (-1) + 5 = 4: only passes if BOTH datagrams are accumulated
         * AND the negative decoding is byte-256 (last-state-wins would
         * give 5; a -255 decoding would also give 0+5 = 5). */
        REQUIRE(dx == 4,  "mouse-dx-accumulated-signed");
        REQUIRE(dy == -3, "mouse-dy-accumulated-signed");
        REQUIRE(dz == -1, "mouse-dz-twos-complement");

        REQUIRE(gm_get_mouse_deltas(h, &dx, &dy, &dz) == 0, "mouse-reget");
        REQUIRE(dx == 0 && dy == 0 && dz == 0, "mouse-consumed-and-zero");
    }

    /* --- An OVERSIZED datagram (> 64 bytes, WSAEMSGSIZE on the Winsock
     * side) must neither interrupt the drain nor pass without a trace --
     * it counts in in_rejected_len, and the legitimate datagram already
     * queued behind it is consumed in the SAME gm_poll_inputs. --- */
    {
        uint8_t big[80];
        memset(big, 0xBB, sizeof(big));
        REQUIRE(sendto(fake, (const char *)big, (int)sizeof(big), 0,
                       (struct sockaddr *)&emitter_addr, sizeof(emitter_addr)) == (int)sizeof(big),
                "oversized-send-big");

        uint8_t buf[64];
        int8_t  axes_zero[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        size_t  len = build_joy17(buf, 13u, 0u, (uint16_t)GM_JOY_B3, 0u, axes_zero);
        REQUIRE(sendto(fake, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&emitter_addr, sizeof(emitter_addr)) == (int)len,
                "oversized-send-valid");

        Sleep(50);
        uint32_t rejected_len_before = h->in_rejected_len;
        int accepted_o = gm_poll_inputs(h);
        printf("[oversized] 80 bytes then 17 valid bytes, a single drain: "
               "%d accepted, in_rejected_len %u -> %u\n", accepted_o,
               (unsigned)rejected_len_before, (unsigned)h->in_rejected_len);
        /* Before the fix: accepted == 0 (drain interrupted by WSAEMSGSIZE's
         * -1) and in_rejected_len silent. */
        REQUIRE(accepted_o == 1, "oversized-accepted-count");
        REQUIRE(h->in_rejected_len == rejected_len_before + 1u, "oversized-rejected-len");

        gm_joy_inputs got_o;
        REQUIRE(gm_get_joy_inputs(h, &got_o) == 0, "oversized-get");
        REQUIRE(got_o.frame == 13u && got_o.joy1 == (uint16_t)GM_JOY_B3,
                "oversized-state-behind-the-big-one");
    }

    /* --- Rebinding on the SAME handle resets the sequence guard to zero
     * -- a restarted receiver (counter back near zero) must be accepted,
     * not counted as in_stale. --- */
    {
        /* Handle's current state: last_joy.frame == 12, last_ps2.frame == 3
         * (previous cases). Rebind: everything must start over from zero. */
        REQUIRE(gm_bind_inputs(h, fake_port) == 0, "rebind-bind");

        /* Rebinding sends a new hello byte from a NEW socket: relearn the
         * source address, as the real receiver would. */
        struct sockaddr_in emitter_addr3;
        uint8_t hello3[8];
        int hn3 = fake_recv(fake, hello3, (int)sizeof(hello3), &emitter_addr3, 1000);
        REQUIRE(hn3 == 1, "rebind-hello-length");

        /* A frame=1 datagram (much older than the previous session's 12):
         * before the fix it would have fallen into in_stale and has_joy
         * would have stayed at 0. */
        uint8_t buf[64];
        int8_t  axes_zero[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        size_t  len = build_joy17(buf, 1u, 0u, (uint16_t)GM_JOY_B2, 0u, axes_zero);
        REQUIRE(sendto(fake, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&emitter_addr3, sizeof(emitter_addr3)) == (int)len,
                "rebind-send");

        Sleep(50);
        int accepted_r = gm_poll_inputs(h);
        printf("[rebind] receiver restarted (frame 1 after a session at 12): "
               "%d accepted, in_stale=%u\n", accepted_r, (unsigned)h->in_stale);
        REQUIRE(accepted_r == 1, "rebind-accepted-count");
        REQUIRE(h->in_stale == 0u, "rebind-no-stale");

        gm_joy_inputs got_r;
        REQUIRE(gm_get_joy_inputs(h, &got_r) == 0, "rebind-get");
        REQUIRE(got_r.frame == 1u && got_r.joy1 == (uint16_t)GM_JOY_B2,
                "rebind-fresh-state");

        /* emitter_addr follows the new socket for the following cases. */
        emitter_addr = emitter_addr3;
    }

    /* --- Foreign source address: covered if 127.0.0.2 is available on
     * this host, explicitly skipped otherwise (never declared successful
     * without proof). --- */
    {
        SOCKET foreign = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        int foreign_ok = 0;
        if (foreign != INVALID_SOCKET) {
            struct sockaddr_in foreign_addr;
            memset(&foreign_addr, 0, sizeof(foreign_addr));
            foreign_addr.sin_family = AF_INET;
            inet_pton(AF_INET, "127.0.0.2", &foreign_addr.sin_addr);
            foreign_addr.sin_port = 0;
            foreign_ok = (bind(foreign, (struct sockaddr *)&foreign_addr,
                               sizeof(foreign_addr)) == 0);
        }

        if (!foreign_ok) {
            printf("[foreign] 127.0.0.2 unavailable on this host -- case skipped, "
                   "not declared successful (covered by code review)\n");
            if (foreign != INVALID_SOCKET) closesocket(foreign);
        } else {
            struct sockaddr_in target;
            int target_len = (int)sizeof(target);
            REQUIRE(getsockname((SOCKET)h->input_sock, (struct sockaddr *)&target,
                                &target_len) == 0,
                    "foreign-getsockname");
            target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            uint8_t buf[64];
            int8_t axes_zero[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
            size_t len = build_joy17(buf, 99u, 0u, 0u, 0u, axes_zero);

            REQUIRE(sendto(foreign, (const char *)buf, (int)len, 0,
                           (struct sockaddr *)&target, sizeof(target)) == (int)len,
                    "foreign-send");

            Sleep(50);
            uint32_t rejected_src_before = h->in_rejected_src;
            int accepted7 = gm_poll_inputs(h);
            printf("[foreign] datagram from 127.0.0.2: %d accepted, "
                   "in_rejected_src %u -> %u\n", accepted7,
                   (unsigned)rejected_src_before, (unsigned)h->in_rejected_src);
            REQUIRE(accepted7 == 0, "foreign-accepted-count");
            REQUIRE(h->in_rejected_src == rejected_src_before + 1u, "foreign-rejected-src");

            closesocket(foreign);
        }
    }

    /* ---------------------------------------------------------------------
     * gm_send_input_hello: resend of the hook byte ON THE ALREADY-OPEN
     * SOCKET, never a new one. Four cases (T1-T4): same source port on two
     * resends, unchanged socket, surviving input state, and a clean
     * refusal on a channel that is not hooked.
     * --------------------------------------------------------------------- */

    /* T3 (first half): before any gm_bind_inputs, the channel is not
     * hooked -> -1, and nothing is sent. */
    {
        gm_handle *h3 = gm_init("127.0.0.1", 32100);
        REQUIRE(h3 != NULL, "hello-t3a-init");
        REQUIRE(gm_send_input_hello(h3) == -1, "hello-t3a-not-hooked");

        struct sockaddr_in dummy_a;
        uint8_t dummy_buf_a[8];
        int nd_a = fake_recv(fake, dummy_buf_a, (int)sizeof(dummy_buf_a), &dummy_a, 200);
        REQUIRE(nd_a == -1, "hello-t3a-nothing-sent");

        gm_close(h3);
    }

    /* T1 + T2: two resends from the SAME socket as the last gm_bind_inputs
     * (the one from the "rebind" block above) -- same source port P1 on
     * both, and h->input_sock unchanged. */
    {
        uint16_t p1 = ntohs(emitter_addr.sin_port);
        int sock_before = h->input_sock;

        REQUIRE(gm_send_input_hello(h) == 0, "hello-t1-send-1");
        REQUIRE(gm_send_input_hello(h) == 0, "hello-t1-send-2");

        struct sockaddr_in from_a, from_b;
        uint8_t hb_a[8], hb_b[8];
        int na = fake_recv(fake, hb_a, (int)sizeof(hb_a), &from_a, 1000);
        REQUIRE(na == 1, "hello-t1-len-1");
        int nb = fake_recv(fake, hb_b, (int)sizeof(hb_b), &from_b, 1000);
        REQUIRE(nb == 1, "hello-t1-len-2");

        printf("[hello] two resends: source ports %u and %u (P1=%u)\n",
               (unsigned)ntohs(from_a.sin_port), (unsigned)ntohs(from_b.sin_port),
               (unsigned)p1);
        REQUIRE(ntohs(from_a.sin_port) == p1, "hello-t1-source-port-1");
        REQUIRE(ntohs(from_b.sin_port) == p1, "hello-t1-source-port-2");

        REQUIRE(h->input_sock == sock_before, "hello-t2-socket-unchanged");
    }

    /* T4: input state already received SURVIVES the resend -- a
     * fundamental difference from gm_bind_inputs, which resets
     * last_joy/last_ps2 to zero. */
    {
        uint8_t buf[64];
        int8_t axes_zero[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        size_t len = build_joy17(buf, 42u, 0u, (uint16_t)GM_JOY_B4, 0u, axes_zero);
        REQUIRE(sendto(fake, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&emitter_addr, sizeof(emitter_addr)) == (int)len,
                "hello-t4-send");
        Sleep(50);
        int accepted_h = gm_poll_inputs(h);
        REQUIRE(accepted_h == 1, "hello-t4-accepted");

        uint32_t frame_before = h->last_joy.frame;
        REQUIRE(gm_send_input_hello(h) == 0, "hello-t4-send-hello");

        /* Drain the byte sent so it does not pollute a later test. */
        struct sockaddr_in from_c;
        uint8_t hc[8];
        int nc = fake_recv(fake, hc, (int)sizeof(hc), &from_c, 1000);
        REQUIRE(nc == 1, "hello-t4-drain-hello");

        gm_joy_inputs got_h;
        REQUIRE(gm_get_joy_inputs(h, &got_h) == 0, "hello-t4-get");
        REQUIRE(got_h.frame == frame_before && got_h.joy1 == (uint16_t)GM_JOY_B4,
                "hello-t4-state-survives");
        REQUIRE(h->last_joy.frame == frame_before, "hello-t4-last_joy-not-reset");
    }

    /* T3 (second half): after gm_close_inputs, the channel is no longer
     * hooked -> -1, and nothing is sent. Dedicated handle so as not to
     * touch `h`, which is still used further below. */
    {
        gm_handle *h5 = gm_init("127.0.0.1", 32100);
        REQUIRE(h5 != NULL, "hello-t3b-init");
        REQUIRE(gm_bind_inputs(h5, fake_port) == 0, "hello-t3b-bind");

        struct sockaddr_in hello_addr5;
        uint8_t hello5[8];
        int hn5 = fake_recv(fake, hello5, (int)sizeof(hello5), &hello_addr5, 1000);
        REQUIRE(hn5 == 1, "hello-t3b-hello-length");

        gm_close_inputs(h5);
        REQUIRE(gm_send_input_hello(h5) == -1, "hello-t3b-after-close");

        struct sockaddr_in dummy_b;
        uint8_t dummy_buf_b[8];
        int nd_b = fake_recv(fake, dummy_buf_b, (int)sizeof(dummy_buf_b), &dummy_b, 200);
        REQUIRE(nd_b == -1, "hello-t3b-nothing-sent");

        gm_close(h5);
    }

    /* ---------------------------------------------------------------------
     * The RED case: a client restarting on the SAME socket as before (the
     * channel stays open, nobody calls gm_bind_inputs) must see its inputs
     * dropped en masse. This is the INVERSE of the rebind block above:
     * rebinding closes and REOPENS the socket (it is the RECEIVER that
     * restarts) and proves the guard IS reset to zero; here it is the
     * CLIENT that restarts, the socket does not move, and this case proves
     * the guard is NOT reset to zero. Dedicated handle h6 so as not to
     * disturb any existing case.
     * --------------------------------------------------------------------- */
    {
        gm_handle *h6 = gm_init("127.0.0.1", 32100);
        REQUIRE(h6 != NULL, "restart-init");
        REQUIRE(gm_bind_inputs(h6, fake_port) == 0, "restart-bind");

        struct sockaddr_in emitter6;
        uint8_t hello6[8];
        int hn6 = fake_recv(fake, hello6, (int)sizeof(hello6), &emitter6, 1000);
        REQUIRE(hn6 == 1, "restart-hello-length");

        uint8_t buf[64];
        int8_t  axes_zero[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

        /* Client 1: a round that leaves the guard high at 500. */
        size_t len = build_joy17(buf, 500u, 0u, (uint16_t)GM_JOY_B1, 0u, axes_zero);
        REQUIRE(sendto(fake, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&emitter6, sizeof(emitter6)) == (int)len,
                "restart-c1-send");

        Sleep(50);
        int accepted_c1 = gm_poll_inputs(h6);
        REQUIRE(accepted_c1 == 1, "restart-c1-accepted");
        REQUIRE(h6->last_joy.frame == 500u, "restart-c1-guard-at-500");

        /* Client 2: SAME handle, SAME socket, no rebind between the two --
         * that is the whole difference from the rebind block. Three
         * datagrams of frames 1, 2, 3: a freshly restarted client's
         * counter. */
        uint32_t stale_before = h6->in_stale;
        uint32_t src_before   = h6->in_rejected_src;

        len = build_joy17(buf, 1u, 0u, (uint16_t)GM_JOY_B2, 0u, axes_zero);
        REQUIRE(sendto(fake, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&emitter6, sizeof(emitter6)) == (int)len,
                "restart-c2-send-1");
        len = build_joy17(buf, 2u, 0u, (uint16_t)GM_JOY_B2, 0u, axes_zero);
        REQUIRE(sendto(fake, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&emitter6, sizeof(emitter6)) == (int)len,
                "restart-c2-send-2");
        len = build_joy17(buf, 3u, 0u, (uint16_t)GM_JOY_B2, 0u, axes_zero);
        REQUIRE(sendto(fake, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&emitter6, sizeof(emitter6)) == (int)len,
                "restart-c2-send-3");

        Sleep(50);
        int accepted_c2 = gm_poll_inputs(h6);
        printf("[restart] client restarted (frames 1..3 after a round at 500): "
               "%d accepted, in_stale %u -> %u, in_rejected_src %u -> %u\n",
               accepted_c2, (unsigned)stale_before, (unsigned)h6->in_stale,
               (unsigned)src_before, (unsigned)h6->in_rejected_src);

        REQUIRE(accepted_c2 == 0, "restart-c2-nothing-accepted");
        REQUIRE(h6->in_stale == stale_before + 3u, "restart-c2-three-stale");
        /* Counterpart of the rejected_src=0 measured on the bench: the
         * datagrams were indeed RECEIVED then dropped, the provenance
         * filter has nothing to do with it. */
        REQUIRE(h6->in_rejected_src == src_before, "restart-c2-received-not-filtered");

        gm_joy_inputs got6;
        REQUIRE(gm_get_joy_inputs(h6, &got6) == 0, "restart-c2-get");
        REQUIRE(got6.frame == 500u && got6.joy1 == (uint16_t)GM_JOY_B1,
                "restart-c2-state-stays-with-client-1");

        /* -----------------------------------------------------------------
         * The GREEN case: resetting the guard, on the SAME socket, WITHOUT
         * rebinding. The frame=1 datagram dropped three lines above is
         * resent and must now be accepted.
         * ------------------------------------------------------------- */
        uint32_t stale_after_red = h6->in_stale;

        REQUIRE(gm_input_reset_peer_guard(h6) == 0, "reset-returns-0");

        /* The three no-effect REQUIREs, the most important half of the
         * test: the guard drops, nothing else moves. */
        REQUIRE(h6->in_stale == stale_after_red, "reset-does-not-touch-in_stale");
        REQUIRE(h6->has_joy == 1, "reset-does-not-touch-has_joy");
        REQUIRE(h6->last_joy.joy1 == (uint16_t)GM_JOY_B1,
                "reset-does-not-touch-the-buttons");

        REQUIRE(h6->last_joy.frame == 0u && h6->last_joy.order == 0u &&
                h6->last_ps2.frame == 0u && h6->last_ps2.order == 0u,
                "reset-guard-at-zero");

        /* The same datagram just dropped, resent on the same socket. */
        len = build_joy17(buf, 1u, 0u, (uint16_t)GM_JOY_B2, 0u, axes_zero);
        REQUIRE(sendto(fake, (const char *)buf, (int)len, 0,
                       (struct sockaddr *)&emitter6, sizeof(emitter6)) == (int)len,
                "reset-c2-send");

        Sleep(50);
        int accepted_after_reset = gm_poll_inputs(h6);
        printf("[restart] after resetting the guard: %d accepted, "
               "in_stale stays at %u\n",
               accepted_after_reset, (unsigned)h6->in_stale);

        REQUIRE(accepted_after_reset == 1, "reset-c2-accepted");
        REQUIRE(h6->in_stale == stale_after_red, "reset-c2-no-new-stale");

        gm_joy_inputs got6b;
        REQUIRE(gm_get_joy_inputs(h6, &got6b) == 0, "reset-c2-client-2-state");
        REQUIRE(got6b.frame == 1u && got6b.joy1 == (uint16_t)GM_JOY_B2,
                "reset-c2-client-2-state");

        gm_close(h6);
    }

    /* The two error cases: null handle, and a channel never hooked. */
    {
        REQUIRE(gm_input_reset_peer_guard(NULL) == -1, "reset-null-handle");
        gm_handle *h7 = gm_init("127.0.0.1", 32100);
        REQUIRE(h7 != NULL, "reset-h7-init");
        REQUIRE(gm_input_reset_peer_guard(h7) == -1, "reset-channel-not-hooked");
        gm_close(h7);
    }

    gm_close(h);
    closesocket(fake);
    WSACleanup();

    printf("ALL %d input-channel tests PASSED\n", g_checks);
    return 0;
}
