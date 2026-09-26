/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gm_socket.c — Winsock2 UDP transport primitives for libgm.
 *
 * Tiny POSIX-style wrappers over Winsock so the rest of
 * libgm doesn't have to drag <winsock2.h> into every translation unit. All
 * functions return 0 on success, -1 on error (or bytes for sendto/recvfrom).
 *
 * Threading model: WSAStartup is called once per process via a static guard.
 * Sockets themselves are per-handle (owned by gm_handle in gm.c); no shared
 * state lives here.
 */

#include "gm_socket.h"

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>   /* getaddrinfo */
/* ws2ipdef.h, Windows 10 2004 and later -- absent from the MinGW headers in
 * use on this machine (checked 2026-09-25). */
#  ifndef UDP_SEND_MSG_SIZE
#  define UDP_SEND_MSG_SIZE 2
#  endif
typedef SOCKET  gm_sock_t;
#  define GM_BAD_SOCK  INVALID_SOCKET
#else
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>      /* getaddrinfo */
#  include <unistd.h>     /* close */
#  include <errno.h>
typedef int     gm_sock_t;
#  define GM_BAD_SOCK  (-1)
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
static int g_winsock_inited = 0;
#endif

int gm_socket_init_winsock(void)
{
#ifdef _WIN32
    if (g_winsock_inited) return 0;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    g_winsock_inited = 1;
#endif
    /* POSIX: nothing to initialize. The name stays the original one -- gm.c
     * calls it, and renaming it would make a needless diff where a plain
     * transposition is what's wanted. */
    return 0;
}

int gm_socket_open(const char *ip, uint16_t port,
                   int *out_sock, struct sockaddr_in *out_to)
{
    struct addrinfo  hints;
    struct addrinfo *res = NULL;
    char             port_txt[8];
    gm_sock_t        s;

    if (!ip || !out_sock || !out_to) return -1;

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == GM_BAD_SOCK) return -1;

    /* A hostname is accepted everywhere an address used to be -- full name,
     * short name, .local name. A human types a name, not four numbers, and
     * this puts two address fields in front of their eyes.
     *
     * WHAT CHANGES COMPARED TO THE OLD RESOLUTION (Winsock, a function
     * deprecated for a long time), and is ACCEPTED:
     *   - a name resolves (that's the point);
     *   - 255.255.255.255 is accepted -- the old function gave it the same
     *     value as an error, an oddity noted in groovy_followers.h;
     *   - exotic forms no longer pass: the old function accepted octal,
     *     hexadecimal, and short forms, so that "192.0.2.010" silently
     *     targeted 192.0.2.8. getaddrinfo is strict. That is a gain, and
     *     the emitter's test suite now says so.
     *
     * CALLED ONCE PER SESSION, from gm_init, never in an image's send path.
     * Resolution is therefore FIXED for the session's duration: a receiver
     * that changes address mid-game requires a restart. Accepted limit,
     * documented in the emitter's user guide. */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;        /* the Groovy protocol is IPv4 */
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    snprintf(port_txt, sizeof(port_txt), "%u", (unsigned)port);

    if (getaddrinfo(ip, port_txt, &hints, &res) != 0 || !res) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return -1;               /* unknown name: clean refusal, nothing sent */
    }

    memset(out_to, 0, sizeof(*out_to));
    memcpy(out_to, res->ai_addr, sizeof(*out_to));
    freeaddrinfo(res);

    out_to->sin_family = AF_INET;
    out_to->sin_port   = htons(port);

    *out_sock = (int)s;
    return 0;
}

void gm_socket_close(int sock)
{
    if (sock < 0) return;
#ifdef _WIN32
    closesocket((SOCKET)sock);
#else
    close(sock);
#endif
}

