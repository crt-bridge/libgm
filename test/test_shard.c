/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_shard.c -- the codec session's transport (phase 36, slice 1 of the codec phase).
 *
 * No encoder and no decoder here: the bytes are opaque. What is proven is that a bitstream
 * (60 access units of H.264 High 4:4:4 Predictive, 368x480, made from a synthetic test pattern: see fixtures/make_h264_fixtures.py) cut into
 * shards with Reed-Solomon parity comes out of the receiver byte for byte, with datagrams lost
 * and reordered, and that a frame with too many shards lost is NOT delivered rather than
 * delivered wrong. Then the same over a real loopback socket through gm_send_codec_frame.
 * POSIX only. Port 42411 (never the bench's 32100). */
#ifdef _WIN32
#include <stdio.h>
int main(void) { printf("test_shard : skipped on Windows\n"); return 0; }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "gm.h"
#include "gm_internal.h"
#include "gm_shard.h"

static int g_fail = 0;
#define EXPECT(cond, label) do { \
    if (cond) printf("PASS: %s\n", label); \
    else { printf("FAIL: %s\n", label); g_fail++; } \
} while (0)

/* deterministic PRNG, so a failure repeats */
static uint32_t g_rng = 0x2545F491u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

/* ---- sink collecting the datagrams of one frame ---- */
struct bag { uint8_t d[300][1500]; size_t n[300]; unsigned count; };
static int collect(void *u, const uint8_t *dg, size_t len)
{
    struct bag *b = (struct bag *)u;
    if (b->count >= 300u || len > 1500u) return -1;
    memcpy(b->d[b->count], dg, len); b->n[b->count] = len; b->count++;
    return 0;
}

static uint8_t *pattern(size_t n, uint32_t seed)
{
    uint8_t *p = (uint8_t *)malloc(n ? n : 1u);
    size_t i;
    uint32_t x = seed * 2654435761u + 1u;
    for (i = 0; i < n; i++) { x = x * 1664525u + 1013904223u; p[i] = (uint8_t)(x >> 24); }
    return p;
}

/* Feeds the datagrams of `bag` except those whose index is flagged in `drop`, in the order `ord`.
 * Returns 1 and fills *f if a frame came out. */
static int feed(gm_shard_rx *rx, struct bag *b, const uint8_t *drop, const unsigned *ord, gm_shard_frame *f)
{
    unsigned i;
    int got = 0;
    for (i = 0; i < b->count; i++) {
        const unsigned j = ord ? ord[i] : i;
        if (drop && drop[j]) continue;
        if (gm_shard_rx_push(rx, b->d[j], b->n[j], f) == 1) got = 1;
    }
    return got;
}

/* ---- the fixture: 60 access units of a synthetic test pattern ---- */
struct stream { unsigned n; size_t *size; uint8_t *key; uint8_t *data; };

static int load_stream(struct stream *s, const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char hdr[8];
    unsigned i;
    long total = 0;
    if (!f) return -1;
    if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, "GMFX", 4) != 0) { fclose(f); return -1; }
    s->n = (unsigned)(hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | ((unsigned)hdr[7] << 24));
    s->size = (size_t *)calloc(s->n, sizeof(size_t));
    s->key = (uint8_t *)calloc(s->n, 1);
    for (i = 0; i < s->n; i++) {
        unsigned char r[5];
        if (fread(r, 1, 5, f) != 5) { fclose(f); return -1; }
        s->size[i] = (size_t)(r[0] | (r[1] << 8) | (r[2] << 16) | ((unsigned)r[3] << 24));
        s->key[i] = r[4];
        total += (long)s->size[i];
    }
    s->data = (uint8_t *)malloc((size_t)total);
    if (fread(s->data, 1, (size_t)total, f) != (size_t)total) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

