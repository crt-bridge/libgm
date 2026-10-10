/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gm.h — public API for libgm (Wire+Transport for crt-bridge emitters).
 *
 * Wire structs and opcodes are byte-exact mirrors of the receiver's protocol
 * header (the daemon-side ground truth).
 *
 * Reconciliations from older docs (CRITICAL — do not "fix"):
 *   - CMD_INIT is 4 bytes on the wire (hardware-in-the-loop verified,
 *     capture `02 00 03 02`). Mistglow's 5-byte form with rgb_mode is wrong
 *     for our daemon; the receiver's protocol header enforces the same
 *     sizeof() == 4 guard.
 *   - pclock is in MHz (NOT Hz). GroovyMAME 0.287 sends a double in MHz;
 *     the receiver multiplies by 1.0e6. libgm matches GroovyMAME —
 *     sends MHz to remain bit-exact (verified via a Python UDP sniffer).
 *   - CMD_BLIT_VSYNC opcode is 0x06 (progressive). 0x07 is
 *     CMD_BLIT_FIELD_VSYNC for interlaced; libgm's MVP sends 0x06 only.
 *
 * Build target: MinGW-w64 on Windows. MSVC is not a goal for v1.
 */

#ifndef GM_H
#define GM_H

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Opcodes — verbatim from the receiver's protocol header
 * ------------------------------------------------------------------------- */
#define CMD_CLOSE             0x01
#define CMD_INIT              0x02
#define CMD_SWITCHRES         0x03
#define CMD_AUDIO             0x04
#define CMD_GET_STATUS        0x05
#define CMD_BLIT_VSYNC        0x06
#define CMD_BLIT_FIELD_VSYNC  0x07
#define CMD_GET_VERSION       0x08

/* ---------------------------------------------------------------------------
 * Status flags — verbatim from the receiver's protocol header
 * ------------------------------------------------------------------------- */
#define FLAG_VRAM_READY      0x01
#define FLAG_VRAM_END_FRAME  0x02
#define FLAG_VRAM_SYNCED     0x04
#define FLAG_VGA_FRAMESKIP   0x08
#define FLAG_VGA_VBLANK      0x10
#define FLAG_VGA_F1          0x20
#define FLAG_AUDIO           0x40
#define FLAG_VRAM_QUEUE      0x80

/* ---------------------------------------------------------------------------
 * Wire structs (BYTE-EXACT mirrors of the receiver's protocol header).
 *
 * Packing defense: every wire struct is gated by a file-scope
 * _Static_assert against its expected sizeof(). If MinGW packing rules drift
 * or a future edit reorders fields, the build fails at compile time — better
 * than silent wire corruption on the JVC.
 * ------------------------------------------------------------------------- */
#pragma pack(push, 1)

/* CMD_INIT (0x02). 4 bytes. Hardware-verified GroovyMAME 0.287 wire = 02 00 03 02. */
typedef struct {
    uint8_t cmd;          /* 0x02 */
    uint8_t compression;  /* On the wire: 0 = raw, 1 = LZ4, and nothing
                            * else. Verified 2026-08-31 against the
                            * upstream source: groovymister.cpp:448 writes
                            * (lz4Frames) ? 1 : 0, and groovy.cpp:1141
                            * clamps any value > 1 to 0. The 0-6 modes
                            * psakhis documents are the LOCAL setting of
                            * his own emitter; they never travel on the
                            * wire. Mirrors the receiver's protocol header. */
    uint8_t sample_rate;  /* audio sample-rate code */
    uint8_t channels;     /* 0=disabled, 1=mono, 2=stereo */
} gm_cmd_init_t;
_Static_assert(sizeof(gm_cmd_init_t) == 4, "CMD_INIT must be 4 bytes");

/* CMD_SWITCHRES (0x03). 26 bytes.
 * pclock is IEEE 754 LE double at offset 1 — UNALIGNED on every architecture.
 * Always memcpy when filling this field. Units: MHz (NOT Hz). */
typedef struct {
    uint8_t  cmd;        /* 0x03 */
    double   pclock;     /* MHz, IEEE 754 LE — offset 1, UNALIGNED */
    uint16_t h_active;
    uint16_t h_begin;
    uint16_t h_end;
    uint16_t h_total;
    uint16_t v_active;
    uint16_t v_begin;
    uint16_t v_end;
    uint16_t v_total;
    uint8_t  interlace;
} gm_cmd_switchres_t;
_Static_assert(sizeof(gm_cmd_switchres_t) == 26,
               "CMD_SWITCHRES must be 26 bytes");

