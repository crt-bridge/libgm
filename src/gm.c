/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gm.c — gm_handle lifecycle (gm_init / gm_close).
 *
 * NOTE: gm_recv_status and gm_send_get_status_internal are defined in
 * gm_proto.c. gm_heartbeat_start / gm_heartbeat_stop
 * are defined in gm_heartbeat.c. This file owns only
 * the handle lifecycle: gm_init and gm_close.
 */

#include "gm.h"
#include "gm_internal.h"
#include "gm_socket.h"

#ifdef _WIN32
#  include <winsock2.h>   /* struct sockaddr_in, definition complete */
#  include <windows.h>    /* GetEnvironmentVariableA */
#else
#  include <netinet/in.h> /* struct sockaddr_in, definition complete */
#  include <stdlib.h>     /* getenv */
#endif
#include <stdlib.h>
#include <stdio.h>        /* fprintf, snprintf */
#include <string.h>

/* Reads GROOVY_USO into `val` (a `cap`-byte buffer). On Windows,
 * GetEnvironmentVariableA -- NOT getenv, whose CRT copy inside the DLL does
 * not see a variable set AFTER the DLL is loaded (tests set it from Python,
 * after the import). A return of 0 or >= cap (absent or too long) is treated
 * as absent. On POSIX, getenv, then a bounded copy via snprintf. Returns 1 if
 * the value is "off" (case-insensitive: off, OFF, Off) or "0" -- any other
 * value, or its absence, leaves the default active (the same convention as
 * every other crt-bridge lever: default on an unknown value). */
static int gm_ascii_ieq(const char *a, const char *b)
{
    /* Hand-rolled ASCII comparison: strcasecmp is POSIX, _stricmp is
     * Microsoft, and tolower depends on locale. */
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
    }
}

static int gm_uso_env_off(char *val, size_t cap)
{
    val[0] = '\0';
#ifdef _WIN32
    DWORD n = GetEnvironmentVariableA("GROOVY_USO", val, (DWORD)cap);
    if (n == 0 || n >= (DWORD)cap)
        val[0] = '\0';
#else
    const char *v = getenv("GROOVY_USO");
    if (v) snprintf(val, cap, "%s", v);
#endif
    return (gm_ascii_ieq(val, "off") || strcmp(val, "0") == 0) ? 1 : 0;
}

void gm_uso_disarm(gm_handle *h)
{
    int      err = 0;
    unsigned got = 0u;
    h->uso_active = 0u;
    if (gm_socket_set_send_segment(h->sock, 0u, &err) == 0) {
        h->uso_seg = 0u;
        return;
    }
    /* Zero-reset refused by gm_socket_set_send_segment (or we're off Windows,
     * or nothing had been applied there in the first place): try our own
     * readback next. If that also fails, uso_seg keeps its old value --
     * unless the zero-reset syscall itself is known to have succeeded (next
     * block, Windows only). */
    if (gm_socket_get_send_segment(h->sock, &got) == 0) {
        h->uso_seg = got;
        return;
    }
#ifdef _WIN32
    /* err == 0 here means the
     * setsockopt(UDP_SEND_MSG_SIZE, 0) call INSIDE gm_socket_set_send_segment
     * DID SUCCEED -- only its own confirmation readback failed or disagreed,
     * and our own readback just above has now also failed. For a reset to
     * zero, err == 0 is the best evidence available: unlike a non-zero
     * segment, a zero value has no system adjustment to correct for. If
     * err != 0, the setsockopt call itself failed and nothing is confirmed:
     * uso_seg keeps its value (the cautious assumption, the open-time
     * double-failure case). */
    if (err == 0) h->uso_seg = 0u;
#endif
}

/* Never report "-1 bytes" when SO_SNDBUF is refused. */
static const char *gm_sndbuf_text(const gm_handle *h, char *buf, size_t cap)
{
    if (h->sndbuf > 0)
        snprintf(buf, cap, "send queue %d bytes", h->sndbuf);
    else
        snprintf(buf, cap, "send queue left at the system default (SO_SNDBUF refused)");
    return buf;
}

/* Cause of the last gm_init on the calling thread (GM_INIT_* in gm.h).
 * Thread-local: there is no handle to hang it on when gm_init returns NULL,
 * and a process-wide variable would let one thread read another thread's
 * cause. gm_init sets it on EVERY exit, success included, so a stale cause
 * never outlives the next call.
 *
 * Windows uses the Win32 TLS API, not _Thread_local: MinGW compiles
 * _Thread_local to emulated TLS, which makes libgm.dll import
 * __emutls_get_address from libgcc_s_seh-1.dll -- a runtime DLL its callers
 * do not ship, so the library would fail to load. The slot is allocated on
 * first use and kept for the life of the process; a fresh slot reads NULL,
 * i.e. GM_INIT_OK, on every thread. Should Windows run out of TLS slots,
 * the cause falls back to a process-wide value rather than being lost. */
