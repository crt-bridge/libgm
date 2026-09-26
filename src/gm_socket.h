/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gm_socket.h — internal UDP transport API for libgm.
 *
 * NOT a public header (lives in src/, not include/). Callers outside libgm
 * use the gm_handle wrappers in gm.h instead. This header exists so gm.c
 * and gm_proto.c can share the Winsock2 wrappers without each TU pulling
 * <winsock2.h> directly.
 */

#ifndef LIBGM_GM_SOCKET_H
#define LIBGM_GM_SOCKET_H

#include <stdint.h>
#include <stddef.h>

/* Forward-declare sockaddr_in so this header doesn't force <winsock2.h> on
 * every TU that includes it. The full definition is needed only in .c
 * files that actually call into the helpers. */
struct sockaddr_in;

/* WSAStartup once per process. Idempotent. Returns 0 on success. */
int  gm_socket_init_winsock(void);

/* Create a SOCK_DGRAM socket and fill *out_to with sockaddr_in for ip:port.
 * Returns 0 on success and writes the socket fd to *out_sock. */
int  gm_socket_open(const char *ip, uint16_t port,
                    int *out_sock, struct sockaddr_in *out_to);

void gm_socket_close(int sock);

/* sendto wrapper. Returns bytes sent (>=0) or -1 on error. */
int  gm_socket_sendto(int sock, const void *buf, size_t len,
                      const struct sockaddr_in *to);

/* Same thing as gm_socket_sendto, and additionally returns in *out_err (if
 * non-NULL) the EXACT outcome of the send:
 *   0                    send completed in full;
 *   > 0                  Winsock code, read via WSAGetLastError() IMMEDIATELY
 *                        after sendto's failure, before any other Winsock
 *                        call; or errno under POSIX, read immediately the
 *                        same way;
 *   GM_SOCK_ERR_REFUSED  refused before Winsock (sock < 0, buf or to null);
 *   GM_SOCK_ERR_SHORT    sendto returned a count different from len.
 * Return value identical to gm_socket_sendto. */
#define GM_SOCK_ERR_REFUSED (-1)
#define GM_SOCK_ERR_SHORT   (-2)
int  gm_socket_sendto_err(int sock, const void *buf, size_t len,
                          const struct sockaddr_in *to, int *out_err);

/* select(timeout) + recvfrom. Returns bytes received or -1 on
 * timeout/error. timeout_ms == 0 → poll non-blocking.
 * A datagram larger than cap (WSAEMSGSIZE, consumed truncated by Winsock)
 * returns cap + 1 — an invalid length that callers' strict length-based
 * dispatch rejects — instead of -1, which would wrongly mean "nothing else
 * pending". */
int  gm_socket_recvfrom_timeout(int sock, void *buf, size_t cap, int timeout_ms);

/* Same thing, but also returns the datagram's source address in *out_from —
 * needed by the input channel's provenance filter.
 * gm_socket_recvfrom_timeout is a plain call with out_from == NULL. */
int  gm_socket_recvfrom_timeout_from(int sock, void *buf, size_t cap, int timeout_ms,
                                     struct sockaddr_in *out_from);

/* Requests a send queue of `bytes` bytes (SO_SNDBUF); returns the value READ
 * BACK by getsockopt (Linux doubles and caps it), or -1 if the call fails. */
int  gm_socket_set_sndbuf(int sock, int bytes);
/* Windows only, UDP_SEND_MSG_SIZE = seg (0 = disable split-sends). Returns 0
 * if accepted AND read back equal to seg; -1 otherwise, with *out_err =
 * WSAGetLastError() read immediately (0 if the readback fails or differs —
 * in that case the option is cleared back to 0 before returning). POSIX:
 * returns -1, *out_err = 0, nothing is attempted (UDP_SEGMENT out of
 * scope). INVARIANT: once the segment is set, any send longer than seg is
 * split by the system; only gm_send_payload (gm_proto.c) sends more than
 * one MTU at a time. The handle tracks this in h->uso_seg (gm_uso_disarm,
 * gm.c) — see gm_internal.h for the cases where a failed readback leaves
 * the true socket state uncertain — and gm_set_mtu refuses an MTU larger
 * than a segment it could not clear. */
int  gm_socket_set_send_segment(int sock, unsigned seg, int *out_err);
/* Reads UDP_SEND_MSG_SIZE back into *out_seg (0 = no segment). Returns 0,
 * or -1 if the readback fails (*out_seg left untouched). POSIX: *out_seg = 0,
 * returns 0. */
int  gm_socket_get_send_segment(int sock, unsigned *out_seg);

#endif /* LIBGM_GM_SOCKET_H */
