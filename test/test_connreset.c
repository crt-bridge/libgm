/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_connreset.c — on Windows, a datagram sent to
 * a closed UDP port earns an ICMP "port unreachable", and Winsock queues one
 * WSAECONNRESET per ICMP on the sending socket, AHEAD of the datagrams that
 * arrive after. gm_recv_status returns -1 on such an error, gm_poll_inputs
 * stops on it: a peer that opens late is read seconds late. libgm turns the
 * report off (SIO_UDP_CONNRESET = FALSE) on both of its sockets.
 *
 * Loopback only, ports 42400-42403 -- never the bench's 32100/32101/32110.
 * No RetroArch. Two cases:
 *
 *   1. VIDEO SOCKET. A gm_init handle sends N datagrams to its closed port,
 *      then a peer opens that port and sends a 13-byte status: the FIRST
 *      gm_recv_status must return it (0), with the right fields.
 *   2. INPUT SOCKET. gm_bind_inputs, then gm_send_input_hello, toward a
 *      closed port; then a peer opens it and sends a 9-byte joypad state:
 *      the FIRST gm_poll_inputs must accept it (1).
 *
 * A WITNESS in each case: a plain socket, WITHOUT libgm's treatment, sends
 * to a second closed port interleaved with the handle's sends, and the same
 * late peer answers it. Its first read MUST fail. That proves ICMP errors
 * were generated in that very window -- otherwise the case would pass on a
 * setup blind to the defect, and the test fails rather than lie. Windows
 * rate-limits ICMP errors (a burst of 200 sends left 100 to 196 errors, and
 * a case run right after got none): hence sends paced 2 ms apart, few of
 * them, and one second between the two cases.
 *
 * On a failure, the number of calls needed before the datagram comes out is
 * printed: that is the length of the queue, i.e. the size of the defect.
 *
 * Windows only: Linux reports nothing on an unconnected UDP socket.
 */
#ifndef _WIN32
#include <stdio.h>
int main(void) { printf("test_connreset : skipped outside Windows\n"); return 0; }
#else

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "gm.h"
#include "gm_internal.h"
#include "gm_socket.h"

#define PORT_VIDEO          42400u
#define PORT_VIDEO_WITNESS  42401u
#define PORT_INPUT          42402u
#define PORT_INPUT_WITNESS  42403u
#define N_SENT              20
#define PACE_MS             2
#define MAX_READS           200

static int fails;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) fails++;
}

