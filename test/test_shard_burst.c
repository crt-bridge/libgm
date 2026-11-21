/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_shard_burst.c -- the codec transport under BURST losses, as Wi-Fi makes them (phase 38, slice 4).
 *
 * A two-state loss model (good: nothing lost; bad: everything lost; Gilbert-Elliott) drives the datagrams of the real
 * H.264 stream (60 pictures, a keyframe every 10, cycled). For each parity percentage it prints what comes out: how many
 * frames are delivered and shown, how long a hole lasts before a keyframe puts the decoder back on its feet (the
 * keyframe is asked for at once and comes a frame or two later), and what the parity costs on the wire.
 * The table is the measure that decides the default parity; the assertions only guard the arithmetic (no frame is ever
 * delivered wrong, more parity never delivers noticeably fewer frames). POSIX only. */
#ifdef _WIN32
#include <stdio.h>
int main(void) { printf("test_shard_burst : skipped on Windows\n"); return 0; }
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gm_shard.h"

static int g_fail = 0;
#define EXPECT(cond, label) do { if (cond) printf("PASS: %s\n", label); else { printf("FAIL: %s\n", label); g_fail++; } } while (0)

static uint32_t g_rng = 0xC0FFEE11u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static int chance(double p) { return (rnd() & 0xFFFFFFu) < (uint32_t)(p * 16777216.0); }

struct bag { uint8_t d[300][1500]; size_t n[300]; unsigned count; };
static int collect(void *u, const uint8_t *dg, size_t len)
{
    struct bag *b = (struct bag *)u;
    if (b->count >= 300u) return -1;
    memcpy(b->d[b->count], dg, len);
    b->n[b->count++] = len;
    return 0;
}

struct stream { unsigned n; size_t *size; uint8_t *key; uint8_t *data; size_t *off; };

static int load(struct stream *s, const char *a, const char *b)
{
    FILE *f = fopen(a, "rb");
    unsigned char h[8];
    unsigned i;
    long total = 0;
    if (!f) f = fopen(b, "rb");
    if (!f || fread(h, 1, 8, f) != 8 || memcmp(h, "GMFX", 4)) return -1;
    s->n = (unsigned)(h[4] | (h[5] << 8) | (h[6] << 16) | ((unsigned)h[7] << 24));
    s->size = (size_t *)calloc(s->n, sizeof(size_t));
    s->key = (uint8_t *)calloc(s->n, 1);
    s->off = (size_t *)calloc(s->n, sizeof(size_t));
    for (i = 0; i < s->n; i++) {
        unsigned char r[5];
        if (fread(r, 1, 5, f) != 5) return -1;
        s->size[i] = (size_t)(r[0] | (r[1] << 8) | (r[2] << 16) | ((unsigned)r[3] << 24));
        s->key[i] = r[4];
        s->off[i] = (size_t)total;
        total += (long)s->size[i];
    }
    s->data = (uint8_t *)malloc((size_t)total);
    if (fread(s->data, 1, (size_t)total, f) != (size_t)total) return -1;
    fclose(f);
    return 0;
}

struct result { unsigned frames, delivered, identical, shown, lost, holes; double overhead, mean_hole, max_hole; };

