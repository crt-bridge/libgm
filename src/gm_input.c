/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gm_input.c — Groovy input channel (UDP/32101), CONSUMER side.
 *
 * Binding by a single byte, non-blocking drain, strict dispatch by datagram
 * length. The out-of-order guard on (frame, order) is the gm_seq_is_newer
 * function, defined in gm_seq.h: written from the wire protocol's ordering
 * semantics -- frame is the primary key, order only breaks ties at equal
 * frame -- not copied from any prior source.
 *
 * REVERSED DIRECTION — read this before touching this file. On the video
 * channel (32100, gm_proto.c), it is US (the sender) pushing the main
 * stream and the receiver acknowledging it. On this channel (32101), it is
 * the OPPOSITE: the RECEIVER pushes the joypad/keyboard/mouse stream, and WE
 * only send a single byte to make ourselves known. Same architecture pattern
 * as `BindInputs`/`init_socket`: groovymister.cpp:839-869
 * (psakhis/Groovy_MiSTer, GPL-2.0-only) and input_mister.cpp:138-212
 * (antonioginer/GroovyMAME, BSD-3-Clause) both document it independently;
 * only the description of the pattern is reused here, no line is borrowed,
 * and the receiver/sender roles are reversed compared to the video channel.
 * A reflexive port of gm_proto.c would wrongly assume that we push here too.
 *
 * No allocation in gm_poll_inputs: it runs in RetroArch's main loop.
 * Receive buffer on the stack, fixed size.
 */

#include "gm.h"
#include "gm_internal.h"
#include "gm_seq.h"
#include "gm_socket.h"

#ifdef _WIN32
#  include <winsock2.h>
typedef SOCKET gm_in_sock_t;
#  define GM_IN_BAD_SOCK  INVALID_SOCKET
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
typedef int gm_in_sock_t;
#  define GM_IN_BAD_SOCK  (-1)
#endif
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * gm_bind_inputs — opens the input channel socket and sends the binding
 * byte. port == 0 -> video port + 1 (see the gm.h note on why this
 * derivation was chosen over hardcoding 32101).
 * ------------------------------------------------------------------------- */
int gm_bind_inputs(gm_handle *h, uint16_t port)
{
    if (!h) return -1;

    /* A second call does not leak a descriptor: close whatever might
     * already be open first. */
    if (h->input_sock >= 0) {
        gm_close_inputs(h);
    }

    h->input_to = h->to;
    uint16_t p = port ? port : (uint16_t)(ntohs(h->to.sin_port) + 1u);
    h->input_to.sin_port = htons(p);

    /* The address is already resolved in input_to (derived from h->to, set
     * by gm_init): no need to go back through gm_socket_open and its
     * inet_addr(). Raw socket, as upstream does. */
    gm_in_sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == GM_IN_BAD_SOCK) return -1;

    /* Non-blocking: gm_poll_inputs must never wait for a datagram. */
#ifdef _WIN32
    {
        u_long nb = 1;
        /* FIONBIO is a u_long whose value exceeds LONG_MAX on 32 bits;
         * ioctlsocket takes a signed `long`. Explicit cast, not a
         * -Wconversion silenced globally. */
        if (ioctlsocket(s, (long)FIONBIO, &nb) != 0) { closesocket(s); return -1; }
    }
#else
    /* The UPSTREAM form, the one this file's header already named: the port
     * only restores it where it exists natively. */
    if (fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) != 0) { close(s); return -1; }
#endif

    h->input_sock = (int)s;

    /* Binding takes a SINGLE byte — see this file's header. No bind() on a
     * fixed local port: the source port is whatever the OS assigns, and the
     * receiver learns it upon receiving this byte. */
    uint8_t hello = 0;
    if (gm_socket_sendto(h->input_sock, &hello, 1, &h->input_to) != 1) {
        gm_socket_close(h->input_sock);
        h->input_sock = -1;
        return -1;
    }

    h->has_joy          = 0;
    h->has_ps2          = 0;
    h->in_rejected_len  = 0;
    h->in_rejected_src  = 0;
    h->in_stale         = 0;
    h->mouse_acc_x      = 0;
    h->mouse_acc_y      = 0;
    h->mouse_acc_z      = 0;
    /* The sequence guard (frame/order, in gm_poll_inputs below) compares
     * against the last_joy/last_ps2 counters. Without this reset, rebinding
     * on the SAME handle after the receiver restarts (its counter back near
     * zero) would count everything as in_stale until the counter catches
     * back up with the old value -- potentially hours of dead input. */
    memset(&h->last_joy, 0, sizeof(h->last_joy));
    memset(&h->last_ps2, 0, sizeof(h->last_ps2));

    fprintf(stderr, "[gm-input] bound on port %u\n", (unsigned)p);
    return 0;
}

/* ---------------------------------------------------------------------------
 * gm_send_input_hello — resends the binding byte ON THE ALREADY-OPEN
 * SOCKET, without closing or reopening it (see the gm.h comment). Emits no
 * stderr trace (called up to three times per peer and per session, the
 * telemetry lives on the caller's side), resets nothing, opens no socket.
 * ------------------------------------------------------------------------- */
