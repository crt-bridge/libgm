/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_test_bridge.c — minimal C test bridge exposing both modeline
 * computation AND ACK-pacing state machine
 * to Python ctypes for unit testing without RetroArch in the loop.
 *
 * Pacing test support:
 *   - Mock clock: define GROOVY_PACING_MONO_NS_OVERRIDE before groovy_pacing.h
 *     so tests can advance time deterministically.
 *   - Mock log hook: GROOVY_PACING_LOG_HOOK captures log events into a counter
 *     instead of writing to stderr.
 *   - Exported test_pacing_* entry points for ctypes.
 *
 * Strategy: instead of reaching into retroarch.exe to invoke the vtable symbol
 * (brittle — symbol visibility is not guaranteed on Windows DLLs), this bridge
 * exposes standalone C entry points that exercise:
 *   modelines: compute_modeline_from_av_info + gm_send_switchres
 *   pacing:    groovy_ack_state_t + compute_vsync_target_from_acks_post_poll
 *
 * Build: `make -C libgm groovy_test_bridge.dll` (requires MinGW-w64).
 *   The Makefile adds this target behind GROOVY_BRIDGE_BIN in `all`.
 *
 * Include path requirements:
 *   - gm.h: from include/ (CFLAGS -Iinclude already set in the Makefile)
 *   - groovy_modeline.h, groovy_pacing.h and the other groovy_*.h: the
 *     RetroArch driver's headers, outside this repository (BRIDGE_CFLAGS)
 *
 * NOTE: groovy_modeline.h guards against inclusion before gm.h with
 *   #error if GM_H is not defined. This TU includes gm.h first.
 *   groovy_pacing.h has the same guard.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* libgm public API — defines gm_modeline, gm_status, gm_handle, gm_init,
 * gm_send_switchres, gm_recv_status, gm_close. MUST come before pacing header. */
#include "gm.h"

/* ── Mock clock for deterministic pacing tests ──────────────────────────
 * Provide our own groovy_pacing_mono_ns() backed by a test-controlled variable
 * BEFORE including groovy_pacing.h so the #ifndef guard picks it up. */
static uint64_t mock_mono_ns_g = 0u;
static inline uint64_t groovy_pacing_mono_ns(void) { return mock_mono_ns_g; }
#define GROOVY_PACING_MONO_NS_OVERRIDE 1

/* ── Mock log hook — counts ack_lost events instead of printing ──────────── */
static int log_ack_lost_count_g = 0;
static void groovy_pacing_log_ack_lost_test(const char *evt) {
    (void)evt;
    log_ack_lost_count_g++;
}
#define GROOVY_PACING_LOG_HOOK groovy_pacing_log_ack_lost_test

/* Clock servo -- reuses the SAME mocked clock (mock_mono_ns_g) as the
 * pacing above (groovy_pacing.h must not have a second one). Log counter
 * DISTINCT from log_ack_lost_count_g ([groovy-servo] is a separate log
 * family from [groovy_pacing]/[groovy-lat]). */
static inline uint64_t groovy_servo_mono_ns(void) { return mock_mono_ns_g; }
#define GROOVY_SERVO_MONO_NS_OVERRIDE 1

static int log_servo_count_g = 0;
static void groovy_servo_log_test(const char *evt) {
    (void)evt;
    log_servo_count_g++;
}
#define GROOVY_SERVO_LOG_HOOK groovy_servo_log_test

/* ── Include shared headers (after overrides) ─────────────────────────────── */

/* Single 240p/480i regime decision, shared with the driver. Must precede
 * groovy_modeline.h: compute_modeline_from_dims now calls
 * groovy_mode_classify to derive v_total and interlace from the same rule
 * as the regime decision. */
#include "groovy_mode.h"

/* Shared modeline computation (must follow gm.h per header guard). */
#include "groovy_modeline.h"

/* ACK-pacing state machine (must follow gm.h per header guard and AFTER the
 * GROOVY_PACING_MONO_NS_OVERRIDE define above). */
#include "groovy_pacing.h"

/* Clock servo: per-mode feed-forward, ring of send timestamps, sub-frame
 * error, PI bounded on the residual. Depends on groovy_pacing.h
 * (groovy_ack_state_t), so included right after. */
#include "groovy_servo.h"

/* Emitter audio logic (step 2): CMD_INIT frequency code, GROOVY_AUDIO
 * setting read, linear resampler. No dependency on gm.h or RetroArch --
 * included here to be exercised outside the runloop, exactly like the
 * code record_groovy.c runs. */
#include "groovy_audio.h"

/* Emitter compression setting: reads GROOVY_COMPRESSION. Same pattern as
 * groovy_audio.h above. */
#include "groovy_compression.h"

/* The generic CPU box filter for supersampling: reads GROOVY_DOWNSAMPLE
 * and averages n*n blocks. Same pattern as groovy_compression.h above. */
#include "groovy_downsample.h"

/* The three input converters: dispatch, direct 1:1 path, and reference
 * box filter. Same pattern. */
#include "groovy_convert.h"

/* Per-session send MTU: reads GROOVY_MTU. Same pattern as
 * groovy_compression.h above. */
#include "groovy_mtu.h"
/* Input channel setting and latency histogram: reads GROOVY_INPUT,
 * measures k. Same pattern as groovy_audio.h and groovy_compression.h. */
#include "groovy_input.h"

/* Numbered frame witness: reads GROOVY_DUMP_FRAME_INTERVAL /
 * GROOVY_DUMP_FRAME_SEQ, renders the name. Same pattern as
 * groovy_compression.h above. */
#include "groovy_dump.h"

/* Diagnostic hook: reads GROOVY_SKIP_FRAME_EVERY and the frame-skip
 * decision. Same pattern as groovy_dump.h above (no dependency on
 * gm.h). */
#include "groovy_skip.h"

/* Lever 1 switch: reads GROOVY_FRAME_DUP. Same pattern as
 * groovy_compression.h above -- no dependency on gm.h, the boolean
 * crosses the boundary via gm_set_frame_dup. */