/* p_gb: good -> bad per datagram; p_bg: bad -> good. Mean burst = 1 / p_bg datagrams. */
static void run(const struct stream *st, unsigned parity, double p_gb, double p_bg, unsigned total_frames, struct result *r)
{
    gm_shard_rx *rx = gm_shard_rx_new();
    static struct bag bag;
    unsigned fr, j, bad = 0;
    unsigned long long wire = 0, raw = 0;
    int need_key = 1, asked = 0, in_hole = 0;
    unsigned ask_at = 0, hole_start = 0, holes = 0;
    double hole_sum = 0.0, hole_max = 0.0;
    memset(r, 0, sizeof *r);
    r->frames = total_frames;
    for (fr = 0; fr < total_frames; fr++) {
        const unsigned i = fr % st->n;
        const int key = (i % 10u) == 0u || (asked && fr >= ask_at);   /* the emitter owes a keyframe a frame or two after the request */
        gm_shard_frame f;
        memset(&bag, 0, sizeof bag);
        gm_shard_send(st->data + st->off[i], st->size[i], (uint16_t)fr, key ? GM_SHARD_F_KEY : 0, 1472, parity, collect, &bag);
        raw += st->size[i];
        for (j = 0; j < bag.count; j++) {
            wire += bag.n[j];
            bad = bad ? !chance(p_bg) : chance(p_gb);
            if (bad) continue;
            if (gm_shard_rx_push(rx, bag.d[j], bag.n[j], &f) == 1) {
                r->delivered++;
                if (f.len == st->size[i] && !memcmp(f.data, st->data + st->off[i], f.len)) r->identical++;
                if (f.gap) need_key = 1;
                if (need_key && !(f.flags & GM_SHARD_F_KEY)) {
                    if (!asked) { asked = 1; ask_at = fr + 2u; }
                    if (!in_hole) { in_hole = 1; hole_start = fr; }
                } else {
                    need_key = 0;
                    if (f.flags & GM_SHARD_F_KEY) asked = 0;
                    r->shown++;
                    if (in_hole) {
                        const double h = (double)(fr - hole_start);
                        hole_sum += h; holes++;
                        if (h > hole_max) hole_max = h;
                        in_hole = 0;
                    }
                }
            }
        }
    }
    r->lost = total_frames - r->delivered;
    r->holes = holes;
    r->overhead = raw ? (double)wire / (double)raw : 0.0;
    r->mean_hole = holes ? hole_sum / holes : 0.0;
    r->max_hole = hole_max;
    gm_shard_rx_free(rx);
}

int main(void)
{
    struct stream st;
    static const unsigned par[] = { 0, 10, 20, 30, 50 };
    static const struct { const char *name; double p_gb, p_bg; } model[] = {
        { "light  (loss ~2%, bursts of ~4 datagrams)",   0.005, 0.25 },
        { "medium (loss ~6%, bursts of ~7 datagrams)",   0.010, 0.15 },
        { "heavy  (loss ~12%, bursts of ~10 datagrams)", 0.015, 0.10 },
    };
    unsigned m, p, prev;
    memset(&st, 0, sizeof st);
    if (load(&st, "test/fixtures/synthetic-h264-444-368x480-60-key10.bin", "libgm/test/fixtures/synthetic-h264-444-368x480-60-key10.bin") != 0) {
        printf("%s\n", "SKIP: a test fixture is missing (python -I libgm/test/fixtures/make_h264_fixtures.py --ffmpeg <ffmpeg with libx264> makes it)");
        return 77;   /* skipped: make stops on it, a silent green is never the answer */
    }
    for (m = 0; m < 3; m++) {
        printf("INFO: %s\n", model[m].name);
        printf("INFO:   parity  wire x   delivered   shown   lost  holes  mean hole (frames)  longest\n");
        prev = 0;
        for (p = 0; p < sizeof par / sizeof par[0]; p++) {
            struct result r;
            g_rng = 0xC0FFEE11u;
            run(&st, par[p], model[m].p_gb, model[m].p_bg, 6000u, &r);
            printf("INFO:   %3u %%   %5.2f   %6.1f %%  %5.1f %%  %5u  %5u  %9.1f          %5.0f\n", par[p], r.overhead,
                   100.0 * r.delivered / r.frames, 100.0 * r.shown / r.frames, r.lost, r.holes, r.mean_hole, r.max_hole);
            if (r.identical != r.delivered) { printf("FAIL: a delivered frame differs (parity %u)\n", par[p]); g_fail++; }
            if (r.delivered + 60u < prev) { printf("FAIL: more parity delivered noticeably fewer frames\n"); g_fail++; }
            prev = r.delivered;
        }
    }
    EXPECT(g_fail == 0, "no_delivered_frame_is_ever_wrong_and_more_parity_never_delivers_fewer");
    printf("%s (test_shard_burst)\n", g_fail ? "SOME CASES FAILED" : "ALL CASES PASSED");
    return g_fail ? 1 : 0;
}
#endif