/* CMD_BLIT_VSYNC (0x06) — header packet.
 *
 * gm_send_blit always emits the 11-byte form (byte-equivalent with the
 * GroovyMAME 0.287 baseline — see the "WHY 11 BYTES" note in gm_proto.c).
 * lz4_size is 0 in raw sessions, and the real compressed byte count when
 * the session was opened with compression=1. The header
 * packet stands ALONE on the wire — payload bytes arrive in subsequent UDP
 * datagrams with NO opcode prefix and are reassembled by the daemon's
 * continuation guard.
 *
 * NO _Static_assert — wire size is form-dependent (the receiver still
 * accepts a 7-byte form with lz4_size omitted). Senders must take care to
 * emit exactly 7 or 11 bytes, not sizeof(this struct). */
typedef struct {
    uint8_t  cmd;        /* 0x06 */
    uint32_t frame_id;
    uint16_t vsync;
    uint32_t lz4_size;   /* 0 = raw ; real compressed size when session is LZ4 */
} gm_cmd_blit_vsync_base_t;

/* Status ACK (13 bytes). Daemon → emitter. */
typedef struct {
    uint32_t frame_echo;
    uint16_t vcount_echo;
    uint32_t frame;
    uint16_t vcount;
    uint8_t  flags;
} gm_status_pkt_t;
_Static_assert(sizeof(gm_status_pkt_t) == 13, "ACK must be 13 bytes");

/* Status snapshot returned by gm_recv_status (host byte order).
 *
 * MUST be inside #pragma pack(push,1) so ctypes callers using _pack_=1 see
 * the same 13-byte layout as the wire struct. Without packing, MinGW-w64
 * inserts 2 bytes of padding between vcount_echo (u16 at offset 4) and
 * frame (u32, naturally aligned to offset 8), shifting frame to offset 8
 * instead of the wire's offset 6. ctypes GmStatus(_pack_=1) then reads the
 * wrong bytes, yielding frame=0x2B0000 instead of 43 (seen on hardware,
 * 2026-05-10).
 */
typedef struct {
    uint32_t frame_echo;
    uint16_t vcount_echo;
    uint32_t frame;
    uint16_t vcount;
    uint8_t  flags;
} gm_status;
_Static_assert(sizeof(gm_status) == 13,
               "gm_status must be 13 bytes — Win32 padding bug");

/* ---------------------------------------------------------------------------
 * Groovy input channel (UDP/32101). Bit-exact mirror of the receiver's input
 * header -- any divergence between the two is a bug, just like between the
 * receiver's protocol header and gm.h for video. The two upstream
 * implementations (input_mister.cpp:59-99, antonioginer/GroovyMAME,
 * BSD-3-Clause; and groovymister_wrapper.h:52-66, psakhis/Groovy_MiSTer,
 * GPL-2.0-only) agree bit-for-bit on these four layouts -- an
 * unprotectable protocol fact, no line is borrowed from either one.
 * ------------------------------------------------------------------------- */
typedef struct { uint32_t frame; uint8_t order; uint16_t joy1, joy2; } gm_joy_dig_inputs;
_Static_assert(sizeof(gm_joy_dig_inputs) == 9,  "digital joystick = 9 bytes");

typedef struct { uint32_t frame; uint8_t order; uint16_t joy1, joy2;
                 int8_t joy1_lx, joy1_ly, joy1_rx, joy1_ry;
                 int8_t joy2_lx, joy2_ly, joy2_rx, joy2_ry; } gm_joy_inputs;
_Static_assert(sizeof(gm_joy_inputs) == 17, "joystick + analog = 17 bytes");

typedef struct { uint32_t frame; uint8_t order; uint8_t keys[32]; } gm_keyboard_inputs;
_Static_assert(sizeof(gm_keyboard_inputs) == 37, "PS/2 keyboard = 37 bytes");

typedef struct { uint32_t frame; uint8_t order; uint8_t keys[32];
                 uint8_t mouse_bits, mouse_x, mouse_y, mouse_z; } gm_ps2_inputs;
_Static_assert(sizeof(gm_ps2_inputs) == 41, "keyboard + mouse = 41 bytes");

