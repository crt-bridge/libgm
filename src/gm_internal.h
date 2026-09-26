/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gm_internal.h — private struct definitions and internal function declarations
 * shared across libgm translation units.
 *
 * NOT a public header. The opaque `gm_handle` type is forward-declared in
 * gm.h; the full struct lives here so gm.c (lifecycle), gm_proto.c (sendto
 * helpers), and gm_heartbeat.c (thread helper) can all access gm_handle fields
 * without exposing Winsock2 or Win32 threading types to library users.
 */

#ifndef LIBGM_GM_INTERNAL_H
#define LIBGM_GM_INTERNAL_H

#include "gm.h"

#ifdef _WIN32
#  include <winsock2.h>   /* struct sockaddr_in */
#else
#  include <netinet/in.h> /* struct sockaddr_in */
#  include <pthread.h>    /* pthread_mutex_t / pthread_cond_t for the bounded shutdown */
#endif
#include "gm_hb_compat.h" /* gm_hb_thread, gm_hb_flag — included AFTER <winsock2.h> */

/* --- Sending past the wire's own wait (super-sends via UDP_SEND_MSG_SIZE). --- */
#define GM_SNDBUF_BYTES   (1024 * 1024)   /* 1 MiB, measured with a send probe */
#define GM_USO_SUPER_MAX  65000u          /* super-send ceiling; rounded down to a multiple of
                                           * the MTU by gm_send_payload. MTU 1472 -> 44 segments
                                           * = 64768, the size measured by that probe */

struct gm_handle {
    int                sock;
    struct sockaddr_in to;

    /* Heartbeat — implemented in gm_heartbeat.c. Opt-in API: BLIT_VSYNC traffic
     * already acts as a heartbeat during play, and no crt-bridge caller starts
     * this thread today. Still ported to POSIX anyway: a library should not have
     * a public function that fails depending on the platform. */
    gm_hb_thread       hb_thread;
    gm_hb_flag         hb_stop;
    /* hb_running replaces the original Windows code's "h->hb_thread != NULL"
     * test: pthread_t is OPAQUE and has no portable "null" value (an unsigned
     * integer under glibc, a pointer under Darwin). An explicit flag says the
     * same thing on all three platforms. calloc leaves 0 = no thread, which is
     * the right default. */
    int                hb_running;
    unsigned           hb_interval_ms;
#ifndef _WIN32
    /* Bounded shutdown, POSIX only. pthread_join has no standard bounded
     * variant; pthread_timedjoin_np is a glibc extension ABSENT from macOS;
     * and pthread_condattr_setclock is absent from macOS too. The only
     * common denominator is a mutex/condition pair with a deadline on the
     * WALL clock. See gm_heartbeat.c. */
    pthread_mutex_t    hb_done_mtx;
    pthread_cond_t     hb_done_cond;
    int                hb_done;
#endif

    /* ACK cache — populated by gm_recv_status.
     * has_status == 0 means cache is stale / never populated. */
    gm_status          last_status;
    int                has_status;

    /* --- Source address of the last status received.
     * gm_status stays fixed at 13 bytes (_Static_assert, gm.h): the address
     * cannot travel there, so it lives here and comes out through
     * gm_last_status_peer(), on the exact same model as last_status/has_status
     * above. --- */
    struct sockaddr_in last_status_from;   /* source address of the last status received */
    int                has_status_from;    /* 0 until a status has arrived           */

    /* --- LZ4 compression. Fixed at session open by gm_send_init, never
     * touched afterward; the gm_send_blit* functions only read it. This is
     * the exact counterpart of record_groovy.c's audio pattern. --- */
    uint8_t            compression;      /* 0 = raw, 1 = LZ4. Nothing else travels. */
    uint8_t           *lz4_scratch;      /* OUTPUT buffer, grows on demand */
    size_t             lz4_scratch_cap;