#ifdef _WIN32
static volatile LONG s_init_error_slot = (LONG)TLS_OUT_OF_INDEXES;
static volatile LONG s_init_error_fallback = GM_INIT_OK;

static DWORD gm_init_error_slot(void)
{
    LONG cur = s_init_error_slot;
    if (cur != (LONG)TLS_OUT_OF_INDEXES) return (DWORD)cur;
    DWORD mine = TlsAlloc();
    if (mine == TLS_OUT_OF_INDEXES) return TLS_OUT_OF_INDEXES;
    LONG prev = InterlockedCompareExchange(&s_init_error_slot, (LONG)mine,
                                           (LONG)TLS_OUT_OF_INDEXES);
    if (prev != (LONG)TLS_OUT_OF_INDEXES) {   /* another thread won the race */
        TlsFree(mine);
        return (DWORD)prev;
    }
    return mine;
}

static void gm_init_error_set(int code)
{
    DWORD slot = gm_init_error_slot();
    if (slot != TLS_OUT_OF_INDEXES)
        TlsSetValue(slot, (LPVOID)(intptr_t)code);
    else
        InterlockedExchange(&s_init_error_fallback, (LONG)code);
}

int gm_last_init_error(void)
{
    /* TlsGetValue resets the thread's last-error code: keep the caller's
     * (WSAGetLastError reads the same value) intact across this call. */
    DWORD saved = GetLastError();
    DWORD slot  = gm_init_error_slot();
    int   code;
    if (slot == TLS_OUT_OF_INDEXES)
        code = (int)s_init_error_fallback;
    else
        code = (int)(intptr_t)TlsGetValue(slot);
    SetLastError(saved);
    return code;
}
#else
static _Thread_local int s_init_error = GM_INIT_OK;

static void gm_init_error_set(int code)
{
    s_init_error = code;
}

int gm_last_init_error(void)
{
    return s_init_error;
}
#endif

const char *gm_init_error_str(int code)
{
    switch (code) {
    case GM_INIT_OK:               return "no error";
    case GM_INIT_ERR_NO_ADDRESS:   return "receiver address missing";
    case GM_INIT_ERR_WINSOCK:      return "Winsock initialization failed";
    case GM_INIT_ERR_ALLOC:        return "handle allocation failed";
    case GM_INIT_ERR_SOCKET:       return "socket could not be opened";
    case GM_INIT_ERR_RESOLVE:      return "receiver address failed to resolve";
    case GM_INIT_ERR_SEND_SEGMENT: return "socket keeps a send segment shorter than the MTU";
    default:                       return "unknown error";
    }
}