int gm_send_input_hello(gm_handle *h)
{
    if (!h) return -1;
    if (h->input_sock < 0) return -1;

    uint8_t hello = 0;
    if (gm_socket_sendto(h->input_sock, &hello, 1, &h->input_to) != 1)
        return -1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * gm_input_reset_peer_guard — resets the sequence guard when a restarted
 * peer comes back on the SAME socket. See gm.h for the full contract and the
 * ordering requirement (call BEFORE gm_send_input_hello).
 *
 * Deliberately NOT a full memset of last_joy/last_ps2: only
 * last_joy.frame/order and last_ps2.frame/order are reset. Leaving has_joy
 * at 1 avoids a display gap -- a memset would produce a zeroed state, i.e.
 * all buttons released, not the previous peer's state. Four fields, not one
 * more.
 * ------------------------------------------------------------------------- */
int gm_input_reset_peer_guard(gm_handle *h)
{
    if (!h) return -1;
    if (h->input_sock < 0) return -1;

    /* Four fields, not one more -- see gm.h for the reason behind each
     * exclusion. A datagram numbered 0 would still be rejected after this
     * call; without consequence, the remote core's turn counter starts at 1
     * (it is incremented before sending). */
    h->last_joy.frame = 0u;
    h->last_joy.order = 0u;
    h->last_ps2.frame = 0u;
    h->last_ps2.order = 0u;

    return 0;
}

/* ---------------------------------------------------------------------------
 * gm_poll_inputs — drains the input socket, up to 64 datagrams per call
 * (hard bound: a hostile peer does not get to run this loop indefinitely
 * inside the video loop).
 *
 * Returns the number of ACCEPTED datagrams (which updated last_joy or
 * last_ps2), -1 if the channel is not bound.
 * ------------------------------------------------------------------------- */
int gm_poll_inputs(gm_handle *h)
{
    if (!h) return -1;
    if (h->input_sock < 0) return -1;

    int accepted = 0;

    for (int i = 0; i < 64; i++) {
        uint8_t buf[64];
        struct sockaddr_in from;

        int n = gm_socket_recvfrom_timeout_from(h->input_sock, buf, sizeof(buf),
                                                0, &from);
        if (n < 0) break;   /* nothing left pending on the socket */

        /* Oversized datagram (> buffer), consumed truncated by Winsock
         * (WSAEMSGSIZE, reported as cap + 1 by the socket layer). Counted
         * as a rejected length and the drain continues -- before this fix
         * it was confused with "nothing left pending", cut the frame's
         * drain short, and appeared in no counter. */
        if (n > (int)sizeof(buf)) {
            h->in_rejected_len++;
            continue;
        }

        /* Provenance filter: only the receiver's IP address is checked, not
         * the port — nothing in the protocol guarantees that the receiver
         * sends from the same port seen at binding time. */
        if (from.sin_addr.s_addr != h->input_to.sin_addr.s_addr) {
            h->in_rejected_src++;
            continue;
        }

        /* STRICT dispatch by length — four exact cases, nothing else is
         * read (V5 Input Validation). */
        switch (n) {
        case 9: {
            uint32_t frame;
            uint8_t  order;
            uint16_t joy1, joy2;
            memcpy(&frame, buf + 0, sizeof(uint32_t));
            order = buf[4];
            memcpy(&joy1, buf + 5, sizeof(uint16_t));
            memcpy(&joy2, buf + 7, sizeof(uint16_t));

            if (gm_seq_is_newer(frame, order, h->last_joy.frame, h->last_joy.order)) {
                h->last_joy.frame = frame;
                h->last_joy.order = order;
                h->last_joy.joy1  = joy1;
                h->last_joy.joy2  = joy2;
                /* Short message = "no analog", not "keep the old axes":
                 * all eight axes are set to zero. */
                h->last_joy.joy1_lx = 0; h->last_joy.joy1_ly = 0;
                h->last_joy.joy1_rx = 0; h->last_joy.joy1_ry = 0;
                h->last_joy.joy2_lx = 0; h->last_joy.joy2_ly = 0;
                h->last_joy.joy2_rx = 0; h->last_joy.joy2_ry = 0;
                h->has_joy = 1;
                accepted++;
            } else {
                h->in_stale++;
            }
            break;
        }
        case 17: {
            gm_joy_inputs in;
            memcpy(&in.frame, buf + 0, sizeof(uint32_t));
            in.order = buf[4];
            memcpy(&in.joy1, buf + 5, sizeof(uint16_t));
            memcpy(&in.joy2, buf + 7, sizeof(uint16_t));
            in.joy1_lx = (int8_t)buf[9];
            in.joy1_ly = (int8_t)buf[10];
            in.joy1_rx = (int8_t)buf[11];
            in.joy1_ry = (int8_t)buf[12];
            in.joy2_lx = (int8_t)buf[13];
            in.joy2_ly = (int8_t)buf[14];
            in.joy2_rx = (int8_t)buf[15];
            in.joy2_ry = (int8_t)buf[16];

            if (gm_seq_is_newer(in.frame, in.order, h->last_joy.frame, h->last_joy.order)) {
                h->last_joy = in;
                h->has_joy  = 1;
                accepted++;
            } else {
                h->in_stale++;
            }
            break;
        }
        case 37: {
            uint32_t frame;
            uint8_t  order;
            uint8_t  keys[32];
            memcpy(&frame, buf + 0, sizeof(uint32_t));
            order = buf[4];
            memcpy(keys, buf + 5, 32);

            if (gm_seq_is_newer(frame, order, h->last_ps2.frame, h->last_ps2.order)) {
                h->last_ps2.frame = frame;
                h->last_ps2.order = order;
                memcpy(h->last_ps2.keys, keys, 32);
                /* Short message = "no mouse": the four mouse bytes are set
                 * to zero. */
                h->last_ps2.mouse_bits = 0;
                h->last_ps2.mouse_x    = 0;
                h->last_ps2.mouse_y    = 0;
                h->last_ps2.mouse_z    = 0;
                h->has_ps2 = 1;
                accepted++;
            } else {
                h->in_stale++;
            }
            break;
        }
        case 41: {
            gm_ps2_inputs in;
            memcpy(&in.frame, buf + 0, sizeof(uint32_t));
            in.order = buf[4];
            memcpy(in.keys, buf + 5, 32);
            in.mouse_bits = buf[37];
            in.mouse_x    = buf[38];
            in.mouse_y    = buf[39];
            in.mouse_z    = buf[40];

            if (gm_seq_is_newer(in.frame, in.order, h->last_ps2.frame, h->last_ps2.order)) {
                h->last_ps2 = in;
                h->has_ps2  = 1;
                /* Mouse bytes are DELTAS per datagram -- accumulated here,
                 * at the drain point, so nothing is lost when several
                 * datagrams land within the same RetroArch frame. Signed
                 * decoding follows the GroovyMAME formula
                 * (input_mister.cpp:697-698): rel = byte - ((bits << n) &
                 * 0x100), i.e. byte - 256 when the sign bit is set (bit 4
                 * for x, shifted by 4; bit 5 for y, shifted by 3) -- NOT
                 * -255 (an upstream off-by-one: a delta of -1 would decode
                 * to 0). dz is an 8-bit two's complement, with no sign bit
                 * in mouse_bits (the receiver's encoder writes dz & 0xFF). */
                h->mouse_acc_x += (int)in.mouse_x - (((int)in.mouse_bits << 4) & 0x100);
                h->mouse_acc_y += (int)in.mouse_y - (((int)in.mouse_bits << 3) & 0x100);
                h->mouse_acc_z += (int)(int8_t)in.mouse_z;
                accepted++;
            } else {
                h->in_stale++;
            }
            break;
        }
        default:
            /* Nothing else is read — no decoding, no assumption about an
             * unknown length. */
            h->in_rejected_len++;
            continue;
        }
    }

    return accepted;
}

/* ---------------------------------------------------------------------------
 * gm_get_joy_inputs / gm_get_ps2_inputs — a plain copy of the current state.
 * The fork's translators (joypad driver) are what interprets the bits.
 * ------------------------------------------------------------------------- */
int gm_get_joy_inputs(gm_handle *h, gm_joy_inputs *out)
{
    if (!h || !out) return -1;
    if (!h->has_joy) return -1;
    *out = h->last_joy;
    return 0;
}

int gm_get_ps2_inputs(gm_handle *h, gm_ps2_inputs *out)
{
    if (!h || !out) return -1;
    if (!h->has_ps2) return -1;
    *out = h->last_ps2;
    return 0;
}

/* ---------------------------------------------------------------------------
 * gm_get_mouse_deltas — consumes the mouse-delta accumulator and resets it
 * to zero. See the case 41 comment in gm_poll_inputs for the decoding and
 * the reason the accumulation exists. NULL pointers accepted.
 * ------------------------------------------------------------------------- */
int gm_get_mouse_deltas(gm_handle *h, int *dx, int *dy, int *dz)
{
    if (!h) return -1;
    if (dx) *dx = h->mouse_acc_x;
    if (dy) *dy = h->mouse_acc_y;
    if (dz) *dz = h->mouse_acc_z;
    h->mouse_acc_x = 0;
    h->mouse_acc_y = 0;
    h->mouse_acc_z = 0;
    return 0;
}

/* ---------------------------------------------------------------------------
 * gm_close_inputs — closes the input socket if it is open. Silent if the
 * channel was never bound (useful since gm_close() calls it
 * unconditionally).
 * ------------------------------------------------------------------------- */
void gm_close_inputs(gm_handle *h)
{
    if (!h) return;
    if (h->input_sock < 0) return;

    fprintf(stderr,
            "[gm-input] closing — rejected_len=%u rejected_src=%u stale=%u\n",
            (unsigned)h->in_rejected_len, (unsigned)h->in_rejected_src,
            (unsigned)h->in_stale);

    gm_socket_close(h->input_sock);
    h->input_sock = -1;
}
