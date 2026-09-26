/* SPDX-License-Identifier: GPL-3.0-or-later */
/* proto_probe.c — the receiver's REAL parser, callable from the emitter's
 * tests.
 *
 * Compiles the receiver's parser source directly, from outside this
 * repository (see the Makefile's $(PROBE_DLL) rule): the verdict returned
 * to the tests is that of the code that runs on the receiver, never that
 * of a copy that could silently drift.
 *
 * Includes ONLY the receiver's proto.h -- never gm.h: the two define the
 * same CMD_* macros, and mixing them in the same translation unit is a
 * gratuitous collision.
 *
 * Flattened exports for ctypes (no struct crossing the boundary -- the
 * gm_status padding bug already cost half a day on struct
 * marshalling).
 */

#include "proto.h"
#include "lz4.h"   /* From src/, added to PROBE_CFLAGS: the probe links
                    * against libgm.a, which already embeds lz4.c -- no lz4
                    * package needed on the machine. */

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
#  define DLLEXPORT __declspec(dllexport)
#else
#  define DLLEXPORT __attribute__((visibility("default")))
#endif

/* The four parse_result_t values, read from the receiver's own enum. The
 * test therefore never hardcodes a number: a future renumbering of the
 * enum cannot make the test falsely green. */
DLLEXPORT int probe_parse_ok(void)
{
    return (int)PARSE_OK;
}

DLLEXPORT int probe_parse_too_short(void)
{
    return (int)PARSE_TOO_SHORT;
}

DLLEXPORT int probe_parse_bad_opcode(void)
{
    return (int)PARSE_BAD_OPCODE;
}

DLLEXPORT int probe_parse_bad_compression(void)
{
    return (int)PARSE_BAD_COMPRESSION;
}

/* 0x07. Returns the parse_result_t code. The outputs are written even on
 * failure by parse_cmd_blit_field_vsync_ex itself (extras is zeroed first
 * on the receiver side) -- the test only reads them on PARSE_OK.
 *
 * *payload_len is set to (int)payload_len: this is the value that proves
 * no image byte travels in the header datagram. */
DLLEXPORT int probe_parse_blit_field(const uint8_t *buf, int len, int compressed,
                                     uint32_t *frame_id, int *field, int *vsync,
                                     uint32_t *lz4_size, int *delta, int *dup,
                                     int *payload_len)
{
    cmd_blit_field_vsync_base_t base;
    blit_field_extras_t extras;
    const uint8_t *payload = NULL;
    size_t payload_len_sz = 0;

    parse_result_t rc = parse_cmd_blit_field_vsync_ex(
        buf, (size_t)len, compressed, &base, &extras, &payload, &payload_len_sz);

    if (rc == PARSE_OK) {
        if (frame_id)    *frame_id    = base.frame_id;
        if (field)       *field       = (int)base.field;
        if (vsync)       *vsync       = (int)base.vsync;
        if (lz4_size)    *lz4_size    = extras.lz4_size;
        if (delta)       *delta       = (int)extras.delta;
        if (dup)         *dup         = (int)extras.dup;
        if (payload_len) *payload_len = (int)payload_len_sz;
    }

    return (int)rc;
}

/* 0x06. No `compressed` input: parse_cmd_blit_vsync decides by length
 * alone (7 or 11), unambiguously -- unlike 0x07. */
DLLEXPORT int probe_parse_blit_vsync(const uint8_t *buf, int len,
                                     uint32_t *frame_id, int *vsync,
                                     uint32_t *lz4_size)
{
    cmd_blit_vsync_base_t base;

    parse_result_t rc = parse_cmd_blit_vsync(buf, (size_t)len, &base);

    if (rc == PARSE_OK) {
        if (frame_id) *frame_id = base.frame_id;
        if (vsync)    *vsync    = (int)base.vsync;
        if (lz4_size) *lz4_size = base.lz4_size;
    }

    return (int)rc;
}

/* Proof by round-trip: a non-zero lz4_size proves nothing by itself, the
 * emitted block must give back EXACTLY the starting bytes. LZ4_decompress_safe
 * is the same function as the receiver's decoder uses, and the same
 * version (v1.10.0 on both sides of the wire, see src/LZ4-PROVENANCE.md).
 *
 * Returns the number of bytes written, or -1. */
DLLEXPORT int probe_lz4_decompress(const uint8_t *src, int src_len,
                                   uint8_t *dst, int dst_cap)
{
    if (!src || !dst || src_len <= 0 || dst_cap <= 0) return -1;

    int written = LZ4_decompress_safe((const char *)src, (char *)dst,
                                      src_len, dst_cap);
    return (written >= 0) ? written : -1;
}