int main(void)
{
    gm_shard_rx *rx = gm_shard_rx_new();
    static struct bag bag;
    gm_shard_frame f;
    unsigned k, m;
    size_t sz;

    /* ---- the plan ---- */
    EXPECT(gm_shard_plan(0, 1472, 20, &k, &m, &sz) == 0 && k == 1 && m == 1 && sz == 250, "plan_tiny_frame_is_one_data_shard_and_one_parity");
    EXPECT(gm_shard_plan(1463, 1472, 20, &k, &m, &sz) == 0 && k == 2 && sz >= 733 && m == 1, "plan_just_over_one_shard_splits_in_two");
    EXPECT(gm_shard_plan(13000, 1472, 20, &k, &m, &sz) == 0 && k == 9 && m == 2, "plan_13k_is_9_plus_2");
    EXPECT(gm_shard_plan(13000, 1472, 0, &k, &m, &sz) == 0 && m == 0, "plan_no_parity_when_zero_percent");
    EXPECT(gm_shard_plan(10000000, 1472, 20, &k, &m, &sz) == -1, "plan_too_big_for_255_shards_refused");
    EXPECT(gm_shard_plan(100, 255, 20, &k, &m, &sz) == -1, "plan_datagram_under_256_refused");

    /* ---- every datagram is between 256 bytes and the MTU, every frame size ---- */
    {
        static const size_t lens[] = { 0, 1, 100, 246, 247, 250, 1000, 1462, 1463, 1466, 1467, 2928, 9000, 40000, 131072 };
        unsigned li, i;
        int ok = 1;
        for (li = 0; li < sizeof lens / sizeof lens[0]; li++) {
            uint8_t *p = pattern(lens[li], (uint32_t)li);
            memset(&bag, 0, sizeof bag);
            if (gm_shard_send(p, lens[li], (uint16_t)(100 + li), 0, 1472, 20, collect, &bag) != 0) { ok = 0; free(p); break; }
            for (i = 0; i < bag.count; i++)
                if (bag.n[i] < 256u || bag.n[i] > 1472u) ok = 0;
            free(p);
        }
        EXPECT(ok, "no_datagram_under_256_bytes_or_over_the_mtu");
    }

    /* ---- lossless round trip, every size, in order and shuffled ---- */
    {
        static const size_t lens[] = { 0, 1, 100, 250, 1462, 1463, 9000, 40000, 131072 };
        unsigned li, i;
        int ok = 1, ok_shuf = 1;
        for (li = 0; li < sizeof lens / sizeof lens[0]; li++) {
            uint8_t *p = pattern(lens[li], 7u + li);
            unsigned ord[300];
            gm_shard_rx_reset(rx);
            memset(&bag, 0, sizeof bag);
            gm_shard_send(p, lens[li], (uint16_t)(5 + li), GM_SHARD_F_KEY, 1472, 20, collect, &bag);
            if (!(feed(rx, &bag, NULL, NULL, &f) == 1 && f.len == lens[li] && (lens[li] == 0 || !memcmp(f.data, p, lens[li]))
                  && f.recovered == 0 && (f.flags & GM_SHARD_F_KEY))) ok = 0;
            gm_shard_rx_reset(rx);
            for (i = 0; i < bag.count; i++) ord[i] = i;
            for (i = bag.count; i > 1; i--) { unsigned j = rnd() % i, t = ord[i - 1]; ord[i - 1] = ord[j]; ord[j] = t; }
            if (!(feed(rx, &bag, NULL, ord, &f) == 1 && f.len == lens[li] && (lens[li] == 0 || !memcmp(f.data, p, lens[li])))) ok_shuf = 0;
            free(p);
        }
        EXPECT(ok, "lossless_round_trip_byte_for_byte_every_size");
        EXPECT(ok_shuf, "round_trip_with_the_shards_in_any_order");
    }

    /* ---- exactly m lost: rebuilt; m + 1 lost: NOT delivered ---- */
    {
        uint8_t *p = pattern(40000, 99u);
        unsigned trial, i, kk = 0, mm = 0;
        int rebuilt_ok = 1, refused_ok = 1;
        size_t szz;
        gm_shard_plan(40000, 1472, 20, &kk, &mm, &szz);
        memset(&bag, 0, sizeof bag);
        gm_shard_send(p, 40000, 700, 0, 1472, 20, collect, &bag);
        for (trial = 0; trial < 60; trial++) {
            uint8_t drop[300];
            unsigned lost = 0;
            memset(drop, 0, sizeof drop);
            while (lost < mm) { unsigned j = rnd() % bag.count; if (!drop[j]) { drop[j] = 1; lost++; } }
            gm_shard_rx_reset(rx);
            if (!(feed(rx, &bag, drop, NULL, &f) == 1 && f.len == 40000 && !memcmp(f.data, p, 40000))) rebuilt_ok = 0;
            while (lost < mm + 1u) { unsigned j = rnd() % bag.count; if (!drop[j]) { drop[j] = 1; lost++; } }
            gm_shard_rx_reset(rx);
            if (feed(rx, &bag, drop, NULL, &f) == 1) refused_ok = 0;
            (void)i;
        }
        EXPECT(rebuilt_ok, "m_shards_lost_anywhere_are_rebuilt_byte_for_byte");
        EXPECT(refused_ok, "m_plus_1_lost_is_not_delivered_never_delivered_wrong");
        free(p);
    }

    /* ---- the fixture stream, 0 % then 10 % of the datagrams lost ---- */
    {
        struct stream st;
        memset(&st, 0, sizeof st);
        if (load_stream(&st, "test/fixtures/synthetic-h264-444-368x480-60.bin") != 0 &&
            load_stream(&st, "libgm/test/fixtures/synthetic-h264-444-368x480-60.bin") != 0) {
            printf("%s\n", "SKIP: a test fixture is missing (python -I libgm/test/fixtures/make_h264_fixtures.py --ffmpeg <ffmpeg with libx264> makes it)");
            exit(77);   /* skipped: make stops on it, a silent green is never the answer */
        } else {
            unsigned pass, fr, i;
            for (pass = 0; pass < 2; pass++) {
                const unsigned loss_permille = pass ? 100u : 0u;
                size_t off = 0;
                unsigned delivered = 0, identical = 0, expected_ok = 0, gaps = 0;
                uint64_t sent_bytes = 0, raw_bytes = 0;
                gm_shard_rx_free(rx);
                rx = gm_shard_rx_new();
                for (fr = 0; fr < st.n; fr++) {
                    uint8_t drop[300];
                    unsigned kk, mm, dropped = 0;
                    size_t szz;
                    memset(&bag, 0, sizeof bag);
                    memset(drop, 0, sizeof drop);
                    gm_shard_send(st.data + off, st.size[fr], (uint16_t)(1000 + fr), st.key[fr] ? GM_SHARD_F_KEY : 0,
                                  1472, 20, collect, &bag);
                    gm_shard_plan(st.size[fr], 1472, 20, &kk, &mm, &szz);
                    for (i = 0; i < bag.count; i++) {
                        sent_bytes += bag.n[i];
                        if (rnd() % 1000u < loss_permille) { drop[i] = 1; dropped++; }
                    }
                    raw_bytes += st.size[fr];
                    if (dropped <= mm) expected_ok++;
                    if (feed(rx, &bag, drop, NULL, &f) == 1) {
                        delivered++;
                        if (f.len == st.size[fr] && !memcmp(f.data, st.data + off, f.len)) identical++;
                        if (f.gap) gaps++;
                    }
                    off += st.size[fr];
                }
                if (pass == 0) {
                    EXPECT(delivered == st.n && identical == st.n && gaps == 0, "stream_60_frames_lossless_identical_no_gap");
                    printf("INFO: 60 frames, %llu bytes of bitstream, %llu on the wire (header and 20 %% parity)\n",
                           (unsigned long long)raw_bytes, (unsigned long long)sent_bytes);
                } else {
                    const gm_shard_stats *ss = gm_shard_rx_stats(rx);
                    EXPECT(delivered == identical, "stream_10_percent_loss_every_delivered_frame_is_identical");
                    EXPECT(delivered == expected_ok, "stream_10_percent_loss_delivers_exactly_the_frames_the_parity_can_save");
                    EXPECT(ss->recovered_frames > 0, "stream_10_percent_loss_some_frames_needed_the_parity");
                    printf("INFO: 10 %% loss: %u of %u frames delivered (%u rebuilt from parity), %u arrived after a gap\n",
                           delivered, st.n, (unsigned)ss->recovered_frames, gaps);
                }
            }
            free(st.size); free(st.key); free(st.data);
        }
    }

    /* ---- two frames in flight, the later one first: the late one is not delivered ---- */
    {
        uint8_t *a = pattern(5000, 1u), *b = pattern(5000, 2u);
        static struct bag ba, bb;
        memset(&ba, 0, sizeof ba); memset(&bb, 0, sizeof bb);
        gm_shard_send(a, 5000, 10, 0, 1472, 20, collect, &ba);
        gm_shard_send(b, 5000, 11, 0, 1472, 20, collect, &bb);
        gm_shard_rx_reset(rx);
        EXPECT(feed(rx, &bb, NULL, NULL, &f) == 1 && f.frame_id == 11, "later_frame_delivered_first");
        EXPECT(feed(rx, &ba, NULL, NULL, &f) == 0, "earlier_frame_arriving_late_is_dropped");
        free(a); free(b);
    }

    /* ---- malformed datagrams are rejected, never read out of bounds ---- */
    {
        uint8_t junk[400];
        gm_shard_rx_reset(rx);
        memset(junk, 0, sizeof junk);
        EXPECT(gm_shard_rx_push(rx, junk, 100, &f) == -1, "short_datagram_rejected");
        junk[3] = 0; junk[4] = 0;
        EXPECT(gm_shard_rx_push(rx, junk, 400, &f) == -1, "k_zero_rejected");
        junk[3] = 200; junk[4] = 100;
        EXPECT(gm_shard_rx_push(rx, junk, 400, &f) == -1, "k_plus_m_over_255_rejected");
        junk[3] = 2; junk[4] = 1; junk[2] = 9;
        EXPECT(gm_shard_rx_push(rx, junk, 400, &f) == -1, "index_out_of_range_rejected");
    }
    gm_shard_rx_free(rx);

    /* ---- over a real socket, through the library ---- */
    {
        const uint16_t port = 42411u;
        const int s = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a; memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) != 0) { printf("FAIL: bind\n"); return 1; }
        struct timeval tv = { 0, 200000 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        uint8_t buf[2000];

        gm_init_params ip; memset(&ip, 0, sizeof ip);
        ip.compression = GM_CODEC_H264; ip.sample_rate = 0; ip.channels = 0;

        gm_handle *h = gm_init("127.0.0.1", port);
        EXPECT(gm_send_init(h, &ip) == -1, "codec_init_refused_in_a_classic_session");
        gm_set_padding(h, 1);
        EXPECT(gm_send_init(h, &ip) == 0, "codec_init_accepted_in_a_padded_session");
        {
            const ssize_t n = recv(s, buf, sizeof buf, 0);
            EXPECT(n == 8 && buf[0] == 0x02 && buf[1] == GM_CODEC_H264 && buf[4] == 0 && buf[7] == 0, "wire_init_is_02_10_and_zero_tail");
        }
        EXPECT(h->compression == 0 && h->codec == GM_CODEC_H264 && h->codec_parity_pct == 10, "lz4_stays_off_and_parity_defaults_to_10");
        {
            uint8_t px[100] = { 0 };
            EXPECT(gm_send_blit(h, 1, 1, px, sizeof px) == -1, "rgb_blit_refused_in_a_codec_session");
        }
        {
            uint8_t *p = pattern(20000, 5u);
            gm_shard_rx *r2 = gm_shard_rx_new();
            int got = 0, hdr_ok = 0;
            ssize_t n;
            EXPECT(gm_send_codec_frame(h, 77, 1234, p, 20000, 1) == 0, "codec_frame_sent");
            while ((n = recv(s, buf, sizeof buf, 0)) > 0) {
                if (!hdr_ok && buf[0] == 0x06 && n == 11) { hdr_ok = 1; continue; }
                if (gm_shard_rx_push(r2, buf, (size_t)n, &f) == 1) {
                    got = (f.frame_id == 0 && f.len == 20000 && !memcmp(f.data, p, 20000) && (f.flags & GM_SHARD_F_KEY));
                    break;
                }
            }
            EXPECT(hdr_ok, "groovy_blit_header_goes_first");
            EXPECT(got, "frame_comes_out_of_the_socket_identical");
            gm_shard_rx_free(r2); free(p);
        }
        EXPECT(gm_set_codec_parity(h, 101) == -1 && gm_set_codec_parity(h, 0) == 0, "parity_percentage_bounds");

        /* the capability bits of the peer's status */
        {
            struct sockaddr_in me; socklen_t ml = sizeof me;
            unsigned char st[13]; gm_status gs;
            getsockname(h->sock, (struct sockaddr *)&me, &ml);
            EXPECT(gm_peer_codecs(h) == 0, "peer_codecs_none_before_any_status");
            memset(st, 0, sizeof st); st[12] = 0x03;
            sendto(s, st, sizeof st, 0, (struct sockaddr *)&me, sizeof me);
            EXPECT(gm_recv_status(h, &gs, 200) == 0 && gm_peer_codecs(h) == 0, "peer_codecs_none_for_an_older_receiver");
            st[12] = 0x03 | FLAG_CODEC_H264;
            sendto(s, st, sizeof st, 0, (struct sockaddr *)&me, sizeof me);
            EXPECT(gm_recv_status(h, &gs, 200) == 0 && gm_peer_codecs(h) == 1u, "peer_codecs_h264");
            st[12] = 0x03 | FLAG_CODEC_H264 | FLAG_CODEC_HEVC;
            sendto(s, st, sizeof st, 0, (struct sockaddr *)&me, sizeof me);
            EXPECT(gm_recv_status(h, &gs, 200) == 0 && gm_peer_codecs(h) == 3u, "peer_codecs_h264_and_hevc");
        }
        /* the keyframe request counter, status bytes 10-11 */
        {
            struct sockaddr_in me2; socklen_t ml2 = sizeof me2;
            unsigned char st2[13]; gm_status gs2;
            getsockname(h->sock, (struct sockaddr *)&me2, &ml2);
            memset(st2, 0, sizeof st2); st2[12] = 0x03;
            EXPECT(gm_codec_idr_requested(h) == 0, "no_request_before_any_status");
            sendto(s, st2, sizeof st2, 0, (struct sockaddr *)&me2, sizeof me2);
            EXPECT(gm_recv_status(h, &gs2, 200) == 0 && gm_codec_idr_requested(h) == 0, "the_first_status_only_sets_the_baseline");
            st2[10] = 1;
            sendto(s, st2, sizeof st2, 0, (struct sockaddr *)&me2, sizeof me2);
            EXPECT(gm_recv_status(h, &gs2, 200) == 0 && gm_codec_idr_requested(h) == 1, "a_moved_counter_is_a_request");
            EXPECT(gm_codec_idr_requested(h) == 0, "a_request_is_read_once");
            sendto(s, st2, sizeof st2, 0, (struct sockaddr *)&me2, sizeof me2);
            EXPECT(gm_recv_status(h, &gs2, 200) == 0 && gm_codec_idr_requested(h) == 0, "the_same_counter_again_is_not_a_new_request");
            st2[10] = 3;   /* two requests, one status lost in between: still one keyframe owed */
            sendto(s, st2, sizeof st2, 0, (struct sockaddr *)&me2, sizeof me2);
            EXPECT(gm_recv_status(h, &gs2, 200) == 0 && gm_codec_idr_requested(h) == 1, "a_counter_that_jumped_is_still_a_request");
        }
        gm_close(h);
        close(s);
    }

    printf("%s (test_shard)\n", g_fail ? "SOME CASES FAILED" : "ALL CASES PASSED");
    return g_fail ? 1 : 0;
}
#endif