#pragma pack(pop)

/* Input-channel button masks -- same values as the receiver's input header
 * (prefixed GM_JOY_ instead of GROOVY_JOY_ on the emitter side). Bits 14/15
 * are undefined upstream: do not invent them. */
#define GM_JOY_RIGHT (1u<<0)
#define GM_JOY_LEFT  (1u<<1)
#define GM_JOY_DOWN  (1u<<2)
#define GM_JOY_UP    (1u<<3)
#define GM_JOY_B1    (1u<<4)
#define GM_JOY_B2    (1u<<5)
#define GM_JOY_B3    (1u<<6)
#define GM_JOY_B4    (1u<<7)
#define GM_JOY_B5    (1u<<8)
#define GM_JOY_B6    (1u<<9)
#define GM_JOY_B7    (1u<<10)
#define GM_JOY_B8    (1u<<11)
#define GM_JOY_B9    (1u<<12)
#define GM_JOY_B10   (1u<<13)

/* ---------------------------------------------------------------------------
 * THREADING CONTRACT (caller-owned):
 *
 * libgm provides primitives — gm_send_*, gm_recv_status — that are SAFE to
 * call from a single thread. They are NOT thread-safe across multiple threads
 * sharing the same gm_handle. If a caller needs concurrent send + recv, the
 * caller must own the synchronization.
 *
 * gm_heartbeat_start/stop is OPT-IN. The heartbeat thread internally calls
 * gm_send_get_status_internal which performs sendto on h->sock. If the caller
 * also calls gm_send_blit on the SAME handle from another thread, the caller
 * must serialize sendto calls (e.g., a mutex around the gm_send_* family).
 *
 * The expected use pattern (record_groovy on Windows, single-threaded video
 * driver, video_threaded=false): all gm_send_* are called from one
 * thread; gm_heartbeat handles its own send concurrency by running on its own
 * thread, but record_groovy will USUALLY not enable heartbeat because
 * BLIT_VSYNC traffic is itself the implicit heartbeat during active gameplay.
 *
 * Input channel: gm_bind_inputs/gm_poll_inputs/gm_get_*_inputs are called
 * from a SINGLE thread, and that thread may be the same one that calls
 * gm_send_* -- the input channel owns its OWN socket (input_sock), so there
 * is no interleaving of sendto with video. That is what makes calling it
 * from RetroArch's main loop safe.
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * Public API. Caller-owned threading model: the heartbeat helper is
 * OPTIONAL — record_groovy uses it; a future Duckstation integration may
 * not. No global state — all functions take a gm_handle*.
 * ------------------------------------------------------------------------- */

/* Opaque session handle. Definition lives in src/gm_*.c. */
typedef struct gm_handle gm_handle;

/* Init params for gm_send_init (host byte order; libgm packs to wire). */
typedef struct {
    uint8_t compression;   /* On the wire: 0 = raw, 1 = LZ4, and nothing
                             * else -- see the comment on
                             * gm_cmd_init_t.compression. Kept in the handle
                             * by gm_send_init; the gm_send_blit* calls that
                             * follow read it back from h. */
    uint8_t sample_rate;   /* audio sample-rate code; ignored by the receiver */
    uint8_t channels;      /* 0 = audio disabled (not yet implemented) */
} gm_init_params;

/* Modeline for gm_send_switchres. pclock_mhz is MHz — see header note. */
typedef struct {
    double   pclock_mhz;
    uint16_t h_active, h_begin, h_end, h_total;
    uint16_t v_active, v_begin, v_end, v_total;
    uint8_t  interlace;
} gm_modeline;

/* ---------------------------------------------------------------------------
 * Per-session send MTU.
 *
 * 1472 = 1500 (Ethernet) - 20 (IPv4) - 8 (UDP). This is the default, and the
 * value the bench bridge keeps: without GROOVY_MTU, the wire does not move
 * by a single byte.
 *  548 = 576 (the minimum MTU IPv4 guarantees on any path) - 28.
 * 3800 = the Groovy protocol's "jumbo" size.
 *
 * Target value for a peer behind WireGuard: 1252 = 1280 - 20 - 8. Without
 * it, every datagram leaves as TWO IP fragments -- measured on a live test
 * run over Wi-Fi/Tailscale.
 * ------------------------------------------------------------------------- */
#define GM_MTU_DEFAULT 1472u
#define GM_MTU_MIN      548u
#define GM_MTU_MAX     3800u