gm_handle *gm_init(const char *receiver_ip, uint16_t port)
{
    if (!receiver_ip) {
        gm_init_error_set(GM_INIT_ERR_NO_ADDRESS);
        return NULL;
    }
    if (gm_socket_init_winsock() != 0) {
        gm_init_error_set(GM_INIT_ERR_WINSOCK);
        return NULL;
    }

    gm_handle *h = (gm_handle *)calloc(1, sizeof(*h));
    if (!h) {
        gm_init_error_set(GM_INIT_ERR_ALLOC);
        return NULL;
    }

    /* gm_socket_open: -2 = the name did not resolve, -1 = anything else
     * (socket creation failed). */
    int open_rc = gm_socket_open(receiver_ip, port, &h->sock, &h->to);
    if (open_rc != 0) {
        if (open_rc == -2)
            gm_init_error_set(GM_INIT_ERR_RESOLVE);
        else
            gm_init_error_set(GM_INIT_ERR_SOCKET);
        free(h);
        return NULL;
    }

    /* hb_thread is NULL after calloc; heartbeat starts only if caller opts in
     * via gm_heartbeat_start (caller-owned threading). */

    /* Input channel: -1 means "not bound". 0 is a valid descriptor, so
     * calloc() (which leaves 0) is not enough here. */
    h->input_sock = -1;

    /* See the calloc PITFALL above this field in gm_internal.h: 0 would make
     * the splitters loop forever. */
    h->mtu = GM_MTU_DEFAULT;

    /* Off-the-wire send: every handle opened by gm_init gets the same
     * treatment, master and followers alike. A SO_SNDBUF failure does not
     * block opening -- silent fallback; the send queue then stays at the
     * system default, but nothing else changes. */
    h->sndbuf = gm_socket_set_sndbuf(h->sock, GM_SNDBUF_BYTES);
    char uso_val[16] = "";
    char sb[96];
    int  uso_err = 0;
    gm_sndbuf_text(h, sb, sizeof sb);
    if (gm_uso_env_off(uso_val, sizeof uso_val)) {
        fprintf(stderr, "[gm-send] %s ; split send inactive (GROOVY_USO=%s)\n",
                sb, uso_val);
    } else if (gm_socket_set_send_segment(h->sock, h->mtu, &uso_err) == 0) {
        h->uso_active = 1u;
        h->uso_seg    = h->mtu;
        fprintf(stderr, "[gm-send] %s ; split send active, segment %u\n",
                sb, h->uso_seg);
    } else {
        /* uso_err == 0 on this path means
         * setsockopt DID SUCCEED and only the readback (inside
         * gm_socket_set_send_segment) failed -- so the cautious assumption
         * becomes "the requested segment is indeed there", never calloc's 0.
         * gm_uso_disarm, below, can then CONFIRM it (successful reset -> 0),
         * CORRECT it (successful readback -> the real value), or KEEP it as
         * is (both the reset and the readback fail): in all three cases the
         * starting value is correct, never again a 0 that would misreport a
         * segment that is actually applied. Guarded under _WIN32: off
         * Windows uso_err is also 0, but nothing was applied there
         * (gm_socket_set_send_segment never attempts setsockopt there) --
         * setting uso_seg here would therefore be wrong off Windows, even
         * though gm_uso_disarm would read back 0 there anyway. */
#ifdef _WIN32
        if (uso_err == 0) h->uso_seg = h->mtu;
#endif
        gm_uso_disarm(h);
#ifdef _WIN32
        if (uso_err != 0)
            fprintf(stderr, "[gm-send] %s ; split send inactive "
                            "(refused by the system, code %d), original path\n",
                    sb, uso_err);
        else
            fprintf(stderr, "[gm-send] %s ; split send inactive (option accepted but "
                            "read back different or unreadable), original path, segment left %u\n",
                    sb, h->uso_seg);
#else
        fprintf(stderr, "[gm-send] %s ; split send inactive "
                        "(not attempted off Windows), original path\n",
                sb);
#endif
        /* If the segment left on the socket per uso_seg -- see
         * gm_internal.h for what that value can and cannot confirm -- is
         * shorter than the MTU, every send on the original path would be
         * re-split starting with the very first frame, with no later guard
         * catching it (gm_set_mtu only sees MTU CHANGES). A handle already
         * re-split like this must not leave gm_init. Nothing else is
         * allocated yet at this stage -- no heartbeat thread, input_sock =
         * -1, lz4_scratch and dup_prev NULL -- closing the socket and
         * freeing the handle is enough. */
        if (h->uso_seg != 0u && h->uso_seg < h->mtu) {
            fprintf(stderr, "[gm-send] open refused: the socket keeps a segment of %u "
                            "bytes, shorter than the MTU %u ; every send would be re-split\n",
                    h->uso_seg, h->mtu);
            gm_socket_close(h->sock);
            free(h);
            gm_init_error_set(GM_INIT_ERR_SEND_SEGMENT);
            return NULL;
        }
    }

    /* Padded session mode: NOTHING to set here, same reasoning as the frame
     * dup block below -- calloc() already gives the right default (0 =
     * classic session), and that is the safeguard. */

    /* Frame dup, unchanged: NOTHING to set here, unlike mtu above. calloc()
     * already gives exactly the right defaults -- dup_enabled = 0 (inert),
     * dup_prev[] = NULL, dup_prev_len[] = 0, dup_last_full_ns = 0,
     * dup_force_full = 0, dup_pair_open = 0, dup_count = 0. That is the
     * safeguard: without gm_set_frame_dup, this block never moves, and the
     * wire is bit-identical to before this feature. This comment exists so
     * that a future reader, used to the neighboring mtu field which MUST be
     * set explicitly, does not assume a matching oversight here. */

    gm_init_error_set(GM_INIT_OK);
    return h;
}