    /* --- Input channel. Socket DISTINCT from `sock`: different port,
     * reversed direction (the receiver sends, we consume), and independent
     * lifetime — only opened if gm_bind_inputs is called, which the
     * GROOVY_INPUT runtime guard decides. --- */
    int                input_sock;        /* -1 when the channel is not hooked up */
    struct sockaddr_in input_to;          /* h->to with the input port */
    gm_joy_inputs      last_joy;          /* last accepted joystick state */
    gm_ps2_inputs      last_ps2;          /* last accepted keyboard/mouse state */
    uint8_t            has_joy, has_ps2;  /* 0 = nothing ever received */
    uint32_t           in_rejected_len;   /* datagrams of unknown length */
    uint32_t           in_rejected_src;   /* datagrams from an address other than the receiver */
    uint32_t           in_stale;          /* datagrams older than the current state */

    /* --- Mouse-delta accumulator. The wire's mouse bytes are DELTAS per
     * datagram (the receiver resets its accumulator to zero after EVERY
     * send); "last state wins" in last_ps2 would lose the movement from
     * intermediate datagrams when several are drained within the same
     * RetroArch frame. gm_poll_inputs ADDS the decoded deltas here (sign
     * -256, GroovyMAME's formula); gm_get_mouse_deltas consumes them and
     * resets to zero. --- */
    int                mouse_acc_x, mouse_acc_y, mouse_acc_z;

    /* --- This session's send MTU. Set to GM_MTU_DEFAULT by gm_init, changed
     * afterward by gm_set_mtu ONLY. The three chunkers in gm_proto.c READ it;
     * none of them write it.
     *
     * TRAP: calloc() leaves 0, and an MTU of 0 makes the three chunkers loop
     * FOREVER (chunk = 0, so off += 0, so off < bytes forever). The explicit
     * initialization in gm_init is therefore not redundant — the exact same
     * reason as input_sock = -1 above. --- */
    unsigned           mtu;

    /* --- Padded-session mode. calloc leaves 0: a fresh handle is in a
     * CLASSIC session, which is the guarded invariant — without
     * gm_set_padding, gm_proto.c emits exactly the same bytes as before this
     * feature existed. Same reasoning as dup_enabled below, and
     * DELIBERATELY not that of mtu (which calloc would leave at a dangerous
     * value). --- */
    uint8_t            padded;

    /* --- Holding back of VERY SMALL audio blocks, padded mode only. An
     * entire block shorter than GM_TAIL_MIN bytes cannot be rebalanced by
     * gm_next_chunk: it would go out as a small datagram glued to its short
     * header, the shape that crashes the Mac's Wi-Fi firmware.
     * gm_send_audio holds it back here and glues it to the FRONT of the
     * next block: sample order is ALWAYS preserved (including when carry +
     * block exceeds the u16: two CMD_AUDIO packets, see gm_send_audio), one
     * block of delay for these samples only. The carry — at most 255 bytes,
     * 1.4 ms — is LOST in three accepted cases: gm_send_init (both open AND
     * mid-session re-announce), a mode change via gm_set_padding, and
     * session close (gm_send_close / gm_close do not flush it: emitting it
     * alone would send the tiny datagram this mode forbids). calloc leaves
     * 0: inert outside padded mode. --- */
    size_t             acarry_len;
    uint8_t            acarry[65536];

    /* --- Frame-dup, unchanged otherwise. Set to 0 by gm_init: WITHOUT
     * gm_set_frame_dup, this block is inert and the wire is bit-identical to
     * before this feature existed. That is the guarded invariant.
     *
     * dup_prev[] is indexed by FIELD PARITY: [0] serves progressive and the
     * even field, [1] the odd field. Two buffers, not one, because the 480i
     * path alternates parities from one call to the next
     * (gm_send_blit_half): comparing an even field to an odd one would
     * never come back equal, and the lever would be dead in 480i.
     *
     * Capacities NEVER shrink (same pattern as lz4_scratch): realloc into a
     * temporary variable, never overwriting the pointer on failure. No
     * malloc/free per frame. --- */
    uint8_t            dup_enabled;        /* 0 = inert (default), 1 = active */
    uint8_t           *dup_prev[2];        /* last frame ACTUALLY sent, by parity */
    size_t             dup_prev_cap[2];
    size_t             dup_prev_len[2];    /* 0 = nothing to compare -> full frame */
    uint64_t           dup_last_full_ns;   /* 0 = no full frame in this session yet */
    unsigned           dup_force_full;     /* full frames still owed (refresh cadence) */
    uint8_t            dup_pair_open;      /* 480i: a real half-frame is waiting for its twin */
    uint32_t           dup_count;          /* frame_dup sent since open, never decreases */