int gm_socket_sendto_err(int sock, const void *buf, size_t len,
                         const struct sockaddr_in *to, int *out_err)
{
    if (sock < 0 || !buf || !to) {
        if (out_err) *out_err = GM_SOCK_ERR_REFUSED;
        return -1;
    }
#ifdef _WIN32
    int n = sendto((SOCKET)sock, (const char *)buf, (int)len, 0,
                   (const struct sockaddr *)to, (int)sizeof(*to));
    if (n == SOCKET_ERROR) {
        /* Read HERE, before any other Winsock call: WSAGetLastError is
         * overwritten by the next call, even a successful one (Microsoft
         * Learn). */
        if (out_err) *out_err = WSAGetLastError();
        return -1;
    }
#else
    int n = (int)sendto(sock, buf, len, 0,
                        (const struct sockaddr *)to, (socklen_t)sizeof(*to));
    if (n < 0) {
        /* Same discipline: errno is read HERE, immediately. On POSIX these
         * are errno codes (ECONNREFUSED, ENOBUFS...), not Winsock codes --
         * the SEMANTICS of gm_socket.h's contract hold (> 0 = platform code;
         * -1 and -2 reserved), not the numeric value itself. */
        if (out_err) *out_err = errno;
        return -1;
    }
#endif
    if (out_err) *out_err = (n == (int)len) ? 0 : GM_SOCK_ERR_SHORT;
    return n;
}

int gm_socket_sendto(int sock, const void *buf, size_t len,
                     const struct sockaddr_in *to)
{
    return gm_socket_sendto_err(sock, buf, len, to, NULL);
}

/* setsockopt then a READBACK via getsockopt -- Linux doubles and caps the
 * requested value (man 7 socket), Windows returns it unchanged once
 * accepted. Returning the value read back, never `bytes`, is what lets
 * gm_send_offload_state (gm.c) report the queue that is actually applied,
 * not the one that was requested. */
int gm_socket_set_sndbuf(int sock, int bytes)
{
    if (sock < 0) return -1;
    int v = bytes;
#ifdef _WIN32
    if (setsockopt((SOCKET)sock, SOL_SOCKET, SO_SNDBUF, (const char *)&v, sizeof(v)) == SOCKET_ERROR)
        return -1;
    int got = 0;
    int gl  = sizeof(got);
    if (getsockopt((SOCKET)sock, SOL_SOCKET, SO_SNDBUF, (char *)&got, &gl) == SOCKET_ERROR)
        return -1;
#else
    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)) < 0)
        return -1;
    int       got = 0;
    socklen_t gl  = sizeof(got);
    if (getsockopt(sock, SOL_SOCKET, SO_SNDBUF, &got, &gl) < 0)
        return -1;
#endif
    return got;
}

/* See the full contract in gm_socket.h. Windows only -- POSIX attempts
 * nothing (UDP_SEGMENT is a different mechanism, out of scope here) and
 * returns -1 with *out_err = 0, never a platform code that would make no
 * sense here. */
int gm_socket_set_send_segment(int sock, unsigned seg, int *out_err)
{
    if (out_err) *out_err = 0;
    if (sock < 0) return -1;
#ifdef _WIN32
    DWORD d = (DWORD)seg;
    if (setsockopt((SOCKET)sock, IPPROTO_UDP, UDP_SEND_MSG_SIZE,
                   (const char *)&d, sizeof(d)) == SOCKET_ERROR) {
        /* Read HERE, before any other Winsock call -- same discipline as
         * gm_socket_sendto_err. */
        if (out_err) *out_err = WSAGetLastError();
        return -1;
    }
    DWORD got = 0;
    int   gl  = sizeof(got);
    if (getsockopt((SOCKET)sock, IPPROTO_UDP, UDP_SEND_MSG_SIZE, (char *)&got, &gl) == SOCKET_ERROR
        || got != d) {
        /* setsockopt DID SUCCEED, but the readback fails or disagrees. The
         * socket may carry a segment the caller is about to believe absent
         * (it gets -1 back): reset the option to 0 before returning, or the
         * original path would be silently re-split. If this reset also
         * fails, the caller reads back the real state via
         * gm_socket_get_send_segment (gm_uso_disarm, gm.c). Not a Winsock
         * refusal code: out_err stays 0. */
        if (d != 0u) {
            DWORD zero = 0;
            (void)setsockopt((SOCKET)sock, IPPROTO_UDP, UDP_SEND_MSG_SIZE,
                             (const char *)&zero, sizeof(zero));
        }
        return -1;
    }
    return 0;
#else
    (void)sock;
    (void)seg;
    return -1;
#endif
}