#include "groovy_frame_dup.h"

/* Lever b6's switch and hold buffer: reads GROOVY_AUDIO_HOLD. No
 * dependency on gm.h: this lever has no wire counterpart, only the send
 * instant changes. */
#include "groovy_audio_hold.h"

/* field(n) formula and lever 2 switch: reads GROOVY_FIELD_PER_FRAME.
 * Depends on groovy_pacing.h (already included above) for
 * groovy_ack_state_t. */
#include "groovy_field.h"

/* GROOVY_FOLLOWERS parser, provenance filter, follower saturation and
 * input merge: depends on gm.h (gm_joy_inputs, gm_ps2_inputs,
 * GM_MTU_*), already included above. */
#include "groovy_followers.h"

/* Pure re-announce decision (CMD_INIT+CMD_SWITCHRES): reads
 * GROOVY_REANNOUNCE and the groovy_reannounce_should_fire decision. Same
 * pattern as groovy_skip.h above (no dependency on gm.h). */
#include "groovy_reannounce.h"

/* Menu/variable precedence: pure decision and forced-message
 * construction. Same pattern as groovy_reannounce.h above (no dependency
 * on gm.h). */
#include "groovy_menu_precedence.h"

/* Composition of the four "Additional Follower N" switches. Same pattern
 * as groovy_menu_precedence.h above (no dependency on gm.h). */
#include "groovy_followers_menu.h"

/* ---------------------------------------------------------------------------
 * Export macro — dllexport on Windows, default visibility elsewhere (Linux CI).
 * ------------------------------------------------------------------------- */
#ifdef _WIN32
#  define DLLEXPORT __declspec(dllexport)
#else
#  define DLLEXPORT __attribute__((visibility("default")))
#endif

/* ---------------------------------------------------------------------------
 * groovy_av_cache_t lives in groovy_mode.h (shared with the runtime, no
 * local copy). g_mode_state carries the hysteresis/hint/guard state
 * (struct groovy_mode_state, also groovy_mode.h).
 *
 * g_av_cache: mirrors groovy_state_t::av_cache in record_groovy.c.
 * Declared file-scope so test bridge state persists across calls within the
 * same DLL load, matching the record_groovy.c runtime behavior (driver
 * instance lifetime = process lifetime for a given session).
 *
 * g_last_valid_fps: mirrors groovy_state_t::last_valid_fps.
 * Caches the last fps > 0 seen, allowing SET_GEOMETRY-only callbacks to
 * reuse it rather than producing a zero-fps modeline.
 *
 * Test isolation: call test_groovy_reset_av_cache() at the start of each
 * independent test scenario to zero all three. An autouse pytest fixture
 * in the emitter's test suite handles this automatically.
 *
 * Why: g_av_cache is process-global. Two tests with the same
 * (w, h, fps) tuple but different ports collided — test #1 populated the
 * cache, test #2 hit dedupe and emitted 0 packets. The reset entry point
 * (added below) resolves this by clearing state between tests. */
static groovy_av_cache_t        g_av_cache        = { 0, 0, 0.0, false };
static double                   g_last_valid_fps  = 0.0;
static struct groovy_mode_state g_mode_state;

/* test_groovy_reset_av_cache — exported reset entry point for Python ctypes.
 *
 * Zeroes g_av_cache, g_last_valid_fps, g_mode_state, AND the mock monotonic
 * clock (the guard's switch_ns[]/last_request_ns entries would otherwise
 * leak a stale "now" across scenarios that don't call
 * test_pacing_set_mock_time_ns themselves). Must be called (or invoked via
 * the autouse pytest fixture) before each independent test scenario. */
DLLEXPORT void test_groovy_reset_av_cache(void)
{
    memset(&g_av_cache, 0, sizeof(g_av_cache));
    g_last_valid_fps = 0.0;
    memset(&g_mode_state, 0, sizeof(g_mode_state));
    mock_mono_ns_g = 0u;
}

/* ---------------------------------------------------------------------------
 * test_groovy_push_av_info — exported entry point for Python ctypes.
 *
 * Mirrors record_groovy.c::groovy_push_av_info: the core's declaration is
 * only a HINT for groovy_mode_on_frame,
 * it announces NOTHING by itself. It never touches the cache, never emits a
 * CMD_SWITCHRES, never requests a regime switch — see groovy_mode.h.
 *   - Returns 0 normally.
 *   - Returns -1 on defensive rejection (w=0 or h=0).
 * ------------------------------------------------------------------------- */
DLLEXPORT int test_groovy_push_av_info(
        const char *ip,
        uint16_t    port,
        unsigned    w,
        unsigned    h,
        double      fps)
{
    (void)ip;
    (void)port;

    /* Defensive rejection — reject zero geometry (non-recoverable) */
    if (w == 0u || h == 0u)
        return -1;

    /* fps cache: unchanged from the earlier behaviour (mirrors
     * groovy_state_t::last_valid_fps in record_groovy.c). */
    if (fps > 0.0)
        g_last_valid_fps = fps;

    groovy_mode_on_declare(&g_mode_state, w, h);
    return 0;
}

/* ---------------------------------------------------------------------------
 * test_groovy_push_frame — exported entry point for Python ctypes. Mirrors
 * the frame path of the future record_groovy.c::push_video: decision, then
 * modeline on the cache, then send — in that
 * order, before the blit. This is the ONLY path that may emit a
 * CMD_SWITCHRES; test_groovy_push_av_info above never does.
 *   - Returns 1 (KEEP): nothing changed, nothing sent.
 *   - Returns 2 (DROP_FROZEN): the guard refused this regime switch.
 *   - Returns 0: cache updated and CMD_SWITCHRES sent successfully.
 *   - Returns -2: libgm error (gm_init or gm_send_switchres failed).
 * ------------------------------------------------------------------------- */