static struct sockaddr_in loopback(uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

static uint16_t local_port(SOCKET s)
{
    struct sockaddr_in a;
    int len = (int)sizeof a;
    memset(&a, 0, sizeof a);
    if (getsockname(s, (struct sockaddr *)&a, &len) == SOCKET_ERROR) return 0;
    return ntohs(a.sin_port);
}

/* Plain socket, no SIO_UDP_CONNRESET: the old behavior. */
static SOCKET witness_open(void)
{
    SOCKET w = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in any = loopback(0);
    if (w != INVALID_SOCKET) bind(w, (struct sockaddr *)&any, (int)sizeof any);
    return w;
}

static void witness_send(SOCKET w, uint16_t port)
{
    struct sockaddr_in dst = loopback(port);
    uint8_t b[32];
    memset(b, 0, sizeof b);
    sendto(w, (const char *)b, (int)sizeof b, 0, (struct sockaddr *)&dst, (int)sizeof dst);
}

/* The late peer: opens `port` (closed until now) and sends `len` bytes to
 * 127.0.0.1:`to_port`. The socket is returned, kept open by the caller. */
static SOCKET peer_open_and_send(uint16_t port, uint16_t to_port, const uint8_t *buf, int len)
{
    SOCKET p = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (p == INVALID_SOCKET) return INVALID_SOCKET;
    struct sockaddr_in me = loopback(port);
    if (bind(p, (struct sockaddr *)&me, (int)sizeof me) == SOCKET_ERROR) {
        closesocket(p);
        return INVALID_SOCKET;
    }
    struct sockaddr_in dst = loopback(to_port);
    (void)sendto(p, (const char *)buf, len, 0, (struct sockaddr *)&dst, (int)sizeof dst);
    return p;
}

/* Witness verdict: its first read must FAIL, then the datagram comes out. */
static void witness_check(const char *label, SOCKET w, int expected_len)
{
    uint8_t b[64];
    int failed = 0, got = 0;
    for (int i = 0; i < MAX_READS; i++) {
        int n = gm_socket_recvfrom_timeout((int)w, b, sizeof b, 100);
        if (n == expected_len) { got = 1; break; }
        failed++;
    }
    printf("     %s witness: %d failed read(s) before its datagram (%s)\n",
           label, failed, got ? "read" : "never read");
    char name[160];
    snprintf(name, sizeof name,
             "%s witness: a socket without the treatment queues the errors (first read fails)",
             label);
    check(name, failed > 0 && got);
}

/* Case 1: the handle's video socket. */
static void case_video(void)
{
    gm_handle *h = gm_init("127.0.0.1", (uint16_t)PORT_VIDEO);
    check("video: gm_init opens the handle", h != NULL);
    if (!h) return;
    SOCKET w = witness_open();

    uint8_t buf[1472];
    memset(buf, 0, sizeof buf);
    int sent = 0;
    for (int i = 0; i < N_SENT; i++) {
        if (gm_socket_sendto(h->sock, buf, sizeof buf, &h->to) == (int)sizeof buf) sent++;
        witness_send(w, (uint16_t)PORT_VIDEO_WITNESS);
        Sleep(PACE_MS);
    }
    check("video: the N datagrams leave toward the closed port", sent == N_SENT);
    Sleep(100);

    /* frame_echo 7, vcount_echo 3, frame 42, vcount 100, flags 0x05 */
    uint8_t wire[13] = { 7,0,0,0, 3,0, 42,0,0,0, 100,0, 0x05 };
    SOCKET p  = peer_open_and_send((uint16_t)PORT_VIDEO, local_port((SOCKET)h->sock), wire, 13);
    SOCKET pw = peer_open_and_send((uint16_t)PORT_VIDEO_WITNESS, local_port(w), wire, 13);
    check("video: the late peer opens the port", p != INVALID_SOCKET && pw != INVALID_SOCKET);
    Sleep(50);

    gm_status st;
    memset(&st, 0, sizeof st);
    int rc = gm_recv_status(h, &st, 100);
    check("video: the FIRST gm_recv_status returns the status (0)", rc == 0);
    if (rc == 0) {
        check("video: fields decoded (frame_echo 7, frame 42, vcount 100, flags 5)",
              st.frame_echo == 7u && st.vcount_echo == 3u && st.frame == 42u
              && st.vcount == 100u && st.flags == 0x05u);
    } else {
        int calls = 1;
        while (calls < MAX_READS && gm_recv_status(h, &st, 100) != 0) calls++;
        printf("     video: the status came out on call %d\n", calls + 1);
    }
    witness_check("video", w, 13);

    if (p  != INVALID_SOCKET) closesocket(p);
    if (pw != INVALID_SOCKET) closesocket(pw);
    closesocket(w);
    gm_close(h);
}

/* Case 2: the input socket. */
static void case_input(void)
{
    gm_handle *h = gm_init("127.0.0.1", (uint16_t)PORT_VIDEO);
    check("input: gm_init opens the handle", h != NULL);
    if (!h) return;
    SOCKET w = witness_open();

    int rc = gm_bind_inputs(h, (uint16_t)PORT_INPUT);
    witness_send(w, (uint16_t)PORT_INPUT_WITNESS);
    check("input: gm_bind_inputs sends the binding byte to the closed port", rc == 0);
    if (rc != 0) { closesocket(w); gm_close(h); return; }
    int hellos = 0;
    for (int i = 0; i < N_SENT - 1; i++) {
        Sleep(PACE_MS);
        if (gm_send_input_hello(h) == 0) hellos++;
        witness_send(w, (uint16_t)PORT_INPUT_WITNESS);
    }
    check("input: the binding byte is resent", hellos == N_SENT - 1);
    Sleep(100);

    /* 9-byte joypad state: frame 1, order 0, joy1 0x0001, joy2 0 */
    uint8_t joy[9] = { 1,0,0,0, 0, 1,0, 0,0 };
    SOCKET p  = peer_open_and_send((uint16_t)PORT_INPUT, local_port((SOCKET)h->input_sock), joy, 9);
    SOCKET pw = peer_open_and_send((uint16_t)PORT_INPUT_WITNESS, local_port(w), joy, 9);
    check("input: the late peer opens the port", p != INVALID_SOCKET && pw != INVALID_SOCKET);
    Sleep(50);

    int acc = gm_poll_inputs(h);
    check("input: the FIRST gm_poll_inputs accepts the joypad state (1)", acc == 1);
    if (acc != 1) {
        int calls = 1;
        while (calls < MAX_READS && gm_poll_inputs(h) != 1) calls++;
        printf("     input: the state came out on call %d\n", calls + 1);
    } else {
        gm_joy_inputs in;
        check("input: joy1 == 0x0001",
              gm_get_joy_inputs(h, &in) == 0 && in.joy1 == 0x0001u);
    }
    witness_check("input", w, 9);

    if (p  != INVALID_SOCKET) closesocket(p);
    if (pw != INVALID_SOCKET) closesocket(pw);
    closesocket(w);
    gm_close(h);
}

int main(void)
{
    if (gm_socket_init_winsock() != 0) {
        fprintf(stderr, "test_connreset: WSAStartup failed\n");
        return 1;
    }
    case_video();
    Sleep(1000);   /* let the ICMP error rate limit recover */
    case_input();
    if (fails) { fprintf(stderr, "test_connreset: %d failure(s)\n", fails); return 1; }
    printf("test_connreset : ok\n");
    return 0;
}

#endif /* _WIN32 */