/* ---------------------------------------------------------------------------
 * Unchanged-frame skip.
 * ------------------------------------------------------------------------- */

/* Maximum period between two FULL frames while frame skip is active.
 * A constant, not a setting: one more knob would be a source of drift
 * between the launch script and the fork. A receiver that joins mid-
 * session, a datagram lost on the WAN, or a daemon restart therefore cost
 * at most one second of a frozen picture.
 *
 * Receiver-side constraint: on a static scene, the daemon's frame queue
 * only fills at this cadence, so its idle screen (crt-bridge-rcv's
 * --idle-after and its minimum) must wait STRICTLY
 * longer than this period -- otherwise it draws itself and disappears on
 * every forced full frame. Changing this constant requires revisiting that
 * bound. */
#define GM_FULL_FRAME_PERIOD_NS 1000000000ull

/* Unchanged-frame skip. 0 = off (DEFAULT: the wire does not change by a
 * single byte), 1 = active. Returns 0. Call after gm_init, before
 * gm_send_init. */
int      gm_set_frame_dup(gm_handle *h, int on);

/* ---------------------------------------------------------------------------
 * Padded session mode.
 *
 * On this Wi-Fi link, a UDP datagram of 6 bytes or less dies and takes the
 * large neighboring datagram down with it; 7 bytes and up get through 100%
 * of the time -- a threshold measured TO THE BYTE, identical on the Mac and
 * the NUC: 6 kills, 7 saves. GM_PAD_LEN is 8 -- one byte of DELIBERATE
 * margin above the measured threshold. 8 is also the size of the image
 * header in a raw session (CMD_BLIT_FIELD_VSYNC), so it is a value already
 * proven at 100% on this link.
 *
 * This is a SESSION MODE, set once on the handle BEFORE gm_send_init --
 * NEVER a decision made datagram by datagram. The first CMD_INIT negotiates
 * this mode with the peer through ITS OWN length: 8 bytes open a padded
 * session, 4 (or 5, as a tolerated exception) an unchanged, classic
 * session -- this is how GroovyMAME (which never calls gm_set_padding)
 * stays accepted without a single byte of its path moving.
 *
 * The table of both forms -- authoritative:
 *
 * | command                      | classic                 | padded                                | added   |
 * |-------------------------------|-------------------------|----------------------------------------|---------|
 * | CMD_CLOSE 0x01                 | 1: `01`                  | 8: `01 00 00 00 00 00 00 00`            | 7 zeros |
 * | CMD_INIT 0x02                  | 4: `02 comp rate chan`   | 8: `02 comp rate chan 00 00 00 00`      | 4 zeros |
 * | CMD_SWITCHRES 0x03             | 26                       | 26, UNCHANGED                           | --      |
 * | CMD_AUDIO 0x04 (header)        | 3: `04 sz_lo sz_hi`      | 8: `04 sz_lo sz_hi 00 00 00 00 00`      | 5 zeros |
 * | CMD_GET_STATUS 0x05            | 1: `05`                  | 8: `05 00 00 00 00 00 00 00`            | 7 zeros |
 * | CMD_BLIT_VSYNC 0x06            | 11                       | 11, UNCHANGED                           | --      |
 * | CMD_BLIT_FIELD_VSYNC 0x07      | 8 / 9 / 12               | UNCHANGED                               | --      |
 * | CMD_GET_VERSION 0x08           | 1                        | libgm NEVER EMITS IT                    | --      |
 *
 * PAYLOADS (PCM chunks, image chunks) are NEVER padded -- only the small
 * command header is; padding a payload would throw off the receiver's
 * expected byte count. They are still not PADDED -- not a single byte
 * added -- but in padded mode their CHUNKING changes: never a payload TAIL
 * under 256 bytes (the last two chunks are rebalanced, both image and
 * audio), and a WHOLE audio block under 256 bytes is held back and glued
 * to the front of the next one. The one accepted exception: a WHOLE image
 * under 256 bytes goes out as-is. See gm_next_chunk.
 * No crt-bridge emitter sends `CMD_GET_VERSION`: nothing to
 * code here, its padding will only ever live in later recognition tables,
 * not in this header.
 *
 * ABSENT setting = BIT-IDENTICAL wire: without this call, gm_send_init and
 * the three other short emitters (gm_send_audio, gm_send_close,
 * gm_send_get_status_internal) emit EXACTLY what they emitted before this
 * mode existed -- guarded by a capture-comparison test in the emitter's
 * test suite.
 * ------------------------------------------------------------------------- */