DLLEXPORT int test_groovy_push_frame(
        const char *ip,
        uint16_t    port,
        unsigned    w,
        unsigned    h,
        double      fps)
{
    /* Same defensive rejection, extended here: reject zero
     * geometry before it ever reaches groovy_mode_on_frame. groovy_mode.h's
     * decision is deliberately silent on this -- it is a defensive
     * wire-emission concern (never send a degenerate 0x0 CMD_SWITCHRES),
     * not a regime-classification concern, so the guard lives here rather
     * than in the shared header. */
    if (w == 0u || h == 0u)
        return -1;

    double effective_fps = (fps > 0.0) ? fps
                          : (g_last_valid_fps > 0.0) ? g_last_valid_fps
                          : -1.0;

    enum groovy_mode_action a = groovy_mode_on_frame(
            &g_mode_state, &g_av_cache, w, h, effective_fps, groovy_pacing_mono_ns());

    if (a == GROOVY_MODE_KEEP)
        return 1;
    if (a == GROOVY_MODE_DROP_FROZEN)
        return 2;

    /* a == GROOVY_MODE_ANNOUNCE: same pipeline as push_video —
     * modeline computed on the cache groovy_mode_on_frame just wrote, then
     * sent. */
    gm_modeline mode;
    compute_modeline_from_dims(g_av_cache.width, g_av_cache.height, g_av_cache.fps, &mode);

    gm_handle *gm = gm_init(ip, port);
    if (!gm)
        return -2;

    int rc = gm_send_switchres(gm, &mode);
    gm_close(gm);

    return (rc == 0) ? 0 : -2;
}

/* ---------------------------------------------------------------------------
 * Thin, logic-free exports over groovy_mode.h — one line each, calling the
 * same functions the driver will call. The emitter's test suite holds the
 * sequence table these entry points are required to satisfy.
 * ------------------------------------------------------------------------- */
DLLEXPORT int test_mode_classify(unsigned h)
{
    return (int)groovy_mode_classify(h);
}

DLLEXPORT void test_mode_on_declare(unsigned w, unsigned h)
{
    groovy_mode_on_declare(&g_mode_state, w, h);
}

DLLEXPORT int test_mode_on_frame(unsigned w, unsigned h, double fps, uint64_t now_ns)
{
    return (int)groovy_mode_on_frame(&g_mode_state, &g_av_cache, w, h, fps, now_ns);
}

DLLEXPORT int test_mode_frame_matches(unsigned w, unsigned h, int action)
{
    return groovy_mode_frame_matches(&g_mode_state, &g_av_cache, w, h,
            (enum groovy_mode_action)action) ? 1 : 0;
}

DLLEXPORT int test_mode_of(void)
{
    return (int)groovy_mode_of(&g_av_cache);
}

DLLEXPORT int test_mode_last_reason(void)
{
    return (int)g_mode_state.last_reason;
}

DLLEXPORT unsigned test_mode_cache_width(void)
{
    return g_av_cache.width;
}

DLLEXPORT unsigned test_mode_cache_height(void)
{
    return g_av_cache.height;
}

DLLEXPORT int test_mode_frozen(void)
{
    return g_mode_state.frozen ? 1 : 0;
}

/* which: 0 switches, 1 freezes, 2 frozen_drops, 3 dims_drops. */
DLLEXPORT uint64_t test_mode_counter(int which)
{
    switch (which) {
    case 0: return g_mode_state.switches;
    case 1: return (uint64_t)g_mode_state.freezes;
    case 2: return g_mode_state.frozen_drops;
    case 3: return g_mode_state.dims_drops;
    default: return 0u;
    }
}

DLLEXPORT void test_modeline_from_dims(unsigned w, unsigned h, double fps,
        unsigned *v_total, unsigned *h_total, unsigned *interlace)
{
    gm_modeline mode;
    compute_modeline_from_dims(w, h, fps, &mode);
    if (v_total)   *v_total   = mode.v_total;
    if (h_total)   *h_total   = mode.h_total;
    if (interlace) *interlace = mode.interlace;
}

/* ---------------------------------------------------------------------------
 * ACK-pacing test entry points
 *
 * These functions expose the groovy_ack_state_t state machine to Python ctypes.
 * The test controls time via test_pacing_set_mock_time_ns and injects synthetic
 * ACKs via test_pacing_inject_ack, then calls test_pacing_compute_post_poll to
 * observe the freshness decision — all without a real network socket.
 * ------------------------------------------------------------------------- */

/* test_pacing_reset — zero-initialise a groovy_ack_state_t and reset log counter.
 *
 * Must be called at the start of each pacing test to ensure isolation. */
DLLEXPORT void test_pacing_reset(groovy_ack_state_t *st)
{
    memset(st, 0, sizeof(*st));
    log_ack_lost_count_g = 0;
}

/* test_pacing_set_mock_time_ns — advance the mock monotonic clock.
 *
 * Tests set the current time here; groovy_pacing_mono_ns() returns this value.
 * The mock clock does NOT advance automatically — each test controls it. */
DLLEXPORT void test_pacing_set_mock_time_ns(uint64_t ns)
{
    mock_mono_ns_g = ns;
}

/* test_pacing_inject_ack — synthetically inject a received ACK into state.
 *
 * Simulates what happens inside compute_vsync_target_from_acks when
 * gm_recv_status returns 0 (success). Records the current mock time as the
 * ACK timestamp and sets has_any_ack=1. The ack_lost_logged flag is cleared,
 * mimicking the reset that happens when a new ACK arrives after a timeout. */
DLLEXPORT void test_pacing_inject_ack(groovy_ack_state_t *st, uint16_t vcount)
{
    st->last_vcount      = vcount;
    st->last_ack_mono_ns = mock_mono_ns_g;
    st->ack_lost_logged  = 0;
    st->has_any_ack      = 1;
}

/* test_pacing_compute_post_poll — invoke the post-poll freshness decision.
 *
 * This exercises compute_vsync_target_from_acks_post_poll directly, skipping
 * the gm_recv_status call (which would require a real socket). Tests use
 * test_pacing_inject_ack to pre-populate state instead of real socket I/O. */
