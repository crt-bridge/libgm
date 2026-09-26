/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gm_proto.c — wire serialization for libgm.
 *
 * Command senders for CMD_INIT, CMD_SWITCHRES,
 * CMD_BLIT_VSYNC, and CMD_CLOSE. Wire output MUST byte-match GroovyMAME
 * 0.287's gridlee captures (kept by the emitter's test suite).
 *
 * Discipline:
 *   - All sends use sizeof(packed wire struct) — gated by _Static_assert in
 *     gm.h. If MinGW ever grows the struct, the build fails before the
 *     CRT can be confused.
 *   - pclock is offset 1 inside cmd_switchres — naturally unaligned. Always
 *     memcpy, never assign through a pointer cast (mirror of the receiver
 *     parser's own discipline).
 *   - CMD_BLIT_VSYNC header is sent ALONE on the wire; payload bytes follow
 *     in subsequent UDP datagrams with NO opcode prefix. The receiver's
 *     continuation guard reassembles them.
 *
 * Padded-session mode (full table in gm.h): gm_send_switchres,
 * gm_emit_blit_0x06 (via gm_send_blit/gm_send_blit_half), and
 * gm_send_blit_field stay UNTOUCHED in this file — their wire forms (26,
 * 11, 8/12 bytes) already sit above the measured survival threshold, so
 * padding does not apply to them. Only gm_send_init, gm_send_audio,
 * gm_send_close, and gm_send_get_status_internal change shape depending on
 * h->padded.
 *
 * Amended 2026-09-19: this still only covers the HEADERS. The PAYLOAD
 * itself, video and audio, goes through padded mode via gm_next_chunk:
 * never a payload TAIL shorter than GM_TAIL_MIN (256) bytes; and an ENTIRE
 * audio block under that threshold is held back and glued to the front of
 * the next one (gm_send_audio, h->acarry). One accepted exception: an
 * entire compressed FRAME under 256 bytes ships as-is (never observed in
 * practice). Classic mode: the wire stays bit-identical.
 *
 * POSIX port: Winsock and POSIX coexist in the same files, behind inline
 * #ifdef blocks; the public gm.h API has not moved a line.
 */

#include "gm.h"
#include "gm_internal.h"
#include "gm_socket.h"
#include "lz4.h"

#ifdef _WIN32
#  include <winsock2.h>
#  include <windows.h>
#else
#  include <netinet/in.h>   /* struct sockaddr_in, htons/ntohs */
#  include <arpa/inet.h>
#  include <time.h>         /* clock_gettime, CLOCK_MONOTONIC */
#  include <errno.h>        /* EMSGSIZE... (see gm_uso_refusal below) */
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>          /* fprintf ([gm-send] logging for a refused super-send) */
#include <stdlib.h>
#include <limits.h>
#include <string.h>

/* The public (gm.h) and internal (gm_socket.h) constants for an announce
 * outcome must stay the same numbers — gm_last_announce_error just relays
 * h->last_announce_err, set from gm_socket_sendto_err. */
_Static_assert(GM_SEND_ERR_REFUSED == GM_SOCK_ERR_REFUSED, "public and internal constants diverge");
_Static_assert(GM_SEND_ERR_SHORT == GM_SOCK_ERR_SHORT, "public and internal constants diverge");

/* ---------------------------------------------------------------------------
 * gm_next_chunk — size of the next payload chunk.
 *
 * Classic mode (h->padded == 0): the original split, byte for byte — the
 * session's MTU, then the remainder. Wire stays BIT-IDENTICAL.
 *
 * Padded-session mode (added 2026-09-19): never a SHORT tail. Measured on the
 * bench, off the bridge: a payload datagram of 4 or 8 bytes, followed by the
 * short header of the next block, at 73 Mbit/s receive rate, crashes the
 * Mac's Wi-Fi firmware (RX-ASSERT cdma_VerifyActualCompletedMPDUC; 21 and 14
 * driver drops in 120 s); 64, 256, and 736 bytes hold (0 drops). Chrono Cross
 * delivers audio blocks of 2948 bytes = 1472 + 1472 + 4: 4849 four-byte tails
 * out of 7055 blocks. When the remainder after a full chunk would be shorter
 * than GM_TAIL_MIN, the remainder is split into two halves instead: 1472 +
 * 1472 + 4 becomes 1472 + 738 + 738. The block's byte count never changes;
 * the client and receiver reassemble on byte count, whatever the chunk sizes.
 *
 * Not covered here: an ENTIRE block shorter than GM_TAIL_MIN — there is
 * nothing to rebalance. For AUDIO, gm_send_audio holds it back (h->acarry).
 * For VIDEO, it ships as-is: ACCEPTED LIMIT, an entire compressed frame under
 * 256 bytes has never been observed (LZ4 tops out around 255:1; a black
 * 160x144x3 frame comes out to roughly 283 bytes).
 *
 * Both halves are always at least GM_TAIL_MIN because a session's MTU never
 * drops below GM_MTU_MIN (548): compile-time guard below.
 * ------------------------------------------------------------------------- */
#define GM_TAIL_MIN 256u
_Static_assert(GM_MTU_MIN >= 2u * GM_TAIL_MIN,
               "gm_next_chunk: one half of (mtu, mtu + GM_TAIL_MIN) must be at least GM_TAIL_MIN");
_Static_assert(GM_TAIL_MIN % 4u == 0u,
               "gm_send_audio cuts GM_TAIL_MIN bytes off the end: the cut must land between two sample pairs");

static int gm_emit_audio_block(gm_handle *h, const uint8_t *p, size_t bytes);

static size_t gm_next_chunk(const gm_handle *h, size_t remaining)
{
    const size_t mtu = (size_t)h->mtu;   /* session's MTU; never 0 */
    if (remaining <= mtu)
        return remaining;
    if (h->padded && remaining - mtu < GM_TAIL_MIN)
        return (remaining + 1u) / 2u;
    return mtu;
}

/* ---------------------------------------------------------------------------
 * gm_flush_run — sends a GROUP of chunks already accumulated by
 * gm_send_payload as a single system call.
 *
 * `len <= mtu`: a single chunk, ordinary `gm_socket_sendto` — no need to go
 * through the USO path for that. `len > mtu`: the super-send itself. The
 * system splits it into datagrams of `h->uso_seg` bytes (== h->mtu,
 * guaranteed by the caller); that is exactly what the original loop produced,
 * one `sendto` at a time.
 *
 * Failure: returns -1 in every case, same as the original loop on a failed
 * sendto. But only a refusal that PROVES the option is not held
 * (gm_uso_refusal) disables the split path for the rest of the handle's
 * lifetime, and clears the segment from the socket (gm_uso_disarm). A
 * transient error — WSAENOBUFS, WSAEWOULDBLOCK, a lost route, a receiver
 * unreachable for a second — would have failed the old loop the same way: it
 * is counted, logged once per handle (no per-frame logging), and the next
 * frame goes out through the split path again.
 * ------------------------------------------------------------------------- */

/* Codes that prove the system does not honor UDP_SEND_MSG_SIZE:
 *   WSAEMSGSIZE     the super-send was treated as ONE oversized datagram —
 *                   the option was never applied;
 *   WSAEINVAL       invalid argument for this socket with this option;
 *   WSAEOPNOTSUPP, WSAENOPROTOOPT  option or operation not supported;
 *   GM_SOCK_ERR_SHORT  the system took fewer bytes than the super-send: what
 *                   actually went out is unknown, the path is no longer
 *                   trustworthy.
 * Everything else is treated as transient. Under POSIX, the errno
 * equivalents — the split path is never active there, the list exists only
 * for symmetry. */
static int gm_uso_refusal(int err)
{
    if (err == GM_SOCK_ERR_SHORT) return 1;
#ifdef _WIN32
    return err == WSAEMSGSIZE || err == WSAEINVAL || err == WSAEOPNOTSUPP || err == WSAENOPROTOOPT;
#else
    return err == EMSGSIZE || err == EINVAL || err == EOPNOTSUPP || err == ENOPROTOOPT;
#endif
}

static int gm_flush_run(gm_handle *h, const uint8_t *p, size_t len)
{
    const size_t mtu = (size_t)h->mtu;
    if (len <= mtu)
        return gm_socket_sendto(h->sock, p, len, &h->to) == (int)len ? 0 : -1;

    int err = 0;
    int n = gm_socket_sendto_err(h->sock, p, len, &h->to, &err);
    if (n == (int)len) {
        h->uso_super_sends++;
        return 0;
    }

    if (gm_uso_refusal(err)) {
        h->uso_cut_err = err;
        gm_uso_disarm(h);
        fprintf(stderr,
               "[gm-send] %zu-byte super-send refused (code %d): split-send path disabled for "
               "this session, segment left on socket %u\n",
               len, err, h->uso_seg);
    } else {
        if (h->uso_transient_errs++ == 0u)
            fprintf(stderr,
                   "[gm-send] %zu-byte super-send failed transiently (code %d): frame lost, "
                   "split-send path kept (logged once per handle)\n",
                   len, err);
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * gm_send_payload — chunker shared by both blit paths. Replaces the
 * `while (off < bytes)` loop of gm_emit_blit_0x06 and gm_send_blit_field;
 * does NOT touch gm_emit_audio_block (this only targets a blit's payload).
 *
 * Split path INACTIVE (GROOVY_USO=off/0, system refusal, cut mid-session, or
 * off Windows): the original loop, unchanged down to the byte. uso_active
 * implies uso_seg == mtu (invariant in gm_internal.h, upheld by gm_init and
 * gm_set_mtu): also testing uso_seg here would be a dead guard.
 *
 * Split path ACTIVE: the chunks from gm_next_chunk are GROUPED into
 * super-sends of at most `cap` bytes, sent by gm_flush_run. Why this is
 * BIT-IDENTICAL to the original path: a send of k*mtu + r bytes (r < mtu) is
 * split by the system (UDP_SEND_MSG_SIZE = mtu) into k datagrams of mtu plus
 * one of r — exactly what the loop produced, k calls of mtu then one of r.
 * The rule "a chunk joins the group if it is FULL, or if it is the LAST one
 * and a group is already open" guarantees that a group never takes any shape
 * other than k full chunks plus, at most, one final tail — and that a SHORT
 * tail (padded session) never joins and never opens a group: it goes out
 * alone, through an ordinary `gm_socket_sendto`, after the super-send that
 * precedes it.
 *
 * Verified by hand, three cases (mtu = 1472 throughout):
 *   - 413000 bytes, classic: 413000 = 280*1472 + 840. 280 full chunks grouped
 *     into batches of 44 (cap = (65000/1472)*1472 = 64768 = 44*1472) — 6
 *     groups of 44 full chunks (64768 bytes each) then a seventh group of 40
 *     full chunks + the 840-byte tail (it JOINS: it is the last chunk and a
 *     group is already open).
 *   - 1472*3 + 100 = 4516 bytes, padded: gm_next_chunk returns
 *     [1472, 1472, 786, 786] (the 100-byte tail gets rebalanced). The first
 *     two (full) open a group; is the first 786 the LAST one? No — one chunk
 *     remains after it, so `last` is false for it: it does not join (neither
 *     full nor last), it CUTS the group and goes out alone; the second 786
 *     is the last one, but run_len has just fallen back to 0 — a short tail
 *     cannot open a group on its own — it also goes out alone. Three system
 *     sends: [1472+1472 grouped into one 2944-byte super-send], [786 alone],
 *     [786 alone].
 *   - 100 bytes: a single chunk, neither full nor followed by another —
 *     `last` is true but run_len is 0, the guard "a short tail never starts
 *     a group" blocks it from joining: one ordinary `gm_socket_sendto`, a
 *     single system send.
 * ------------------------------------------------------------------------- */
static int gm_send_payload(gm_handle *h, const uint8_t *payload, size_t bytes)
{
    const size_t mtu = (size_t)h->mtu;
    size_t       off = 0;

    if (!h->uso_active) {
        while (off < bytes) {                                   /* original path, unchanged */
            size_t chunk = gm_next_chunk(h, bytes - off);
            if (gm_socket_sendto(h->sock, payload + off, chunk, &h->to) != (int)chunk)
                return -1;
            off += chunk;
        }
        return 0;
    }

    const size_t cap = ((size_t)GM_USO_SUPER_MAX / mtu) * mtu;  /* >= mtu: GM_MTU_MAX < GM_USO_SUPER_MAX */
    size_t run_off = 0, run_len = 0;
    while (off < bytes) {
        size_t chunk = gm_next_chunk(h, bytes - off);
        int    last  = (off + chunk == bytes);
        int    joins = (chunk == mtu) || (last && run_len > 0u);
        if (run_len > 0u && (!joins || run_len + chunk > cap)) {
            if (gm_flush_run(h, payload + run_off, run_len) != 0) return -1;
            run_len = 0u;
        }
        if (joins && !(chunk < mtu && run_len == 0u)) {         /* a short tail never starts a group */
            if (run_len == 0u) run_off = off;
            run_len += chunk;
        } else {
            if (gm_socket_sendto(h->sock, payload + off, chunk, &h->to) != (int)chunk)
                return -1;
        }
        off += chunk;
    }
    if (run_len > 0u && gm_flush_run(h, payload + run_off, run_len) != 0) return -1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * gm_mono_ns — monotonic Win32 clock, nanosecond resolution.
 *
 * DELIBERATE copy of the body of groovy_pacing_mono_ns (groovy_pacing.h, in
 * the RetroArch driver), not a #include of that file. groovy_pacing.h lives
 * in the RetroArch layer, above libgm, requires gm.h to
 * be included BEFORE it (#error otherwise), and is not in libgm's Makefile
 * -I flags (-Iinclude -Isrc). Including it here would reverse the dependency
 * between the low layer (libgm) and the high layer (record_groovy): libgm
 * must be able to build and test on its own, without RetroArch. The constant
 * and the semantics (the same Win32 time call, ns since an arbitrary but
 * monotonic epoch) stay identical on both sides — only the file the source
 * lives in differs.
 * ------------------------------------------------------------------------- */
static uint64_t gm_mono_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq = { { 0, 0 } };
    LARGE_INTEGER counter;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    /* Multiply by 1e9 (in double) first, then cast — avoids an integer
     * overflow for counter values approaching LLONG_MAX at high QPC
     * frequencies (same guard as groovy_pacing.h). */
    return (uint64_t)((double)counter.QuadPart * 1.0e9
                      / (double)freq.QuadPart);
#else
    /* LITERAL transposition of the software receiver's monotonic clock, in
     * production on macOS since 2026-09-05. CLOCK_MONOTONIC, never its "_RAW"
     * variant (Linux-only): the latter would break macOS portability, with
     * no benefit over intervals of a few seconds. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* ---------------------------------------------------------------------------
 * Invalidates the memory of the previous frame. CAPACITIES are KEPT (never a
 * free here); only the lengths fall back to 0, which means "nothing to
 * compare against", hence a full frame on the next send (rule 2 of
 * gm_dup_should_skip).
 *
 * Called at BOTH invalidation sites, never just one: session open (function
 * below) and a change of geometry OR mode (function after that).
 * record_groovy.c calls the latter from two distinct places (mode change,
 * dimension change within the same mode); invalidating at only one of the
 * two would let a dimension change without a mode change compare a frame
 * against a buffer of the WRONG size. Called BEFORE the send, so state stays
 * consistent even if the send fails.
 * ------------------------------------------------------------------------- */
static void gm_dup_reset(gm_handle *h)
{
    h->dup_prev_len[0] = 0u;
    h->dup_prev_len[1] = 0u;
    h->dup_last_full_ns = 0u;
    h->dup_force_full   = 0u;
    h->dup_pair_open    = 0u;
}

/* ---------------------------------------------------------------------------
 * gm_send_init — CMD_INIT (0x02), 4 bytes on the wire.
 *
 * Wire layout (GroovyMAME 0.287, verified on hardware = `02 00 03 02`):
 *   byte 0: cmd         = 0x02
 *   byte 1: compression  (0 = raw)
 *   byte 2: sample_rate  (audio sample-rate code; 0x03 in that capture)
 *   byte 3: channels     (audio channels; 0x02 = stereo in that capture)
 * ------------------------------------------------------------------------- */
int gm_send_init(gm_handle *h, const gm_init_params *p)
{
    if (!h || !p) {
        if (h) h->last_announce_err = GM_SEND_ERR_REFUSED;
        return -1;
    }

    /* Invalidates the frame-dup memory (one of the TWO sites — see the
     * comment above the function, further up this file). The first frame
     * of a session is always full. */
    gm_dup_reset(h);
    /* A fresh session does not replay audio held back from the previous
     * one. This call also covers a RE-ANNOUNCE in mid-session
     * (record_groovy.c, groovy_reannounce_emit): any carry pending at this
     * instant is LOST — at most 255 bytes, 1.4 ms of audio. Accepted:
     * replaying it after a re-announce would glue audio from before the cut
     * to audio from after it. */
    h->acarry_len = 0;

    gm_cmd_init_t pkt;
    pkt.cmd         = CMD_INIT;
    pkt.compression = p->compression;
    pkt.sample_rate = p->sample_rate;
    pkt.channels    = p->channels;

    /* Padded-session mode. The buffer is ALWAYS GM_PAD_LEN (8) bytes, zeroed
     * BEFORE being filled — the tail bytes in padded mode must be ZEROS,
     * never stack garbage. sizeof(pkt) (4, _Static_assert'd in gm.h) bytes
     * of CMD_INIT are copied into it; the length SENT follows h->padded,
     * NEVER sizeof(buffer) — exactly the trap this file already documents
     * for the 0x06 header (gm_emit_blit_0x06, below). It is this CMD_INIT,
     * by ITS length, that negotiates the mode with the peer: 8 bytes open a
     * padded session, 4 a classic one — unchanged, guarded invariant. */
    uint8_t buf[GM_PAD_LEN];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &pkt, sizeof(pkt));
    size_t len = h->padded ? (size_t)GM_PAD_LEN : sizeof(pkt);

    int n = gm_socket_sendto_err(h->sock, buf, len, &h->to, &h->last_announce_err);
    if (n != (int)len) return -1;

    h->compression = p->compression;   /* kept for subsequent sends */
    return 0;
}

/* ---------------------------------------------------------------------------
 * gm_send_switchres — CMD_SWITCHRES (0x03), 26 bytes on the wire.
 *
 * Wire layout: cmd(1) | pclock(8 LE double, MHz) | h_active/h_begin/h_end/
 *              h_total (u16 LE x4) | v_active/v_begin/v_end/v_total (u16 LE
 *              x4) | interlace(1) = 26 bytes.
 *
 * pclock is in MHz (NOT Hz). GroovyMAME 0.287 wire = double in MHz; the
 * receiver multiplies by 1.0e6. libgm matches GroovyMAME byte-for-byte.
 *
 * pclock sits at offset 1, naturally unaligned. memcpy is mandatory — never
 * assign through &pkt.pclock as a typed pointer (UB on strict-alignment
 * ABIs). The receiver's parser does the same.
 * ------------------------------------------------------------------------- */
int gm_send_switchres(gm_handle *h, const gm_modeline *m)
{
    if (!h || !m) {
        if (h) h->last_announce_err = GM_SEND_ERR_REFUSED;
        return -1;
    }

    /* Invalidates the frame-dup memory (the OTHER of the TWO sites — see the
     * comment above gm_send_init). record_groovy.c calls this function from
     * TWO places (mode change, dimension change within the same mode): both
     * must invalidate, or a frame gets compared against a buffer of the
     * wrong size. This call forces a FULL frame after every CMD_SWITCHRES,
     * which cannot create an extra missing frame — so it does not make a
     * known switch-time frame loss (240p<->480i under compression) any
     * worse, nor does it fix it: the switch path itself is untouched. */
    gm_dup_reset(h);

    gm_cmd_switchres_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.cmd = CMD_SWITCHRES;

    /* Unaligned-double discipline: pclock is at offset 1 in the packed
     * struct. memcpy avoids any compiler-inferred alignment promise. */
    memcpy(&pkt.pclock, &m->pclock_mhz, sizeof(double));

    pkt.h_active  = m->h_active;
    pkt.h_begin   = m->h_begin;
    pkt.h_end     = m->h_end;
    pkt.h_total   = m->h_total;
    pkt.v_active  = m->v_active;
    pkt.v_begin   = m->v_begin;
    pkt.v_end     = m->v_end;
    pkt.v_total   = m->v_total;
    pkt.interlace = m->interlace;

    /* sizeof(pkt) is _Static_assert'd to 26 in gm.h. */
    int n = gm_socket_sendto_err(h->sock, &pkt, sizeof(pkt), &h->to, &h->last_announce_err);
    return n == (int)sizeof(pkt) ? 0 : -1;
}

/* ---------------------------------------------------------------------------
 * gm_lz4_compress — compresses `bytes` bytes from `src` into the handle's
 * working buffer.
 *
 * Returns a pointer to the compressed block and sets *out_size, or NULL on
 * failure (nothing was sent, the caller must not send anything).
 *
 * The output buffer is sized by LZ4_compressBound() and NEVER by the raw
 * size. The upstream emitter (groovy.cpp) instead calls
 * LZ4_compress_default(src, dst, RGBSize, RGBSize): dstCapacity == raw size,
 * which legitimately fails on input that does not compress well, and it
 * handles that failure with fallbacks (adaptive, LZ4HC, delta) that are out
 * of scope here. Sizing to the bound makes failure nearly impossible at our
 * frame sizes (max 663,552 bytes, well under LZ4_MAX_INPUT_SIZE).
 *
 * The buffer grows once and lives until gm_close — the swap_buf pattern from
 * record_groovy.c. Deliberately NOT the fld0/fld1 pattern (malloc/free per
 * frame): that one is an accepted trade-off for a split that only runs in
 * 480i, whereas compression runs on EVERY frame, 240p included.
 * ------------------------------------------------------------------------- */
static const uint8_t *gm_lz4_compress(gm_handle *h, const uint8_t *src,
                                      size_t bytes, uint32_t *out_size)
{
    if (!h || !src || bytes == 0u || bytes > (size_t)INT_MAX) return NULL;

    int bound = LZ4_compressBound((int)bytes);
    if (bound <= 0) return NULL;

    if (h->lz4_scratch_cap < (size_t)bound) {
        /* realloc's result is checked in a temporary variable before being
         * assigned to the handle: on failure, the old buffer stays in place
         * and is never overwritten by NULL. */
        uint8_t *grown = (uint8_t *)realloc(h->lz4_scratch, (size_t)bound);
        if (!grown) return NULL;
        h->lz4_scratch     = grown;
        h->lz4_scratch_cap = (size_t)bound;
    }

    int written = LZ4_compress_default((const char *)src,
                                       (char *)h->lz4_scratch,
                                       (int)bytes, bound);
    if (written <= 0) return NULL;

    *out_size = (uint32_t)written;
    return h->lz4_scratch;
}

/* ---------------------------------------------------------------------------
 * gm_emit_frame_dup — CMD_BLIT_FIELD_VSYNC (0x07), NINE bytes, [8] = 0x01,
 * NO payload. This is the form the upstream library emits and that
 * the receiver's parser has accepted since frame-dup support was added, in both
 * session states. frame_id is the one from the call: the sequence stays
 * continuous, the ack regulator gets one ack per frame, there is NO silence.
 * ------------------------------------------------------------------------- */
static int gm_emit_frame_dup(gm_handle *h, uint32_t frame_id, uint16_t vsync,
                             uint8_t field)
{
    uint8_t hdr[9];
    hdr[0] = CMD_BLIT_FIELD_VSYNC;
    memcpy(&hdr[1], &frame_id, sizeof(uint32_t));
    hdr[5] = field;
    memcpy(&hdr[6], &vsync, sizeof(uint16_t));
    hdr[8] = 0x01u;
    if (gm_socket_sendto(h->sock, hdr, sizeof(hdr), &h->to) != (int)sizeof(hdr))
        return -1;
    h->dup_count++;
    return 0;
}

/* ---------------------------------------------------------------------------
 * gm_dup_should_skip — decides whether the current frame should go out as a
 * frame_dup (1) or in full (0). `half` is 1 for the 480i half-frame path
 * (pairing rule). Does NOT modify the bookkeeping state (dup_prev*,
 * dup_pair_open): gm_dup_after_real() does that, after a successful send.
 *
 * The order of rules 3 and 4 is REVERSED relative to their conceptual
 * numbering — a deliberate deviation. Checking `dup_force_full` BEFORE the
 * deadline avoids a call that arrives while a 480i pair is still open (the
 * twin field has not yet closed the pair, so dup_last_full_ns has not been
 * refreshed yet) from re-arming the counter to 2 a second time: without this
 * guard, both the even field AND the odd field would each refresh
 * `dup_force_full` to 2 instead of letting it count down from 2 to 0 over
 * the two sends, and an EXTRA PAIR would go out in full after the pair that
 * was already refreshed — exactly what the emitter's frame-dup cadence test
 * forbids ("TWO consecutive full half-frames, no more, before dups
 * resume"). In progressive mode (half=0), the deadline closes on the very
 * first successful send (no notion of an open pair): the order of rules 3/4
 * makes no difference there.
 * ------------------------------------------------------------------------- */
static int gm_dup_should_skip(gm_handle *h, int slot, int half,
                              const uint8_t *rgb888, size_t bytes, uint64_t now)
{
    if (!h->dup_enabled || bytes == 0u)
        return 0;                                    /* rule 1: lever off, zero cost */
    if (h->dup_last_full_ns == 0u)
        return 0;                                    /* rule 2: nothing to compare against */
    if (h->dup_force_full > 0u)
        return 0;                                    /* rule 4, evaluated before 3 — see above */
    if (now - h->dup_last_full_ns >= GM_FULL_FRAME_PERIOD_NS) {
        h->dup_force_full = half ? 2u : 1u;           /* rule 3: one-second deadline */
        return 0;
    }
    if (half && h->dup_pair_open)
        return 0;                                    /* rule 5: the xtra.dup branch of the
                                                        * CMD_BLIT_FIELD_VSYNC case in
                                                        * the receiver clears current_slot
                                                        * UNCONDITIONALLY — an interleaved dup
                                                        * would drop a field already received
                                                        * and validated (logged as
                                                        * partial_frame_dropped_on_dup, but lost
                                                        * all the same). */
    return (h->dup_prev_len[slot] == bytes &&
            memcmp(h->dup_prev[slot], rgb888, bytes) == 0) ? 1 : 0;  /* rule 6 */
}

/* ---------------------------------------------------------------------------
 * gm_dup_after_real — updates the frame-dup bookkeeping AFTER a successful
 * REAL send (never after a frame_dup — that one has nothing to remember).
 * Must receive the CLEAR buffer (never compressed): that is what
 * gm_dup_should_skip compares the next frame against.
 * ------------------------------------------------------------------------- */
static void gm_dup_after_real(gm_handle *h, int slot, int half,
                              const uint8_t *rgb888, size_t bytes, uint64_t now)
{
    if (!h->dup_enabled)
        return;   /* rule 1, "after" side: lever off, no bookkeeping — no
                   * realloc, no memcpy of the entire frame (up to 663,552
                   * bytes, 60 times a second on the runloop thread), no
                   * pair state evolving for nothing. The "zero cost"
                   * promised by gm_send_blit's comment holds true again.
                   * gm_set_frame_dup(h, 1) is only called at session open
                   * (record_groovy.c), before any send: the lever starts
                   * from a blank state, and dup_last_full_ns == 0 forces a
                   * full frame (rule 2). */
    int remembered = 1;
    if (h->dup_prev_cap[slot] < bytes) {
        /* Same pattern as gm_lz4_compress: realloc into a temporary
         * variable, never overwriting the existing pointer on failure. */
        uint8_t *grown = (uint8_t *)realloc(h->dup_prev[slot], bytes);
        if (!grown) {
            remembered = 0;  /* memory lost: the next frame will be full */
        } else {
            h->dup_prev[slot]     = grown;
            h->dup_prev_cap[slot] = bytes;
        }
    }
    if (remembered) {
        if (bytes > 0u)
            memcpy(h->dup_prev[slot], rgb888, bytes);
        h->dup_prev_len[slot] = bytes;
    } else {
        h->dup_prev_len[slot] = 0u;
    }

    if (half) {
        /* 480i: the deadline only resets on the CLOSE of a pair, i.e. once
         * a complete IMAGE (both parities) has gone out — not on every
         * half-frame. */
        h->dup_pair_open ^= 1u;
        if (!h->dup_pair_open)
            h->dup_last_full_ns = now;
    } else {
        h->dup_last_full_ns = now;
    }
    if (h->dup_force_full)
        h->dup_force_full--;
}

/* ---------------------------------------------------------------------------
 * gm_send_blit — CMD_BLIT_VSYNC (0x06), 11-byte header datagram (lz4_size
 * form, lz4_size = 0 for raw mode) followed by RGB888 payload chunks.
 *
 * WHY 11 BYTES, NOT 7: the 7-byte header was the first design. The
 * captured GroovyMAME 0.287 baseline (gridlee, CMD_BLIT_VSYNC) is
 * **11 bytes** — `06 01 00 00 00 f1 00 00 00 00 00`: GroovyMAME 0.287 emits
 * the 11-byte variant unconditionally even when compression=0. The
 * receiver's parser accepts `len ∈ {7, 11}`, so either is wire-valid, but
 * for byte-equivalence with that baseline we MUST emit the 11-byte form.
 *
 * Wire layout (11-byte header form):
 *   byte 0:    cmd       = 0x06
 *   byte 1-4:  frame_id  (u32 LE)
 *   byte 5-6:  vsync     (u16 LE)
 *   byte 7-10: lz4_size  (u32 LE; = 0 in raw mode)
 *
 * Header datagram stands ALONE on the wire. Payload bytes follow as
 * MTU-sized prefixless chunks; the receiver's continuation guard tracks
 * them against expected_bytes = h_active * v_active * 3.
 * Send exactly `bytes` — no padding, no truncation.
 * ------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
 * The CMD_BLIT_VSYNC (0x06) chunker itself: compression, 11-byte header,
 * MTU-sized chunks. Factored out of gm_send_blit so the 480i half-frame
 * path (function below) emits EXACTLY the same wire without duplicating
 * this logic. `bytes` is taken BY VALUE: compression here only overwrites
 * the local copy of this parameter — both callers therefore keep their own
 * `bytes` variable intact, in CLEAR form, for the frame-dup bookkeeping
 * after the call. Does NO frame-dup bookkeeping itself: that is the
 * caller's job (decision BEFORE, update AFTER success).
 * ------------------------------------------------------------------------- */
static int gm_emit_blit_0x06(gm_handle *h, uint32_t frame_id, uint16_t vsync,
                             const uint8_t *rgb888, size_t bytes)
{
    /* Compress BEFORE emitting the header — safety guard. Emitting first
     * and then failing would leave the receiver armed on an announced size
     * that nothing will ever fill: a frozen frame and a
     * lz4_decompress_failed. Explicit edge case: bytes == 0, compression is
     * skipped, lz4_size stays 0, only the header goes out — today's
     * behavior, pinned by a test in the emitter's test suite. */
    const uint8_t *payload = rgb888;
    uint32_t       lz4_size = 0u;
    if (h->compression && bytes > 0u) {
        payload = gm_lz4_compress(h, rgb888, bytes, &lz4_size);
        if (!payload) return -1;      /* nothing went out: no lying header */
        bytes  = (size_t)lz4_size;
    }

    /* Step 1: header datagram (11-byte form; lz4_size = 0 in raw sessions,
     * the real compressed byte count in LZ4 sessions). */
    uint8_t hdr[11];
    hdr[0] = CMD_BLIT_VSYNC;  /* 0x06 */
    memcpy(&hdr[1], &frame_id, sizeof(uint32_t));  /* LE on x86, matches wire */
    memcpy(&hdr[5], &vsync,    sizeof(uint16_t));
    memcpy(&hdr[7], &lz4_size, sizeof(uint32_t));
    if (gm_socket_sendto(h->sock, hdr, sizeof(hdr), &h->to) != (int)sizeof(hdr))
        return -1;

    /* Step 2: payload chunks of up to the session's MTU bytes EACH WITH NO
     * OPCODE PREFIX. The receiver's continuation guard reassembles them,
     * tracking against expected_bytes.
     * Send exactly `bytes` — no padding, no truncation. `payload` is
     * `rgb888` in raw sessions, the compressed scratch buffer otherwise.
     * gm_send_payload splits byte-for-byte like the original loop when the
     * split-send path is inactive, and groups the same chunks into
     * super-sends otherwise — the wire does not move. */
    return gm_send_payload(h, payload, bytes);
}

int gm_send_blit(gm_handle *h, uint32_t frame_id, uint16_t vsync,
                 const uint8_t *rgb888, size_t bytes)
{
    if (!h) return -1;
    if (bytes > 0 && !rgb888) return -1;

    /* Frame-dup, unchanged: the decision is made BEFORE compression — a
     * skipped frame has nothing to compress. gm_mono_ns() is only called if
     * the lever is active: zero cost when it is off, the wire does not move
     * a byte. */
    const uint64_t dup_now = h->dup_enabled ? gm_mono_ns() : 0u;
    if (gm_dup_should_skip(h, 0, 0, rgb888, bytes, dup_now))
        return gm_emit_frame_dup(h, frame_id, vsync, 0u);

    if (gm_emit_blit_0x06(h, frame_id, vsync, rgb888, bytes) != 0)
        return -1;

    /* `bytes` here is ALWAYS the CLEAR size: the chunker above takes it by
     * value, its own internal compression does not affect this variable.
     * gm_dup_after_real therefore always compares pixels, never an LZ4
     * block. */
    gm_dup_after_real(h, 0, 0, rgb888, bytes, dup_now);
    return 0;
}

/* ---------------------------------------------------------------------------
 * gm_send_blit_half — 480i half-frame, GroovyMAME path. Four points:
 *
 *   (1) the wire is that of gm_send_blit: an 11-byte CMD_BLIT_VSYNC (0x06)
 *       followed by its chunks. `field` does NOT travel — the receiver
 *       infers parity from `frame_id & 1`.
 *   (2) under the field-parity contract, field(n) = F XOR ((n - G) mod 2)
 *       always equals `n & 1`: the sender's choice (this function, indexed
 *       by `field`) and the receiver's inference (from `frame_id & 1`)
 *       agree by construction, for any value of G.
 *   (3) the pairing rule: no dup while a real half-frame is still waiting
 *       for its twin — the xtra.dup branch of the CMD_BLIT_FIELD_VSYNC case
 *       in the receiver clears current_slot UNCONDITIONALLY, so an
 *       interleaved dup would drop a field already received and validated
 *       (logged as partial_frame_dropped_on_dup, but lost all the same),
 *       and the pair would never display.
 *   (4) the full-frame cadence (GM_FULL_FRAME_PERIOD_NS) counts TWO
 *       half-frames at the deadline, so that an ENTIRE image (both
 *       parities) gets refreshed, never just one half.
 * ------------------------------------------------------------------------- */
int gm_send_blit_half(gm_handle *h, uint32_t frame_id, uint16_t vsync,
                      uint8_t field, const uint8_t *rgb888, size_t bytes)
{
    if (!h) return -1;
    if (field > 1u) return -1;
    if (bytes > 0 && !rgb888) return -1;

    const uint64_t now = h->dup_enabled ? gm_mono_ns() : 0u;
    if (gm_dup_should_skip(h, (int)field, 1, rgb888, bytes, now))
        return gm_emit_frame_dup(h, frame_id, vsync, field);

    if (gm_emit_blit_0x06(h, frame_id, vsync, rgb888, bytes) != 0)
        return -1;

    gm_dup_after_real(h, (int)field, 1, rgb888, bytes, now);
    return 0;
}

/* ---------------------------------------------------------------------------
 * gm_send_blit_field — CMD_BLIT_FIELD_VSYNC (0x07), header datagram followed
 * by RGB888 field payload chunks. Additive sibling to gm_send_blit
 * (progressive path stays byte-identical); adds the compressed header form.
 *
 * Wire layout authority: the receiver's protocol header, and its parser
 * for the length rule the receiver actually enforces.
 *
 *   Raw session (h->compression == 0), 8 bytes — UNCHANGED, bit-for-bit:
 *     byte 0     : cmd (0x07)
 *     bytes 1-4  : frame_id (u32 LE)
 *     byte 5     : field (0 = even rows, 1 = odd rows)
 *     bytes 6-7  : vsync (u16 LE)
 *
 *   Compressed session (h->compression == 1), 12 bytes:
 *     bytes 0-7  : identical to the raw form above
 *     bytes 8-11 : lz4_size (u32 LE) — the compressed byte count of THIS
 *                  field's own block
 *
 * The receiver refuses the 8-byte form outright once the session is
 * compressed (PARSE_BAD_COMPRESSION) — it will not guess. There is no
 * degraded-but-visible failure mode here: a mismatch means no image at all,
 * daemon alive, screen frozen. Length 13 (delta mode) is out of scope and is
 * never emitted — see gm_lz4_compress callers.
 *
 * Each field is its OWN LZ4 block, compressed from its own buffer. The
 * receiver re-arms its continuation guard at every 0x07 header, field 1 of a
 * 480i frame included — compressing an entire
 * interlaced frame as one block and slicing the result in two would NOT
 * produce two independently-decompressible blocks. Since record_groovy.c
 * already calls this function once per field with its own buffer, nothing
 * special is required here — just do not "optimize" by hoisting compression
 * above the field split.
 *
 * The handle's LZ4 scratch buffer is reused between field 0 and field 1 of
 * the same frame. Safe today because each call fully sends its block before
 * returning, and all gm_send_* calls come from the same thread
 * (video_threaded = false). The first thing a future move to
 * multi-threaded sending would silently break.
 *
 * Payload chunking: identical to gm_send_blit — chunks of up to the
 * session's MTU, NO opcode prefix on continuation datagrams (the receiver's
 * continuation guard reassembles each field).
 * ------------------------------------------------------------------------- */
int gm_send_blit_field(gm_handle *h, uint32_t frame_id, uint16_t vsync,
                       uint8_t field, const uint8_t *rgb888, size_t bytes)
{
    if (!h) return -1;
    if (field > 1) return -1;
    if (bytes > 0 && !rgb888) return -1;

    /* Frame-dup: this path stays OFF, DELIBERATELY — no call to
     * gm_dup_should_skip here. This path sends TWO fields per frame under
     * the SAME frame_id, and the decision for field 0 would be made without
     * knowing what field 1 will be. A dup for field 0 followed by a real
     * blit for field 1 would open a slot on the receiver side that would
     * never complete (frame_id_mismatch on the next frame) — exactly the
     * mix the pairing rule forbids. The 480i path goes through
     * gm_send_blit_half (function further down this file) instead, where
     * this rule holds slot by slot. This path otherwise remains the fork's
     * "no variable" behavior: it must stay bit-identical to the baseline,
     * which no call to the frame-dup logic could guarantee here. */

    /* Compress BEFORE emitting the header — same safety guard as
     * gm_send_blit: a compression failure on field 1 after field 0 has
     * already gone out must never leave a header announcing a size that
     * nothing will fill. bytes == 0 skips compression: the header stays at
     * 8 bytes, no size to announce (an empty field emits the header
     * alone, pinned by the emitter's LZ4 wire-contract test). */
    const uint8_t *payload  = rgb888;
    uint32_t       lz4_size = 0u;
    size_t         hdr_len  = 8u;

    if (h->compression && bytes > 0u) {
        payload = gm_lz4_compress(h, rgb888, bytes, &lz4_size);
        if (!payload) return -1;   /* nothing went out: no lying header */
        bytes   = (size_t)lz4_size;
        hdr_len = 12u;
    }

    /* Step 1: header datagram. The array is always 12 bytes wide, but only
     * hdr_len (8 or 12) is ever sent — NEVER sizeof(hdr), exactly the trap
     * gm.h already documents for the 0x06 header. */
    uint8_t hdr[12];
    hdr[0] = CMD_BLIT_FIELD_VSYNC;                    /* 0x07 */
    memcpy(&hdr[1], &frame_id, sizeof(uint32_t));     /* bytes 1-4 LE */
    hdr[5] = field;                                    /* byte 5 */
    memcpy(&hdr[6], &vsync,    sizeof(uint16_t));     /* bytes 6-7 LE */
    if (hdr_len == 12u)
        memcpy(&hdr[8], &lz4_size, sizeof(uint32_t)); /* bytes 8-11 LE */

    if (gm_socket_sendto(h->sock, hdr, hdr_len, &h->to) != (int)hdr_len)
        return -1;

    /* Step 2: payload chunks of up to the session's MTU bytes EACH WITH NO
     * OPCODE PREFIX (same chunking as gm_send_blit). `payload` is `rgb888` in
     * raw sessions, the compressed scratch buffer otherwise. gm_send_payload,
     * same reasoning as gm_emit_blit_0x06. */
    return gm_send_payload(h, payload, bytes);
}

/* ---------------------------------------------------------------------------
 * gm_send_audio — CMD_AUDIO (0x04), 3-byte header datagram followed by PCM
 * payload chunks. Additive sibling of gm_send_blit; the video path does not
 * move a byte.
 *
 * Wire format authority: `psakhis/Groovy_MiSTer` api/groovymister.cpp, CMD_AUDIO.
 *
 *   byte 0     : cmd = 0x04
 *   bytes 1-2  : payload size, little-endian u16
 *
 * The header goes out ALONE on the wire, like for BLIT; the PCM bytes follow
 * as datagrams with no opcode prefix. The receiver reassembles them against
 * its expected byte count.
 *
 * The size is carried by a u16: a payload over 65535 bytes is refused (-1)
 * rather than silently truncated. At 44100 Hz stereo 16-bit, one video
 * frame's worth is 2940 bytes — a twentyfold margin.
 *
 * `frames` counts left/right pairs, not bytes: that is the convention of
 * `struct record_audio_data` on the RetroArch side, and keeping it here
 * avoids one more conversion at the call site.
 * ------------------------------------------------------------------------- */
int gm_send_audio(gm_handle *h, const int16_t *pcm, size_t frames,
                  uint8_t channels)
{
    if (!h) return -1;
    if (channels != 1u && channels != 2u) return -1;
    if (frames > 0 && !pcm) return -1;

    size_t bytes = frames * (size_t)channels * sizeof(int16_t);
    if (bytes > 0xFFFFu) return -1;   /* the size would not fit in a u16 */

    /* Padded mode: never a tiny block (see gm_internal.h, acarry). A block
     * that, carry included, stays under GM_TAIL_MIN is HELD BACK, not sent
     * (returns 0, NOTHING went out); otherwise the carry goes out at the
     * front of this block, in the same CMD_AUDIO. Sample ORDER is always
     * preserved: if carry + block does not fit in the u16, two CMD_AUDIO
     * packets go out — the carry followed by the block minus its last
     * GM_TAIL_MIN bytes, then those last GM_TAIL_MIN bytes. Sending the
     * carry alone first would send the tiny block this mode forbids.
     * Unreachable from record_groovy.c (at most 32768 bytes per call),
     * covered by a test. */
    const uint8_t *p = (const uint8_t *)pcm;
    if (h->padded) {
        if (h->acarry_len + bytes < GM_TAIL_MIN) {
            if (bytes > 0) memcpy(h->acarry + h->acarry_len, p, bytes);
            h->acarry_len += bytes;
            return 0;
        }
        if (h->acarry_len > 0 && h->acarry_len + bytes > 0xFFFFu) {
            /* bytes > 65535 - 255: well above GM_TAIL_MIN. carry + head <=
             * 255 + 65535 - 256: fits in acarry[65536] and in the u16.
             * GM_TAIL_MIN is a multiple of 4: the cut lands between two
             * sample pairs. */
            const size_t head_bytes = bytes - GM_TAIL_MIN;
            const size_t total      = h->acarry_len + head_bytes;
            memcpy(h->acarry + h->acarry_len, p, head_bytes);
            h->acarry_len = 0;
            if (gm_emit_audio_block(h, h->acarry, total) != 0)
                return -1;
            return gm_emit_audio_block(h, p + head_bytes, GM_TAIL_MIN);
        }
        if (h->acarry_len > 0) {
            memcpy(h->acarry + h->acarry_len, p, bytes);
            bytes += h->acarry_len;
            p = h->acarry;
            h->acarry_len = 0;
        }
    }
    return gm_emit_audio_block(h, p, bytes);
}

/* A CMD_AUDIO on the wire: the header, then the payload. `bytes` fits in a
 * u16 — guaranteed by gm_send_audio, the only caller. */
static int gm_emit_audio_block(gm_handle *h, const uint8_t *p, size_t bytes)
{
    /* Step 1: header. Padded-session mode: same pattern as gm_send_init —
     * a GM_PAD_LEN-byte buffer zeroed BEFORE being filled; hdr[0] and
     * hdr[1..2] are IDENTICAL in both modes, five tail zeros in padded
     * mode. The length SENT follows h->padded, never sizeof(buffer). */
    uint8_t hdr[GM_PAD_LEN];
    memset(hdr, 0, sizeof(hdr));
    uint16_t size16 = (uint16_t)bytes;
    hdr[0] = CMD_AUDIO;                              /* 0x04 */
    memcpy(&hdr[1], &size16, sizeof(uint16_t));      /* bytes 1-2, LE */
    size_t hdr_len = h->padded ? (size_t)GM_PAD_LEN : 3u;

    if (gm_socket_sendto(h->sock, hdr, hdr_len, &h->to) != (int)hdr_len)
        return -1;

    /* Step 2: the payload, in chunks of at most the session's MTU, WITH NO
     * opcode prefix — same chunking as gm_send_blit. In padded mode, chunk
     * size comes from gm_next_chunk, which never leaves a tail shorter than
     * GM_TAIL_MIN bytes; the block's byte count itself does not change. In
     * classic mode, the split is the original one, byte for byte. */
    size_t off = 0;
    while (off < bytes) {
        size_t remaining = bytes - off;
        size_t chunk = gm_next_chunk(h, remaining);   /* never a short tail in padded mode */
        if (gm_socket_sendto(h->sock, p + off, chunk, &h->to) != (int)chunk)
            return -1;
        off += chunk;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * gm_send_close — CMD_CLOSE (0x01), single-byte teardown packet.
 * ------------------------------------------------------------------------- */
int gm_send_close(gm_handle *h)
{
    if (!h) return -1;
    /* Padded-session mode: same pattern as gm_send_init/gm_send_audio — a
     * GM_PAD_LEN-byte buffer zeroed out, `01` then seven zeros in padded
     * mode. */
    uint8_t pkt[GM_PAD_LEN];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = CMD_CLOSE;
    size_t len = h->padded ? (size_t)GM_PAD_LEN : 1u;
    return gm_socket_sendto(h->sock, pkt, len, &h->to) == (int)len ? 0 : -1;
}

/* ---------------------------------------------------------------------------
 * gm_recv_status — receive a 13-byte status ACK from the receiver daemon.
 *
 * timeout_ms == 0 : poll non-blocking (returns -1 immediately if no packet).
 * timeout_ms  > 0 : wait up to timeout_ms milliseconds for a packet.
 *
 * Wire format (13 bytes, LE, fixed offsets per Groovy v2 protocol):
 *
 *   offset 0-3:  frame_echo  u32 LE
 *   offset 4-5:  vcount_echo u16 LE
 *   offset 6-9:  frame       u32 LE   <-- must read from +6, NOT +8
 *   offset 10-11: vcount     u16 LE
 *   offset 12:   flags       u8
 *
 * Padding bug seen on hardware (2026-05-10): the original implementation
 * received into a gm_status_pkt_t (packed, correct), then field-copied to
 * gm_status *out (unpacked). MinGW-w64 inserted 2-byte padding between
 * vcount_echo (u16 at offset 4) and frame (u32, aligned to offset 8),
 * making gm_status 16 bytes.
 * The ctypes caller uses GmStatus(_pack_=1) — 13-byte layout — so it read
 * frame from in-memory offset 6, which in the 16-byte C struct held the
 * 2 padding bytes and the low bytes of frame, yielding 0x2B0000 instead of 43.
 *
 * Fix (belt-and-suspenders):
 *   (1) gm_status moved inside #pragma pack(push,1) in gm.h — compile-time
 *       defense via _Static_assert(sizeof(gm_status) == 13).
 *   (2) Decode via explicit byte-offset memcpy into a raw wire buffer —
 *       independent of any struct packing, portable across compilers.
 *
 * Size is validated before decode (truncated/oversized ACK rejected).
 * On success: returns 0, populates *out, caches result in h->last_status.
 * On error/timeout/size mismatch: returns -1, leaves *out unmodified.
 * ------------------------------------------------------------------------- */
int gm_recv_status(gm_handle *h, gm_status *out, int timeout_ms)
{
    if (!h || !out) return -1;

    /* Raw byte buffer for wire receive. Wire is always exactly 13 bytes.
     * We do NOT receive directly into a struct — see the padding note above. */
    uint8_t wire_buf[13];
    struct sockaddr_in from;
    memset(&from, 0, sizeof(from));
    int n = gm_socket_recvfrom_timeout_from(h->sock, wire_buf, sizeof(wire_buf),
                                            timeout_ms, &from);
    if (n != 13) return -1;  /* timeout, error, or truncated/oversized ACK */

    /* Decode 13-byte ACK with explicit byte offsets. Defends against
     * struct-packing surprises across compilers. */
    memcpy(&out->frame_echo,  wire_buf + 0,  sizeof(uint32_t));  /* bytes 0-3  */
    memcpy(&out->vcount_echo, wire_buf + 4,  sizeof(uint16_t));  /* bytes 4-5  */
    memcpy(&out->frame,       wire_buf + 6,  sizeof(uint32_t));  /* bytes 6-9  */
    memcpy(&out->vcount,      wire_buf + 10, sizeof(uint16_t));  /* bytes 10-11 */
    out->flags = wire_buf[12];                                     /* byte  12   */

    /* Cache for caller-side staleness checks. */
    h->last_status = *out;
    h->has_status  = 1;
    h->last_status_from = from;
    h->has_status_from  = 1;

    return 0;
}

/* ---------------------------------------------------------------------------
 * gm_send_get_status_internal — send a 1-byte CMD_GET_STATUS (0x05) poll.
 *
 * Internal helper for the heartbeat thread (gm_heartbeat.c). Not in the
 * public API. Declaration lives in gm_internal.h.
 * ------------------------------------------------------------------------- */
int gm_send_get_status_internal(gm_handle *h)
{
    if (!h) return -1;
    /* Padded-session mode: `05` then seven zeros in padded mode. Padding
     * added FOR CORRECTNESS -- gm_heartbeat_start has no caller anywhere in
     * the RetroArch driver today, so this form never actually goes out in
     * a real session at present. */
    uint8_t pkt[GM_PAD_LEN];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = CMD_GET_STATUS;  /* 0x05 */
    size_t len = h->padded ? (size_t)GM_PAD_LEN : 1u;
    return gm_socket_sendto(h->sock, pkt, len, &h->to) == (int)len ? 0 : -1;
}

/* ---------------------------------------------------------------------------
 * gm_last_ack_frame — the RECEIVER's frame counter, last ack received (input
 * channel). h->last_status is already filled by gm_recv_status; no change
 * to this function is needed.
 * ------------------------------------------------------------------------- */
uint32_t gm_last_ack_frame(const gm_handle *h)
{
    if (!h || !h->has_status) return 0u;
    return h->last_status.frame;
}

/* ---------------------------------------------------------------------------
 * gm_last_status_peer — source address of the LAST status received.
 * Convenience accessor over h->last_status_from, on the exact same model as
 * gm_last_ack_frame: gm_status stays fixed at 13 bytes, nothing is added
 * to it.
 * Returns 0 and leaves *out_ip / *out_port untouched if no status has
 * arrived yet. out_ip comes back in NETWORK order (sin_addr.s_addr as is);
 * out_port in HOST order.
 * ------------------------------------------------------------------------- */
int gm_last_status_peer(const gm_handle *h, uint32_t *out_ip, uint16_t *out_port)
{
    if (!h || !h->has_status_from) return 0;
    if (out_ip)   *out_ip   = (uint32_t)h->last_status_from.sin_addr.s_addr;
    if (out_port) *out_port = (uint16_t)ntohs(h->last_status_from.sin_port);
    return 1;
}

/* ---------------------------------------------------------------------------
 * gm_peer_addr — the address this handle ACTUALLY targets. h->to is set
 * once by gm_socket_open, from inet_addr: "192.0.2.010" becomes 192.0.2.8
 * there (octal). A caller comparing peer addresses must start from THIS
 * value, never a second decimal reading of the same text. Same output
 * convention as gm_last_status_peer: ip in NETWORK order, port in HOST
 * order.
 * ------------------------------------------------------------------------- */
int gm_peer_addr(const gm_handle *h, uint32_t *out_ip, uint16_t *out_port)
{
    if (!h) return 0;
    if (out_ip)   *out_ip   = (uint32_t)h->to.sin_addr.s_addr;
    if (out_port) *out_port = (uint16_t)ntohs(h->to.sin_port);
    return 1;
}

/* ---------------------------------------------------------------------------
 * gm_last_announce_error — outcome of the last gm_send_init or
 * gm_send_switchres on this handle. Simple relay of h->last_announce_err,
 * set by gm_socket_sendto_err via gm_send_init/gm_send_switchres.
 * ------------------------------------------------------------------------- */
int gm_last_announce_error(const gm_handle *h)
{
    if (!h) return GM_SEND_ERR_REFUSED;
    return h->last_announce_err;
}

/* ---------------------------------------------------------------------------
 * gm_frame_dup_count — number of frame_dup packets sent since session open.
 * Monotonic counter, never reset by any function: the caller diffs two
 * readings, the same pattern as record_groovy.c's periodic telemetry.
 * ------------------------------------------------------------------------- */
uint32_t gm_frame_dup_count(gm_handle *h)
{
    if (!h) return 0u;
    return h->dup_count;
}