#define GM_PAD_LEN 8u

/* Padded session mode. 0 = off (DEFAULT), 1 = active. Returns 0, -1 if h is
 * NULL (same convention as gm_set_frame_dup). Call AFTER gm_init and BEFORE
 * gm_send_init: it is gm_send_init that negotiates the mode with the peer,
 * through its length. libgm does not forbid a later call, nor does it
 * promise one works: a CMD_INIT already sent in one mode does not
 * retroactively change shape. */
int      gm_set_padding(gm_handle *h, int on);

/* Number of frame_dup packets emitted since the session opened. A
 * monotonic counter, reset by no function: the caller takes the difference
 * between two reads (the pattern record_groovy.c's periodic telemetry
 * uses). */
uint32_t gm_frame_dup_count(gm_handle *h);

/* 480i half-frame, GroovyMAME path. Emits EXACTLY the same wire bytes as
 * gm_send_blit -- an 11-byte CMD_BLIT_VSYNC (0x06) header followed by its
 * chunks -- but tells libgm WHICH field parity is travelling, information
 * the 0x06 header does not carry and that the receiver infers from
 * frame_id & 1. `field` does NOT go on the wire: it is only used for
 * frame-skip bookkeeping (previous buffer per parity, pairing rule). */
int      gm_send_blit_half(gm_handle *h, uint32_t frame_id, uint16_t vsync,
                           uint8_t field, const uint8_t *rgb888, size_t bytes);

/* ---------------------------------------------------------------------------
 * Lifecycle + wire-emit primitives. All return 0 on success, -1 on error.
 * receiver_ip accepts either an IPv4 address in dotted-decimal notation
 * ("192.0.2.10") or a hostname (fully qualified, short, or ".local"),
 * resolved ONCE when the session opens via getaddrinfo (gm_socket_open). A
 * name that fails to resolve makes gm_init return NULL, same as a null
 * pointer -- clean refusal, nothing is sent. The resolution is frozen for
 * the life of the session: a receiver that changes address mid-game
 * requires a restart.
 *
 * gm_init returns NULL for any of these six causes, each with its code;
 * gm_last_init_error() returns that code on the calling thread:
 *   - GM_INIT_ERR_NO_ADDRESS    receiver_ip is NULL;
 *   - GM_INIT_ERR_WINSOCK       Winsock initialization failed (Windows only);
 *   - GM_INIT_ERR_ALLOC         the handle allocation failed;
 *   - GM_INIT_ERR_SOCKET        the socket could not be opened;
 *   - GM_INIT_ERR_RESOLVE       receiver_ip failed to resolve;
 *   - GM_INIT_ERR_SEND_SEGMENT  the socket already holds, at open time, a
 *                               send segment shorter than the default MTU
 *                               (Windows only): a handle that would already
 *                               come out pre-split is never handed back to
 *                               the caller.
 * After a successful gm_init, gm_last_init_error() returns GM_INIT_OK.
 * port = 32100 default.
 * gm_recv_status: timeout_ms == 0 → non-blocking poll.
 * ------------------------------------------------------------------------- */
gm_handle *gm_init(const char *receiver_ip, uint16_t port);

/* Cause codes for gm_init, read back through gm_last_init_error(). Plain
 * integers, same style as GM_SEND_ERR_*. */
#define GM_INIT_OK                0
#define GM_INIT_ERR_NO_ADDRESS    1
#define GM_INIT_ERR_WINSOCK       2
#define GM_INIT_ERR_ALLOC         3
#define GM_INIT_ERR_SOCKET        4
#define GM_INIT_ERR_RESOLVE       5
#define GM_INIT_ERR_SEND_SEGMENT  6

/* Cause of the last gm_init call made on the CALLING thread: one of the
 * GM_INIT_* codes above, GM_INIT_OK after a success (a stale cause never
 * outlives the next call). Another thread's gm_init does not change it.
 * GM_INIT_OK as well if this thread never called gm_init. */
int gm_last_init_error(void);

/* Human-readable text for a GM_INIT_* code: a static English string, never
 * NULL; "unknown error" for any other value. */
const char *gm_init_error_str(int code);