DLLEXPORT uint16_t test_pacing_compute_post_poll(groovy_ack_state_t *st)
{
    return compute_vsync_target_from_acks_post_poll(st);
}

/* test_pacing_log_count — return the number of ack_lost log events fired.
 *
 * Test 5 asserts this returns 1 after 2.1 s with no new ACKs (single event).
 * Test 6 asserts it resets to 0 when a new ACK arrives (ack_lost_logged flag
 * cleared by test_pacing_inject_ack, consistent with runtime behaviour). */
DLLEXPORT int test_pacing_log_count(void)
{
    return log_ack_lost_count_g;
}

/* test_pacing_state_size — sizeof(groovy_ack_state_t) on the C side. The
 * ctypes mirror on the Python side MUST cover the whole structure:
 * test_pacing_reset does a memset over this sizeof, so any field missing
 * on the Python side is an out-of-buffer write. This getter makes the
 * next divergence loud instead of silent. */
DLLEXPORT size_t test_pacing_state_size(void)
{
    return sizeof(groovy_ack_state_t);
}


/* ---------------------------------------------------------------------------
 * Step 2 (audio to the receiver): groovy_audio.h logic.
 *
 * These entry points call the SAME code as record_groovy.c -- the header
 * is included, not copied. What is green here is true in the driver.
 * ------------------------------------------------------------------------- */

/* Reads the GROOVY_AUDIO setting. 0 = off, 1 = receiver, 2 = both. */
DLLEXPORT int test_audio_mode_parse(const char *s)
{
    return (int)groovy_audio_mode_parse(s);
}

/* Reads the GROOVY_COMPRESSION setting. 0 = off, 1 = lz4. */
DLLEXPORT int test_compression_mode_parse(const char *s)
{
    return (int)groovy_compression_mode_parse(s);
}

/* ---------------------------------------------------------------------------
 * Supersampling CPU box filter. These entry points call the SAME code as
 * record_groovy.c -- the header is included, not copied.
 * ------------------------------------------------------------------------- */
DLLEXPORT unsigned test_downsample_factor_parse(const char *s)
{
    return groovy_downsample_factor_parse(s);
}

DLLEXPORT int test_downsample_dims_ok(unsigned w, unsigned h, unsigned n)
{
    return groovy_downsample_dims_ok(w, h, n);
}

DLLEXPORT void test_downsample_box(uint8_t *dst, const uint8_t *src,
                                   unsigned src_w, unsigned src_h, unsigned n)
{
    groovy_downsample_box_rgb888(dst, src, src_w, src_h, n);
}

/* Identity-test reference: test_downsample_box above calls the dispatch
 * (the code record_groovy.c runs for 2/4/8), this entry point calls the
 * generic version directly, kept as the reference. */
DLLEXPORT void test_downsample_box_generic(uint8_t *dst, const uint8_t *src,
                                           unsigned src_w, unsigned src_h, unsigned n)
{
    groovy_downsample_box_rgb888_generic(dst, src, src_w, src_h, n);
}

/* The input converters from groovy_convert.h. fmt: 0 BGR24, 1 XRGB8888,
 * 2 RGB565. path: 0 dispatch (what record_groovy.c calls), 1 reference
 * box filter, 2 direct path (1:1 only). Returns -1 on an unknown format
 * or path, or a direct path requested outside 1:1. */