int gm_set_mtu(gm_handle *h, unsigned mtu)
{
    if (!h) return -1;
    if (mtu < GM_MTU_MIN || mtu > GM_MTU_MAX)
        return -1;               /* refused: the current value stays in place */

    /* Off-the-wire send: the decision follows the segment held on the
     * socket per uso_seg -- see gm_internal.h for what that value can and
     * cannot confirm -- not uso_active, since a segment can stay applied
     * after a disconnect. Active path: re-apply the segment at the new MTU.
     * Otherwise, or if the system refuses: clear the segment (gm_uso_disarm).
     * If a segment shorter than the new MTU is left, REFUSE the change: the
     * system would re-split every send on the original path, and the wire
     * would change. */
    if (h->uso_seg != 0u && h->uso_seg != mtu) {
        int err = 0;
        if (h->uso_active && gm_socket_set_send_segment(h->sock, mtu, &err) == 0) {
            h->uso_seg = mtu;
            fprintf(stderr, "[gm-send] segment brought back to %u (session MTU)\n", mtu);
        } else {
            const int was_active = h->uso_active;
            gm_uso_disarm(h);
            if (was_active)
                fprintf(stderr, "[gm-send] split send cut off: segment %u refused (code %d), original path\n",
                        mtu, err);
            if (h->uso_seg != 0u && h->uso_seg < mtu) {
                fprintf(stderr, "[gm-send] MTU %u refused: the socket keeps a segment of %u bytes "
                                "that would re-split it ; MTU stays %u\n",
                        mtu, h->uso_seg, h->mtu);
                return -1;
            }
        }
    }
    h->mtu = mtu;
    return 0;
}

/* gm_set_frame_dup -- on the exact model of gm_set_mtu: a setter, no
 * validation beyond h != NULL (a boolean normalized to 0/1, any nonzero
 * value means "active"). */
int gm_set_frame_dup(gm_handle *h, int on)
{
    if (!h) return -1;
    h->dup_enabled = on ? 1u : 0u;
    return 0;
}

/* gm_set_padding -- on the EXACT model of gm_set_frame_dup: a setter, no
 * validation beyond h != NULL, boolean normalized to 0/1 (any nonzero value
 * means "active"). Padded session mode: it is gm_send_init, not this setter,
 * that negotiates the mode with the peer, through the length of the
 * CMD_INIT sent afterward. */
int gm_set_padding(gm_handle *h, int on)
{
    if (!h) return -1;
    const uint8_t new_mode = on ? 1u : 0u;
    /* An audio carry (h->acarry) only makes sense within the padded session
     * that produced it. Changing mode clears it, otherwise a 1 -> 0 -> 1
     * toggle without gm_send_init would stick stale audio at the head of a
     * later block. */
    if (h->padded != new_mode)
        h->acarry_len = 0;
    h->padded = new_mode;
    return 0;
}

/* gm_send_offload_state — returns 1 if blit payload goes out as system-split
 * super-sends, 0 otherwise (GROOVY_USO=off/0, refused by the system, cut off
 * mid-session, off Windows), -1 if h is NULL. uso_active implies
 * uso_seg == mtu (invariant from gm_internal.h, upheld by gm_init and
 * gm_set_mtu): testing it again would be a dead guard. *out_sndbuf = the send
 * queue read back at open time (0 if gm_socket_set_sndbuf failed). */
int gm_send_offload_state(const gm_handle *h, unsigned *out_sndbuf)
{
    if (!h) return -1;
    if (out_sndbuf) *out_sndbuf = h->sndbuf > 0 ? (unsigned)h->sndbuf : 0u;
    return h->uso_active ? 1 : 0;
}

/* gm_send_offload_stats — see gm.h. */
int gm_send_offload_stats(const gm_handle *h, unsigned *out_seg, uint32_t *out_super_sends,
                          uint32_t *out_transient_errs, int *out_cut_err)
{
    if (!h) return -1;
    if (out_seg)            *out_seg            = h->uso_seg;
    if (out_super_sends)    *out_super_sends    = h->uso_super_sends;
    if (out_transient_errs) *out_transient_errs = h->uso_transient_errs;
    if (out_cut_err)        *out_cut_err        = h->uso_cut_err;
    return h->uso_active ? 1 : 0;
}

void gm_close(gm_handle *h)
{
    if (!h) return;
    /* Stop heartbeat thread if running (gm_heartbeat.c).
     * hb_running (gm_internal.h), not h->hb_thread: pthread_t is opaque and
     * has no portable null value, so h->hb_thread stays nonzero after a
     * first stop on POSIX. gm_heartbeat_stop is a no-op anyway that returns
     * -1 as soon as hb_running has dropped back to 0. */
    if (h->hb_running) {
        gm_heartbeat_stop(h);
    }
    /* Input channel: close it BEFORE the video socket, the same order as
     * the rest of this destructor (stop what runs in the background first,
     * free afterward). gm_close_inputs is a silent no-op if the channel was
     * never bound (input_sock == -1). */
    gm_close_inputs(h);
    gm_socket_close(h->sock);
    free(h->lz4_scratch);
    free(h->dup_prev[0]);
    free(h->dup_prev[1]);
    free(h);
}