/* Maximum size of a PAYLOAD datagram, for this session.
 * All three chunkers honor it: gm_send_blit, gm_send_blit_field,
 * gm_send_audio. Command headers are NOT affected — CMD_INIT (4),
 * CMD_SWITCHRES (26), CMD_CLOSE (1) and the blit headers (7/8/11/12) are
 * all shorter than GM_MTU_MIN.
 *
 * PER SESSION, never global: this is the shape a two-receiver fan-out
 * needs — the living-room link at 1472 and a remote peer at 1252, in the
 * same process, each with its own handle.
 *
 * Returns 0 if the value is accepted; -1 for THREE causes:
 *   - h is NULL;
 *   - mtu is outside [GM_MTU_MIN, GM_MTU_MAX];
 *   - the socket holds, OR LIBGM CANNOT RULE OUT THAT IT HOLDS, a send
 *     segment shorter than mtu (Windows only, UDP_SEND_MSG_SIZE that libgm
 *     could neither clear nor read back) -- an MTU WITHIN BOUNDS can
 *     therefore still be refused, the current MTU staying in place.
 * A REFUSAL LEAVES THE CURRENT VALUE UNTOUCHED. For the third cause, libgm
 * writes a `[gm-send]` line to stderr, which the bench does not capture:
 * it is up to the caller to test the -1 and log it themselves. On the
 * emitter side, groovy_mtu_parse returns GM_MTU_DEFAULT for any invalid
 * input, so a bad GROOVY_MTU does fall back to 1472 — and record_groovy
 * says so in its log. */
int        gm_set_mtu(gm_handle *h, unsigned mtu);
int        gm_send_init(gm_handle *h, const gm_init_params *p);
int        gm_send_switchres(gm_handle *h, const gm_modeline *m);
int        gm_send_blit(gm_handle *h, uint32_t frame_id, uint16_t vsync,
                        const uint8_t *rgb888, size_t bytes);

/* Additive: emit a 480i field via CMD_BLIT_FIELD_VSYNC (0x07).
 * field = 0 (even rows) or 1 (odd rows); higher values rejected with -1.
 * Wire layout: 8-byte header (cmd|frame_id|field|vsync) in a raw session,
 * 12 bytes (same 8 plus lz4_size u32 LE) when the session was opened with
 * compression=1 — the receiver refuses the 8-byte form outright once
 * compressed (PARSE_BAD_COMPRESSION), it will not guess.
 * Each field is compressed and sent as its own independent LZ4 block.
 * Returns 0 on success, -1 on error. */
int        gm_send_blit_field(gm_handle *h, uint32_t frame_id, uint16_t vsync,
                              uint8_t field, const uint8_t *rgb888, size_t bytes);

/* Additive — emits an audio chunk via CMD_AUDIO (0x04). `frames` counts
 * left/right pairs, like RetroArch's `struct record_audio_data`;
 * `channels` is 1 or 2. 3-byte header (cmd | size u16 LE) followed by the
 * MTU-chunked payload with no opcode prefix, like the BLIT calls.
 * Refuses (-1) a payload larger than 65535 bytes — the wire size field is
 * a u16 — rather than silently truncating it.
 * IN A PADDED SESSION (gm_set_padding): a chunk under 256 bytes is HELD
 * BACK and prepended to the next chunk — the return value is then 0 even
 * though NOTHING went out on the wire; and a single call can emit TWO
 * CMD_AUDIO packets (the held-back chunk plus one beyond 65535 bytes). The
 * sample order is always preserved. The number of CMD_AUDIO packets on the
 * wire is therefore no longer the number of successful calls. */
int        gm_send_audio(gm_handle *h, const int16_t *pcm, size_t frames,
                         uint8_t channels);
int        gm_recv_status(gm_handle *h, gm_status *out, int timeout_ms);
int        gm_send_close(gm_handle *h);
void       gm_close(gm_handle *h);

/* ---------------------------------------------------------------------------
 * Input channel (UDP, default video port + 1 = 32101). Direction REVERSED
 * compared to video: it is the RECEIVER that emits the stream, and the
 * emitter that initiates the handshake by sending a single byte.
 *
 * Deliberate departure from an earlier design note, documented here: that
 * note proposed gm_bind_inputs(gm_handle *h, const char *receiver_ip,
 * uint16_t port). The receiver address is ALREADY resolved into h->to by
 * gm_init; passing it again as an argument would invite exactly the kind
 * of divergence this project has already paid for elsewhere (two sources
 * for one fact). The signature adopted here derives the address from h->to
 * and only takes the port, with 0 meaning "video port + 1".
 * ------------------------------------------------------------------------- */