/* See the contract in gm_socket.h. */
int gm_socket_get_send_segment(int sock, unsigned *out_seg)
{
    if (!out_seg || sock < 0) return -1;
#ifdef _WIN32
    DWORD got = 0;
    int   gl  = sizeof(got);
    if (getsockopt((SOCKET)sock, IPPROTO_UDP, UDP_SEND_MSG_SIZE, (char *)&got, &gl) == SOCKET_ERROR)
        return -1;
    *out_seg = (unsigned)got;
#else
    *out_seg = 0u;         /* nothing is ever applied off Windows */
#endif
    return 0;
}

int gm_socket_recvfrom_timeout(int sock, void *buf, size_t cap, int timeout_ms)
{
    return gm_socket_recvfrom_timeout_from(sock, buf, cap, timeout_ms, NULL);
}

/* select()'s first argument is the NUMBER of descriptors to examine, not a
 * descriptor. Winsock ignores it, and libgm used to pass 0 there: on POSIX,
 * select() then returns 0 FOREVER -- the binary compiles, starts, and never
 * receives anything, without a single error. Extracted as a pure function so
 * test/test_select_nfds.c can assert it without opening a socket, on
 * every `make test`. */
static int gm_select_nfds(int sock)
{
#ifdef _WIN32
    (void)sock;
    return 0;              /* Winsock idiom: this parameter is ignored */
#else
    return sock + 1;
#endif
}

int gm_socket_recvfrom_timeout_from(int sock, void *buf, size_t cap, int timeout_ms,
                                    struct sockaddr_in *out_from)
{
    if (sock < 0 || !buf) return -1;

    fd_set rfds;
    FD_ZERO(&rfds);
#ifdef _WIN32
    FD_SET((SOCKET)sock, &rfds);
#else
    FD_SET(sock, &rfds);
#endif

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int rc = select(gm_select_nfds(sock), &rfds, NULL, NULL, &tv);
    if (rc <= 0) return -1;  /* timeout (0) or error (SOCKET_ERROR) */

    struct sockaddr_in from;
#ifdef _WIN32
    int from_len = (int)sizeof(from);
    int n = recvfrom((SOCKET)sock, (char *)buf, (int)cap, 0,
                    (struct sockaddr *)&from, &from_len);
#else
    socklen_t from_len = (socklen_t)sizeof(from);
    int n = (int)recvfrom(sock, buf, cap, 0,
                    (struct sockaddr *)&from, &from_len);
#endif
#ifdef _WIN32
    if (n == SOCKET_ERROR) {
        /* On Winsock, a datagram bigger than the buffer fails with
         * WSAEMSGSIZE -- but it is still CONSUMED (truncated) from the
         * queue. Confusing that with -1 ("nothing left pending") would cut
         * a drain short on a single oversized datagram, with no trace in
         * any counter. Reported as an invalid length (cap + 1), which the
         * caller's length-based dispatch rejects. */
        if (WSAGetLastError() == WSAEMSGSIZE) {
            if (out_from) *out_from = from;
            return (int)cap + 1;
        }
        return -1;
    }
#else
    /* There is no POSIX equivalent to the Winsock WSAEMSGSIZE behavior above,
     * and none is manufactured here. BSD truncates an oversized datagram
     * without error; only MSG_TRUNC would reveal it, and that is a LINUX
     * extension absent from macOS -- using it would build exactly the
     * two-speed library this project refuses. Without consequence here: libgm's
     * buffers cover every legitimate Groovy payload. Only worth reopening if
     * a future caller passes a smaller buffer. */
    if (n < 0) return -1;
#endif
    if (out_from) *out_from = from;
    return n;
}