    /* --- Outcome of the last ANNOUNCE send on this handle. Set by
     * gm_send_init and gm_send_switchres ONLY — never by the blits, audio,
     * close, or get_status. 0 = accepted by Winsock; > 0 = Winsock code; -1 =
     * refused before Winsock; -2 = truncated send. calloc leaves 0: a fresh
     * handle has not failed anything yet. Read by gm_last_announce_error. --- */
    int                last_announce_err;

    /* --- Sending past the wire's own wait. calloc leaves 0 — same
     * reasoning as padded above: a fresh handle has uso_active = 0 (the
     * original path) until gm_init has successfully set the segment. sndbuf
     * is set even if the split-send path fails to arm — the two levers are
     * independent (the send-buffer request also applies under Linux, where
     * the split-send option does not exist). --- */
    int                sndbuf;      /* send queue read back at open; -1 if refused */
    uint8_t            uso_active;  /* 1 = super-sends split by the system */
    /* Segment set on the socket, according to the last readback; failing
     * that, the last segment requested (this is NOT always a confirmed
     * reflection of the socket: when nothing is readable, gm_init and
     * gm_uso_disarm set the INTENTION as a cautious assumption, never
     * calloc's 0). 0 = none. Only falls back to 0 if clearing the option
     * succeeded — confirmed by readback, or by a setsockopt(0) that was
     * accepted but that no later readback can confirm (gm_uso_disarm).
     * INVARIANTS: uso_active == 1 implies uso_seg == mtu; and uso_seg is
     * either 0 or at least mtu — upheld by TWO guards: gm_init refuses to
     * open (returns NULL) if the segment left after a double fault at open
     * is shorter than the MTU; gm_set_mtu refuses an MTU larger than a
     * segment it could not clear — without these two guards, the original
     * path would get re-split without any caller knowing. */
    unsigned           uso_seg;
    /* Counters, read by gm_send_offload_stats. */
    uint32_t           uso_super_sends;    /* super-sends accepted whole */
    uint32_t           uso_transient_errs; /* super-sends failed transiently, split path kept */
    int                uso_cut_err;        /* 0 = never cut; otherwise the code that cut it */
};

/* Leaves the split-send path AND clears the segment from the socket.
 * uso_active goes to 0; uso_seg goes to 0 if clearing it succeeded —
 * confirmed by readback, OR by a setsockopt(0) that was accepted but that no
 * later readback can confirm (only a genuine setsockopt refusal leaves real
 * doubt); otherwise to the value READ BACK from the socket, otherwise it
 * keeps its value (cautious assumption: the segment is still there). At
 * open, gm_init first sets uso_seg = mtu when setsockopt succeeded (even
 * before calling this function) — so the value kept on a double fault is
 * always the segment ACTUALLY requested, never calloc's 0. Defined in gm.c,
 * also called by gm_flush_run (gm_proto.c). */
void gm_uso_disarm(gm_handle *h);

/* Internal helper: send a 1-byte CMD_GET_STATUS (0x05) poll packet.
 * Called by the heartbeat thread in gm_heartbeat.c. Not part of public API.
 * Declared here so both gm_proto.c (definition) and gm_heartbeat.c (caller)
 * share the same declaration without circular includes. */
int gm_send_get_status_internal(gm_handle *h);

#endif /* LIBGM_GM_INTERNAL_H */