int gm_bind_inputs(gm_handle *h, uint16_t port);   /* port == 0 -> video port + 1 */

/* Resends the handshake byte ON THE SOCKET ALREADY OPEN — never a new one.
 * That is the key difference from gm_bind_inputs, which closes and
 * reopens: the source port is what the peer remembers as the return
 * address, so resending from a DIFFERENT socket would make it learn an
 * address the data will never come from. Touches neither input_to, nor
 * last_joy/last_ps2, nor the rejection counters.
 * Returns 0 if the byte went out, -1 if the channel is not bound or h is
 * NULL. */
int gm_send_input_hello(gm_handle *h);

/* Resets the input SEQUENCE GUARD -- last_joy.frame/order and
 * last_ps2.frame/order -- WITHOUT closing or reopening the socket, unlike
 * gm_bind_inputs. Serves the symmetric case: here it is not the receiver
 * that restarts but the PEER, which comes back on the same address with a
 * counter that restarted near zero.
 *
 * DELIBERATELY NARROWER than gm_bind_inputs' reset:
 *   - NOT in_stale / in_rejected_len / in_rejected_src -- these counters
 *     are the closing-time oracle (gm_close_inputs) and must stay
 *     cumulative over the whole session, otherwise a future regression
 *     would hide behind an artificial reset;
 *   - NOT has_joy / has_ps2 nor last_joy's buttons and axes -- a
 *     re-announce can happen mid-game (see groovy_reannounce.h's budget)
 *     and must not release any button that is being held.
 *
 * CALL ORDER IS PART OF THE CONTRACT: call BEFORE gm_send_input_hello,
 * never after. The peer cannot emit its first datagram before receiving
 * that byte (the software receiver's input code waits for it), so
 * reset-then-hello leaves no window where a datagram from the new peer
 * would arrive with the guard still armed.
 *
 * DO NOT ASSUME: the counter compared here (input channel, 32101, the
 * remote core's turn counter) and the one the re-announce decision
 * compares (video channel, 32100, the software receiver's ack_frame) are
 * TWO distinct variables. They never coincide on any value; they only
 * restart for the same cause -- the peer process restarting.
 *
 * Returns 0 if the channel is bound, -1 if h is NULL or the channel is not
 * bound -- same convention as gm_send_input_hello. */
int gm_input_reset_peer_guard(gm_handle *h);

int gm_poll_inputs(gm_handle *h);                  /* non-blocking; returns the number of ACCEPTED datagrams, -1 on error */
int gm_get_joy_inputs(gm_handle *h, gm_joy_inputs *out);   /* 0 if a state is available, -1 otherwise */
int gm_get_ps2_inputs(gm_handle *h, gm_ps2_inputs *out);
/* Mouse deltas accumulated since the last call, ALREADY DECODED into
 * signed values (GroovyMAME's formula, antonioginer/GroovyMAME,
 * BSD-3-Clause, input_mister.cpp:697-698: byte - 256 when mouse_bits' sign
 * bit is set -- bit 4 for x, bit 5 for y; dz is an 8-bit two's complement
 * with no sign bit in mouse_bits). gm_ps2_inputs' raw bytes remain exposed
 * as-is by gm_get_ps2_inputs, but they only carry the LAST datagram: only
 * these accumulators survive several datagrams drained within the same
 * frame. CONSUME-AND-RESET: two successive calls return 0 on the second.
 * NULL pointers accepted (field ignored). 0 or -1 (h NULL). */
int gm_get_mouse_deltas(gm_handle *h, int *dx, int *dy, int *dz);
void gm_close_inputs(gm_handle *h);
uint32_t gm_last_ack_frame(const gm_handle *h);    /* RECEIVER's frame counter, last acknowledgement received; 0 if none */

/* Source address of the last status received. Returns 1 if set, 0
 * otherwise. out_ip in NETWORK order, out_port in HOST order. gm_status
 * remains 13 bytes. */
int gm_last_status_peer(const gm_handle *h, uint32_t *out_ip, uint16_t *out_port);