DLLEXPORT int test_convert(unsigned fmt, unsigned path, uint8_t *dst,
                           const uint8_t *src, unsigned dst_w, unsigned dst_h,
                           unsigned src_w, unsigned src_h, unsigned src_pitch)
{
    if (path == 2u && (dst_w != src_w || dst_h != src_h))
        return -1;
    switch (fmt * 3u + path) {
        case 0u: bgr24_to_rgb888(dst, src, dst_w, dst_h, src_w, src_h, src_pitch); break;
        case 1u: groovy_bgr24_to_rgb888_box(dst, src, dst_w, dst_h, src_w, src_h, src_pitch); break;
        case 2u: groovy_bgr24_to_rgb888_direct(dst, src, dst_w, dst_h, src_pitch); break;
        case 3u: xrgb8888_to_rgb888(dst, src, dst_w, dst_h, src_w, src_h, src_pitch); break;
        case 4u: groovy_xrgb8888_to_rgb888_box(dst, src, dst_w, dst_h, src_w, src_h, src_pitch); break;
        case 5u: groovy_xrgb8888_to_rgb888_direct(dst, src, dst_w, dst_h, src_pitch); break;
        case 6u: rgb565_to_rgb888(dst, src, dst_w, dst_h, src_w, src_h, src_pitch); break;
        case 7u: groovy_rgb565_to_rgb888_box(dst, src, dst_w, dst_h, src_w, src_h, src_pitch); break;
        case 8u: groovy_rgb565_to_rgb888_direct(dst, src, dst_w, dst_h, src_pitch); break;
        default: return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Per-session MTU. Same code as record_groovy.c -- the header is
 * included, not copied. The three constant entry points exist so the test
 * can ASSERT equality with gm.h instead of copying the numbers.
 * ------------------------------------------------------------------------- */
DLLEXPORT unsigned test_mtu_parse(const char *s) { return groovy_mtu_parse(s); }
DLLEXPORT unsigned test_gm_mtu_default(void)     { return GM_MTU_DEFAULT; }
DLLEXPORT unsigned test_gm_mtu_min(void)         { return GM_MTU_MIN; }
DLLEXPORT unsigned test_gm_mtu_max(void)         { return GM_MTU_MAX; }

/* CMD_INIT frequency code for a given native frequency. */
DLLEXPORT int test_audio_rate_code(double native_hz)
{
    return (int)groovy_audio_rate_code(native_hz);
}

/* Frequency in Hz carried by a code. */
DLLEXPORT unsigned test_audio_rate_hz(int code)
{
    return groovy_audio_rate_hz((uint8_t)code);
}

/* Upper bound on the number of pairs produced -- the test checks that the
 * real output never exceeds it. */
DLLEXPORT size_t test_audio_resample_capacity(size_t in_frames,
                                              unsigned src_hz, unsigned dst_hz)
{
    return groovy_resample_capacity(in_frames, src_hz, dst_hz);
}

/* Resampling of a sequence of blocks, with state carried from one block
 * to the next -- exactly what push_audio does call after call.
 *
 * `in` carries `nblocks` consecutive blocks of `block_frames` pairs. The
 * output is concatenated into `out`, and the total number of pairs
 * produced is returned. Overflowing `out_cap` returns (size_t)-1. */
DLLEXPORT size_t test_audio_resample_blocks(const int16_t *in,
                                            size_t block_frames,
                                            size_t nblocks,
                                            unsigned src_hz, unsigned dst_hz,
                                            int16_t *out, size_t out_cap)
{
    struct groovy_resampler rs;
    groovy_resample_reset(&rs, src_hz, dst_hz);

    size_t total = 0u;
    for (size_t b = 0u; b < nblocks; ++b) {
        if (total > out_cap)
            return (size_t)-1;
        size_t n = groovy_resample(&rs,
                                   in + b * block_frames * 2u, block_frames,
                                   out + total * 2u, out_cap - total);
        total += n;
    }
    return total;
}

/* ---------------------------------------------------------------------------
 * Fractional wire accounting for the resampler. Exports that give the
 * test direct access to the resampler's state -- struct groovy_resampler
 * crosses the Python/ctypes boundary by pointer, allocated on the Python
 * side over a mirror whose size is checked by test_resample_size. Same
 * code as record_groovy.c -- the header is included, not copied.
 * ------------------------------------------------------------------------- */

/* test_resample_size -- sizeof(struct groovy_resampler) on the C side.
 * The ctypes mirror on the Python side MUST cover the whole
 * structure, otherwise any field missing on the Python side is an
 * out-of-buffer write (same contract as test_pacing_state_size /
 * test_servo_state_size). */
DLLEXPORT size_t test_resample_size(void)
{
    return sizeof(struct groovy_resampler);
}

DLLEXPORT void test_resample_reset(struct groovy_resampler *r,
                                   unsigned src_hz, unsigned dst_hz)
{
    groovy_resample_reset(r, src_hz, dst_hz);
}

DLLEXPORT int test_resample_set_ratio(struct groovy_resampler *r, double ratio)
{
    return groovy_resample_set_ratio(r, ratio);
}

DLLEXPORT double test_resample_get_ratio(const struct groovy_resampler *r)
{
    return r ? r->ratio : 0.0;
}

DLLEXPORT size_t test_resample_capacity_ratio(size_t in_frames,
                                              unsigned src_hz, unsigned dst_hz,
                                              double ratio)
{
    return groovy_resample_capacity_ratio(in_frames, src_hz, dst_hz, ratio);
}

/* test_resample_run -- calls groovy_resample directly, leaving the Python
 * caller to manage the state (`r`) and the ratio between calls -- exactly
 * what the direction test and the test straddling a ratio change
 * mid-series need, unlike test_audio_resample_blocks, which allocates its
 * own internal state per call. */
DLLEXPORT size_t test_resample_run(struct groovy_resampler *r,
                                   const int16_t *in, size_t in_frames,
                                   int16_t *out, size_t out_cap)
{
    return groovy_resample(r, in, in_frames, out, out_cap);
}


/* ---------------------------------------------------------------------------
 * Input channel: GROOVY_INPUT setting and the k latency histogram. Same
 * code as record_groovy.c -- the header is included, not copied. What is
 * green here is true in the driver.
 * ------------------------------------------------------------------------- */

/* Test instance, as a file-scope variable of the bridge -- like
 * g_av_cache above. */
static struct groovy_input_lat g_input_lat;

/* Reads the GROOVY_INPUT setting. 0 = off, 1 = on. */
DLLEXPORT int test_groovy_input_mode_parse(const char *s)
{
    return (int)groovy_input_mode_parse(s);
}

DLLEXPORT void test_input_lat_reset(void)
{
    groovy_input_lat_reset(&g_input_lat);
}

DLLEXPORT void test_input_lat_record(unsigned k)
{
    groovy_input_lat_record(&g_input_lat, k);
}

DLLEXPORT void test_input_lat_percentiles(unsigned *mn, unsigned *p50,
                                          unsigned *p95, unsigned *mx)
{
    groovy_input_lat_percentiles(&g_input_lat, mn, p50, p95, mx);
}

/* ---------------------------------------------------------------------------
 * Numbered frame witness. Same code as record_groovy.c -- the header is
 * included, not copied. What is green here is true in the driver.
 * ------------------------------------------------------------------------- */
DLLEXPORT unsigned test_dump_interval_parse(const char *s)
{
    return groovy_dump_interval_parse(s);
}

DLLEXPORT int test_dump_seq_parse(const char *s)
{
    return groovy_dump_seq_parse(s);
}

DLLEXPORT int test_dump_name(char *dst, size_t cap, const char *path,
                             int seq, uint32_t counter)
{
    return groovy_dump_name(dst, cap, path, seq, counter);
}

/* ---------------------------------------------------------------------------
 * Diagnostic hook. Same code as record_groovy.c -- the header is
 * included, not copied. What is green here is true in the driver.
 * ------------------------------------------------------------------------- */
DLLEXPORT unsigned test_skip_every_parse(const char *s)
{
    return groovy_skip_every_parse(s);
}

DLLEXPORT int test_skip_should_skip(unsigned every, uint32_t frame_id)
{
    return groovy_skip_should_skip(every, frame_id);
}

/* ---------------------------------------------------------------------------
 * Lever 1 switch. Same code as record_groovy.c -- the header is
 * included, not copied. What is green here is true in the driver.
 * ------------------------------------------------------------------------- */
DLLEXPORT int test_frame_dup_mode_parse(const char *s)
{
    return (int)groovy_frame_dup_mode_parse(s);
}

DLLEXPORT const char *test_frame_dup_mode_name(int m)
{
    return groovy_frame_dup_mode_name((enum groovy_frame_dup_mode)m);
}

/* ---------------------------------------------------------------------------
 * Lever b6's switch and hold buffer. Same code as the future
 * record_groovy.c -- the header is included, not copied. What is green
 * here is true in the driver. A single test state per file: hold_g.
 * ------------------------------------------------------------------------- */
static struct groovy_audio_hold hold_g;

DLLEXPORT int test_audio_hold_mode_parse(const char *s)
{
    return (int)groovy_audio_hold_mode_parse(s);
}

DLLEXPORT const char *test_audio_hold_mode_name(int m)
{
    return groovy_audio_hold_mode_name((enum groovy_audio_hold_mode)m);
}

DLLEXPORT void test_audio_hold_release(void)
{
    groovy_audio_hold_release(&hold_g);
}

DLLEXPORT void test_audio_hold_clear(void)
{
    groovy_audio_hold_clear(&hold_g);
}

DLLEXPORT int test_audio_hold_deposit(const int16_t *pcm, size_t frames, unsigned channels)
{
    return groovy_audio_hold_deposit(&hold_g, pcm, frames, channels);
}

DLLEXPORT unsigned test_audio_hold_n_blocks(void)
{
    return hold_g.n_blocks;
}

DLLEXPORT size_t test_audio_hold_frames(void)
{
    return hold_g.frames;
}

DLLEXPORT size_t test_audio_hold_cap_frames(void)
{
    return hold_g.cap_frames;
}

DLLEXPORT unsigned long long test_audio_hold_lost(void)
{
    return (unsigned long long)hold_g.lost;
}

/* Copies at most out_cap_frames pairs from block i into out (ctypes-safe:
 * a raw pointer into the internal buffer would be more fragile, and the
 * copy proves the bytes). Returns the block's pair count (0 if i exceeds
 * n_blocks), not the count copied. */
DLLEXPORT size_t test_audio_hold_copy_block(unsigned i, int16_t *out, size_t out_cap_frames)
{
    size_t block_frames = 0u;
    const int16_t *p = groovy_audio_hold_block(&hold_g, i, &block_frames);
    size_t n;

    if (!p || !out || out_cap_frames == 0u)
        return block_frames;

    n = (block_frames < out_cap_frames) ? block_frames : out_cap_frames;
    memcpy(out, p, n * (size_t)hold_g.channels * sizeof(int16_t));
    return block_frames;
}

/* ---------------------------------------------------------------------------
 * field(n) formula and lever 2 switch. Same code as record_groovy.c --
 * the header is included, not copied. What is green here is true in the
 * driver.
 * ------------------------------------------------------------------------- */

/* test_pacing_inject_ack_ex — injects a complete synthetic ACK (vcount,
 * frame_echo, frame, flags), to test the field(n) formula without going
 * through test_pacing_inject_ack (which does not set
 * frame_echo/frame/flags). */
DLLEXPORT void test_pacing_inject_ack_ex(groovy_ack_state_t *st, uint16_t vcount,
                                         uint32_t frame_echo, uint32_t frame,
                                         uint8_t flags)
{
    st->last_vcount      = vcount;
    st->last_ack_mono_ns = mock_mono_ns_g;
    st->ack_lost_logged  = 0;
    st->has_any_ack      = 1;
    st->last_frame_echo  = frame_echo;
    st->last_frame       = frame;
    st->last_flags       = flags;
}

DLLEXPORT uint8_t test_field_for_frame(groovy_ack_state_t *st, uint32_t n)
{
    return groovy_field_for_frame(st, n);
}

DLLEXPORT int test_field_mode_parse(const char *s)
{
    return (int)groovy_field_mode_parse(s);
}

/* ---------------------------------------------------------------------------
 * Pure re-announce decision (CMD_INIT+CMD_SWITCHRES). Same code as the
 * future driver -- the header is included, not copied. What is green
 * here is true in the driver.
 * ------------------------------------------------------------------------- */
DLLEXPORT int test_reannounce_parse(const char *s)
{ return groovy_reannounce_parse(s); }

DLLEXPORT int test_reannounce_should_fire(struct groovy_reannounce_state *st,
                                          uint32_t peer_ip, uint32_t frame,
                                          uint32_t frame_echo, uint64_t now_ms)
{ return groovy_reannounce_should_fire(st, peer_ip, frame, frame_echo, now_ms); }

DLLEXPORT unsigned test_reannounce_state_size(void)
{ return (unsigned)sizeof(struct groovy_reannounce_state); }

DLLEXPORT unsigned test_reannounce_spacing_parse(const char *s)
{ return groovy_reannounce_spacing_parse(s); }

DLLEXPORT int test_reannounce_spacing_due(uint64_t t_init_ns, unsigned spacing_us, uint64_t now_ns)
{ return groovy_reannounce_spacing_due(t_init_ns, spacing_us, now_ns); }

/* Re-announce budget replenishment over time. Three constants exported so
 * the test does not have to copy their values -- what is green here is
 * true in the driver. */
DLLEXPORT unsigned test_reannounce_replenish_ms(void) { return (unsigned)GROOVY_REANNOUNCE_REPLENISH_MS; }
DLLEXPORT unsigned test_reannounce_retry_ms(void)     { return (unsigned)GROOVY_REANNOUNCE_RETRY_MS; }
DLLEXPORT unsigned test_reannounce_burst_max(void)    { return (unsigned)GROOVY_REANNOUNCE_BURST_MAX; }

/* ---------------------------------------------------------------------------
 * GROOVY_FOLLOWERS parser, provenance filter, follower saturation, input
 * merge. Same code as the future driver -- the header is included, not
 * copied. What is green here is true in the driver.
 * ------------------------------------------------------------------------- */
DLLEXPORT int test_followers_parse(const char *s,
                                   uint32_t master_ip, uint16_t master_port,
                                   int master_compression, int master_shape,
                                   struct groovy_follower_config *out, unsigned cap,
                                   unsigned *n_out, char *err, size_t err_cap)
{
    return groovy_followers_parse(s, master_ip, master_port, master_compression,
                                  master_shape, out, cap, n_out, err, err_cap);
}

DLLEXPORT int test_followers_ip_parse(const char *s, uint32_t *out)
{
    return groovy_followers_ip_parse(s, s ? strlen(s) : 0u, out);
}

DLLEXPORT unsigned test_followers_config_size(void)
{ return (unsigned)sizeof(struct groovy_follower_config); }

DLLEXPORT unsigned test_followers_max(void)     { return GROOVY_FOLLOWERS_MAX; }
DLLEXPORT unsigned test_followers_len_max(void) { return GROOVY_FOLLOWERS_LEN_MAX; }

DLLEXPORT int test_status_from_declared(uint32_t peer_ip, uint16_t peer_port,
                                        uint32_t declared_ip, uint16_t declared_port)
{
    return groovy_status_from_declared(peer_ip, peer_port, declared_ip, declared_port);
}

DLLEXPORT int test_sat_should_skip(struct groovy_sat_state *s, int synced,
                                   uint32_t last_echo, uint32_t frame_id)
{
    return groovy_sat_should_skip(s, synced, last_echo, frame_id);
}

/* The ring of frames SENT to a follower, and the count of those it has
 * not yet acknowledged. Same code as record_groovy.c. */
DLLEXPORT void test_sat_note_sent(struct groovy_sat_state *s, uint32_t frame_id)
{ groovy_sat_note_sent(s, frame_id); }

DLLEXPORT unsigned test_sat_in_flight(const struct groovy_sat_state *s, uint32_t last_echo)
{ return (unsigned)groovy_sat_in_flight(s, last_echo); }

DLLEXPORT unsigned test_sat_ring(void) { return GROOVY_SAT_RING; }

DLLEXPORT unsigned test_sat_state_size(void)
{ return (unsigned)sizeof(struct groovy_sat_state); }

DLLEXPORT int test_axis_merge(int8_t cur, int8_t add)
{ return (int)groovy_axis_merge(cur, add); }

DLLEXPORT void test_input_fold_joy(gm_joy_inputs *acc, const gm_joy_inputs *src, int inputs)
{ groovy_input_fold_joy(acc, src, inputs); }

DLLEXPORT int test_input_fold_ps2(gm_ps2_inputs *acc, const gm_ps2_inputs *src, int inputs)
{ return groovy_input_fold_ps2(acc, src, inputs); }

/* The master's mouse alone, in a pure function that
 * groovy_input_take_mouse calls. Same code as record_groovy.c. */
DLLEXPORT int test_input_fold_mouse(int *sx, int *sy, int *sz,
                                    int dx, int dy, int dz, int inputs)
{ return groovy_input_fold_mouse(sx, sy, sz, dx, dy, dz, inputs); }

/* Aging of a follower's controller (500 ms). Same code as
 * record_groovy.c. */
DLLEXPORT int test_input_is_live(unsigned idx, uint64_t now_ns, uint64_t last_accept_ns)
{ return groovy_input_is_live(idx, now_ns, last_accept_ns); }

DLLEXPORT uint64_t test_input_stale_ns(void)
{ return (uint64_t)GROOVY_INPUT_STALE_NS; }

/* What groovy_input_take_joy returns to the controller driver -- -1 with
 * no channel hooked at all, otherwise 0 (the merge, or a released
 * state). Same code as record_groovy.c. */
DLLEXPORT int test_input_take_joy_rc(int any_hooked, int have_any)
{ return groovy_input_take_joy_rc(any_hooked, have_any); }

/* ---------------------------------------------------------------------------
 * Per-receiver ack_back/ack_lost and latency-sample classification. Same
 * code as record_groovy.c -- the header is included, not copied. What is
 * green here is true in the driver.
 * ------------------------------------------------------------------------- */
DLLEXPORT int test_ack_link_on_status(struct groovy_ack_link *l, uint32_t frame_echo)
{ return groovy_ack_link_on_status(l, frame_echo); }

DLLEXPORT int test_ack_link_on_tick(struct groovy_ack_link *l, groovy_ack_state_t *ack,
                                    uint64_t now_ns)
{ return groovy_ack_link_on_tick(l, ack, now_ns); }

DLLEXPORT unsigned test_ack_link_size(void)
{ return (unsigned)sizeof(struct groovy_ack_link); }

DLLEXPORT int test_lat_classify(const struct groovy_ack_link *l, int has_sent,
                                uint32_t sent, uint32_t echo, uint32_t shown, int use_shown,
                                int64_t *in_flight, int64_t *in_net, int64_t *in_queue)
{
    return groovy_lat_classify(l, has_sent, sent, echo, shown, use_shown,
                               in_flight, in_net, in_queue);
}

/* ---------------------------------------------------------------------------
 * Clock servo. Same code as the future record_groovy.c -- the header is
 * included, not copied. What is green here is true in the driver.
 * ------------------------------------------------------------------------- */

DLLEXPORT void test_servo_reset(groovy_servo_state_t *st)
{
    groovy_servo_reset(st);
    log_servo_count_g = 0;
}

DLLEXPORT size_t test_servo_state_size(void)
{
    return sizeof(groovy_servo_state_t);
}

DLLEXPORT int test_servo_mode_parse(const char *s)
{
    return (int)groovy_servo_mode_parse(s);
}

/* test_servo_inject_ack -- injects a synthetic ack carrying `frame`
 * (progress in frames) into a groovy_ack_state_t, without going through
 * test_pacing_inject_ack (which only sets vcount). */
DLLEXPORT void test_servo_inject_ack(groovy_ack_state_t *st, uint32_t frame, uint64_t mono_ns)
{
    st->last_frame       = frame;
    st->last_ack_mono_ns = mono_ns;
    st->ack_lost_logged  = 0;
    st->has_any_ack      = 1;
}

DLLEXPORT void test_servo_note_send(groovy_servo_state_t *st, uint32_t frame_id, uint64_t now_ns)
{
    groovy_servo_note_send(st, frame_id, now_ns);
}

DLLEXPORT void test_servo_set_feed_forward(groovy_servo_state_t *st, double ff_ppm, double period_ns)
{
    groovy_servo_set_feed_forward(st, ff_ppm, period_ns);
}

DLLEXPORT void test_servo_update_post_poll(groovy_servo_state_t *st, const groovy_ack_state_t *ack,
                                           uint32_t emitted, uint64_t now_ns,
                                           int servo_active, int freeze)
{
    groovy_servo_update_post_poll(st, ack, emitted, now_ns, servo_active, freeze);
}

DLLEXPORT double test_servo_cadence_hz(double pclock_hz, unsigned h_total, unsigned v_total, int interlace)
{
    return groovy_servo_cadence_hz(pclock_hz, h_total, v_total, interlace);
}

DLLEXPORT double test_servo_ff_ppm(double cadence_hz, double video_refresh_rate_hz)
{
    return groovy_servo_ff_ppm(cadence_hz, video_refresh_rate_hz);
}

DLLEXPORT double test_servo_term(const groovy_servo_state_t *st)
{
    return groovy_servo_term(st);
}

DLLEXPORT double test_servo_term_ppm(const groovy_servo_state_t *st)
{
    return groovy_servo_term_ppm(st);
}

DLLEXPORT double test_servo_ff(const groovy_servo_state_t *st)
{
    return st->ff_ppm;
}

DLLEXPORT double test_servo_servo_ppm(const groovy_servo_state_t *st)
{
    return st->servo_ppm;
}

DLLEXPORT double test_servo_residual_ppm(const groovy_servo_state_t *st)
{
    return st->residual_ppm;
}

DLLEXPORT double test_servo_error(const groovy_servo_state_t *st)
{
    return st->error_frames;
}

DLLEXPORT double test_servo_latency_ns(const groovy_servo_state_t *st)
{
    return st->latency_ns;
}

DLLEXPORT int test_servo_state(const groovy_servo_state_t *st)
{
    return st->state;
}

DLLEXPORT unsigned test_servo_reanchors(const groovy_servo_state_t *st)
{
    return (unsigned)st->reanchors;
}

DLLEXPORT unsigned test_servo_toggles(const groovy_servo_state_t *st)
{
    return (unsigned)st->toggles;
}

DLLEXPORT unsigned test_servo_ring_misses(const groovy_servo_state_t *st)
{
    return (unsigned)st->ring_misses;
}

DLLEXPORT int test_servo_log_count(void)
{
    return log_servo_count_g;
}

DLLEXPORT double test_servo_authority_ppm(void)     { return GROOVY_SERVO_AUTHORITY_PPM; }
DLLEXPORT double test_servo_slope_max_ppm_s(void)  { return GROOVY_SERVO_SLOPE_MAX_PPM_S; }
DLLEXPORT double test_servo_kp(void)               { return GROOVY_SERVO_KP_PPM_FRAME; }
DLLEXPORT double test_servo_ki(void)                { return GROOVY_SERVO_KI_PPM_FRAME; }
DLLEXPORT double test_servo_setpoint_frames(void)  { return GROOVY_SERVO_SETPOINT_FRAMES; }
DLLEXPORT unsigned test_servo_ring_size(void)  { return GROOVY_SERVO_RING; }

/* The one-second window over the error. */
DLLEXPORT double test_servo_raw_error(const groovy_servo_state_t *st)
{
    return st->raw_error_frames;
}

DLLEXPORT unsigned test_servo_window_n(const groovy_servo_state_t *st)
{
    return (unsigned)st->window_n;
}

DLLEXPORT unsigned test_servo_samples(const groovy_servo_state_t *st)
{
    return (unsigned)st->samples;
}

DLLEXPORT unsigned test_servo_window_size(void) { return GROOVY_SERVO_WINDOW; }
DLLEXPORT unsigned long long test_servo_window_ns(void) { return GROOVY_SERVO_WINDOW_NS; }

/* ---------------------------------------------------------------------------
 * Menu/variable precedence. Same code as record_groovy.c -- the header
 * is included, not copied. What is green here is true in the driver.
 * ------------------------------------------------------------------------- */
DLLEXPORT int test_precedence_env_wins(const char *v)
{
    return groovy_precedence_env_wins(v);
}

DLLEXPORT int test_precedence_msg(char *dst, size_t cap, const char *key,
                                  const char *env_value, const char *variable)
{
    return groovy_precedence_msg(dst, cap, key, env_value, variable);
}

DLLEXPORT unsigned test_precedence_msg_cap(void)
{
    return (unsigned)GROOVY_FORCED_MSG_CAP;
}

/* ---------------------------------------------------------------------------
 * Composition of the four "Additional Follower N" switches. Same code as
 * record_groovy.c -- the header is included, not copied. What is green
 * here is true in the driver.
 *
 * Flattened signature (four enabled/address pairs) rather than an array
 * of pointers, to stay simple to marshal from ctypes -- the same choice
 * as the other bridges in this file (no array of char* anywhere else
 * here). */
DLLEXPORT int test_followers_menu_compose(
        int e1, const char *a1,
        int e2, const char *a2,
        int e3, const char *a3,
        int e4, const char *a4,
        char *out, size_t out_cap,
        char *err, size_t err_cap)
{
    int         enabled[4] = { e1, e2, e3, e4 };
    const char *address[4] = { a1, a2, a3, a4 };
    return groovy_followers_menu_compose(enabled, address, out, out_cap, err, err_cap);
}

DLLEXPORT unsigned test_followers_menu_n(void)
{
    return GROOVY_FOLLOWERS_MENU_N;
}