/* Address ACTUALLY targeted by this handle (h->to), as gm_init resolved it
 * via getaddrinfo -- a hostname or a dotted-decimal IPv4 address. Earlier,
 * gm_init read this address via inet_addr, which accepted octal,
 * hexadecimal and short forms; that is no longer the case. Returns 1 if
 * set, 0 if h is NULL. out_ip in NETWORK order, out_port in HOST order,
 * like gm_last_status_peer. */
int gm_peer_addr(const gm_handle *h, uint32_t *out_ip, uint16_t *out_port);

/* Outcome of the last gm_send_init or gm_send_switchres call on THIS
 * handle -- and nothing else: blits and audio do not touch it.
 *   0                    datagram accepted by Winsock (which does NOT
 *                        prove it left the machine, nor that it arrived);
 *   > 0                  Winsock code read at the point of failure (10049
 *                        WSAEADDRNOTAVAIL, 10054 WSAECONNRESET, 10055
 *                        WSAENOBUFS...);
 *   GM_SEND_ERR_REFUSED  refused before Winsock (invalid handle or
 *                        arguments);
 *   GM_SEND_ERR_SHORT    bytes sent differs from the payload size.
 * h NULL -> GM_SEND_ERR_REFUSED. Read this right after the call you want
 * to judge: the next announce ON THE SAME handle overwrites it; one from
 * another handle does not. */
#define GM_SEND_ERR_REFUSED (-1)
#define GM_SEND_ERR_SHORT   (-2)
int gm_last_announce_error(const gm_handle *h);

/* State of the wire-offload send path on THIS handle. Returns 1 AS LONG AS
 * the segment held by the socket -- per the last successful reread;
 * failing that, the last ACCEPTED segment (when nothing can be reread,
 * libgm cannot rule out that the accepted segment is still there) --
 * equals the current MTU AND the split-send path has not been cut
 * mid-session (Windows, UDP_SEND_MSG_SIZE); 0 otherwise:
 *   - GROOVY_USO=off or 0 (split-send never requested);
 *   - the system refused it AT OPEN TIME (setsockopt refused, or accepted
 *     but misread on reread);
 *   - CUT MID-SESSION: the super-send was refused (WSAEMSGSIZE, WSAEINVAL,
 *     WSAEOPNOTSUPP, WSAENOPROTOOPT...), or the segment was refused by a
 *     later gm_set_mtu call;
 *   - outside Windows.
 * -1 if h is NULL. See gm_send_offload_stats for the held segment (in the
 * sense above) and the code that cut it. *out_sndbuf = send queue
 * (SO_SNDBUF) reread at open time -- set in both cases, independently of
 * split-send. The wire is the same in both cases: this accessor says
 * nothing about the datagrams' content, only about the path that produced
 * them. */
int gm_send_offload_state(const gm_handle *h, unsigned *out_sndbuf);

/* Counters for the wire-offload send path on THIS handle. Returns the
 * same value as gm_send_offload_state (1, 0, or -1 if h is NULL). Each
 * pointer may be NULL.
 *   *out_seg            segment held by the socket, per the last
 *                       successful reread; failing that, the last
 *                       ACCEPTED segment (see gm_send_offload_state);
 *                       0 = none;
 *   *out_super_sends    super-sends accepted in full since open -- proof
 *                       that the split-send path was used;
 *   *out_transient_errs super-sends that failed transiently (WSAENOBUFS,
 *                       lost route...): the frame is lost just like with
 *                       the old loop, but split-send stays active;
 *   *out_cut_err        0 if split-send was never cut mid-session,
 *                       otherwise the code that cut it (a refusal that
 *                       proves the option is not held: WSAEINVAL,
 *                       WSAEOPNOTSUPP, WSAENOPROTOOPT, WSAEMSGSIZE, or
 *                       GM_SEND_ERR_SHORT). */
int gm_send_offload_stats(const gm_handle *h, unsigned *out_seg, uint32_t *out_super_sends,
                          uint32_t *out_transient_errs, int *out_cut_err);

/* Optional heartbeat helper. Spawns a background thread that emits
 * CMD_GET_STATUS every interval_ms while idle. interval_ms = 0 → use 100 ms
 * default. Caller must call gm_heartbeat_stop before gm_close. */
int        gm_heartbeat_start(gm_handle *h, unsigned interval_ms);
int        gm_heartbeat_stop(gm_handle *h);

#endif /* GM_H */
