/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_send_lock.c -- a codec session is used by TWO threads on ONE handle: the codec thread sends the pictures
 * (gm_send_codec_frame: an 11-byte 0x06 header, then the shards), the caller's thread sends audio, SWITCHRES and a
 * re-announced INIT. The datagrams of one send must not be interleaved with another's: an audio block is a CMD_AUDIO
 * header followed by opcode-less payload datagrams, and a shard that lands in between is swallowed as audio (the audio
 * is damaged, the frame loses a shard). gm_send_init also resets codec_seq while the codec thread increments it.
 *
 * What is proven, over a real loopback socket (port 42433, POSIX only):
 *   - every audio block comes out whole: its payload is the bytes that were sent, no shard inside, no command inside;
 *   - every frame that comes out of the shards is byte for byte the frame that was sent;
 *   - no datagram of one send is lost to another.
 * The receiver here is the daemon's rule, in a few lines: after a CMD_AUDIO header, the next datagrams are payload until the
 * size is reached, and a command header in the middle ends the block (an "escape").
 *
 * Built twice by the Makefile: with the lock (must pass) and with -DGM_TEST_NO_SEND_LOCK (the same test without the lock: it must
 * FAIL, or this test could not see the defect it is here for). */
#ifdef _WIN32
#include <stdio.h>
int main(void) { printf("test_send_lock : skipped on Windows\n"); return 0; }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "gm.h"
#include "gm_internal.h"
#include "gm_shard.h"

#define PORT       42433
#define FRAMES     3000u        /* pictures sent by the codec thread */
#define AUDIO_BYTES 6000u       /* one audio block: 1500 stereo frames of 16 bits */

static int g_fail;
#define EXPECT(cond, label) do { if (cond) printf("PASS: %s\n", label); else { printf("FAIL: %s\n", label); g_fail++; } } while (0)

static void pattern_fill(uint8_t *p, size_t n, uint32_t seed)
{
    size_t i;
    uint32_t x = seed * 2654435761u + 1u;
    for (i = 0; i < n; i++) { x = x * 1664525u + 1013904223u; p[i] = (uint8_t)(x >> 24); }
}
static size_t frame_len(uint32_t gid) { return 300u + (size_t)((gid * 2654435761u) % 8700u); }

/* ---- the receiver: the daemon's rule for audio, the shards for the pictures ---- */
static atomic_uint r_audio_ok, r_audio_bad, r_escape, r_frames_ok, r_frames_bad, r_dgrams;
static atomic_int  r_stop;
static int         r_sock = -1;

static int is_cmd_header(const uint8_t *d, size_t n)
{
    if (n == 8u && d[0] == 0x04 && !d[3] && !d[4] && !d[5] && !d[6] && !d[7]) return 1;   /* padded CMD_AUDIO */
    if (n == 8u && d[0] == 0x02 && d[1] == 0x10) return 1;                                  /* padded CMD_INIT, codec */
    if (n == 26u && d[0] == 0x03) return 1;                                                  /* CMD_SWITCHRES */
    if (n == 11u && d[0] == 0x06) return 1;                                                  /* CMD_BLIT_VSYNC */
    return 0;
}

static void *recv_thread(void *arg)
{
    uint8_t d[2048];
    size_t audio_left = 0u;
    gm_shard_rx *rx = gm_shard_rx_new();
    (void)arg;
    while (!atomic_load(&r_stop)) {
        const ssize_t n = recv(r_sock, d, sizeof d, 0);
        if (n < 0) continue;   /* the 100 ms timeout */
        atomic_fetch_add(&r_dgrams, 1u);
        if (audio_left > 0u) {
            size_t i;
            if (is_cmd_header(d, (size_t)n)) { atomic_fetch_add(&r_escape, 1u); atomic_fetch_add(&r_audio_bad, 1u); audio_left = 0u; }
            else {
                int ok = (size_t)n <= audio_left;
                for (i = 0; i < (size_t)n && ok; i++) if (d[i] != 0xA5) ok = 0;
                if (!ok) { atomic_fetch_add(&r_audio_bad, 1u); audio_left = 0u; continue; }   /* a shard (or a bad chunk) in the audio */
                audio_left -= (size_t)n;
                if (audio_left == 0u) atomic_fetch_add(&r_audio_ok, 1u);
                continue;
            }
        }
        if (is_cmd_header(d, (size_t)n)) {
            if (d[0] == 0x04) { audio_left = (size_t)(d[1] | (d[2] << 8)); if (audio_left == 0u) audio_left = 0u; }
            continue;
        }
        {
            gm_shard_frame f;
            if (gm_shard_rx_push(rx, d, (size_t)n, &f) == 1) {
                uint8_t *want = (uint8_t *)malloc(f.len ? f.len : 1u);
                pattern_fill(want, f.len, f.gid);
                if (f.len == frame_len(f.gid) && memcmp(want, f.data, f.len) == 0) atomic_fetch_add(&r_frames_ok, 1u);
                else atomic_fetch_add(&r_frames_bad, 1u);
                free(want);
            }
        }
    }
    gm_shard_rx_free(rx);
    return NULL;
}

/* ---- the codec thread ---- */
static gm_handle *g_h;
static atomic_int g_codec_done;
static void *codec_thread(void *arg)
{
    uint32_t i;
    (void)arg;
    for (i = 1; i <= FRAMES; i++) {
        const size_t n = frame_len(i);
        uint8_t *p = (uint8_t *)malloc(n);
        pattern_fill(p, n, i);
        (void)gm_send_codec_frame(g_h, i, (uint16_t)i, p, n, i == 1u);
        free(p);
        if ((i & 7u) == 0u) { struct timespec t = { 0, 200000L }; nanosleep(&t, NULL); }   /* 0.2 ms every 8 pictures: the receiver keeps up */
    }
    atomic_store(&g_codec_done, 1);
    return NULL;
}

int main(void)
{
    struct sockaddr_in a;
    struct timeval tv = { 0, 100000 };
    int rcvbuf = 16 * 1024 * 1024;
    pthread_t rt, ct;
    gm_init_params ip;
    gm_modeline m = { 6.7, 320, 336, 368, 426, 240, 244, 247, 262, 0 };
    unsigned audio_sent = 0u, loops = 0u;
    uint8_t *pcm;
    struct timespec t200 = { 0, 200000L };

    r_sock = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (r_sock < 0 || bind(r_sock, (struct sockaddr *)&a, sizeof a) != 0) { printf("FAIL: bind port %d\n", PORT); return 1; }
    setsockopt(r_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    setsockopt(r_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    g_h = gm_init("127.0.0.1", PORT);
    if (!g_h) { printf("FAIL: gm_init\n"); return 1; }
    gm_set_padding(g_h, 1);
    memset(&ip, 0, sizeof ip);
    ip.compression = GM_CODEC_H264;
    if (gm_send_init(g_h, &ip) != 0) { printf("FAIL: codec INIT\n"); return 1; }
    (void)gm_set_codec_parity(g_h, 10u);
    EXPECT(g_h->send_lock_on == 1u, "a_codec_INIT_turns_the_send_lock_on");

    pcm = (uint8_t *)malloc(AUDIO_BYTES);
    memset(pcm, 0xA5, AUDIO_BYTES);
    pthread_create(&rt, NULL, recv_thread, NULL);
    pthread_create(&ct, NULL, codec_thread, NULL);
    /* the caller's thread: audio all the time, SWITCHRES now and then, the INIT re-announced */
    while (!atomic_load(&g_codec_done)) {
        if (gm_send_audio(g_h, (const int16_t *)pcm, AUDIO_BYTES / 4u, 2u) == 0) audio_sent++;
        loops++;
        if (loops % 15u == 0u) (void)gm_send_switchres(g_h, &m);
        if (loops % 150u == 0u) (void)gm_send_init(g_h, &ip);
        nanosleep(&t200, NULL);
    }
    pthread_join(ct, NULL);
    { struct timespec t = { 0, 300000000L }; nanosleep(&t, NULL); }   /* the receiver drains */
    atomic_store(&r_stop, 1);
    pthread_join(rt, NULL);

    printf("INFO: %u audio blocks sent, %u whole, %u damaged (%u by a command in the block) ; %u frames whole, %u of %u damaged ; %u datagrams\n",
           audio_sent, atomic_load(&r_audio_ok), atomic_load(&r_audio_bad), atomic_load(&r_escape), atomic_load(&r_frames_ok),
           atomic_load(&r_frames_bad), FRAMES, atomic_load(&r_dgrams));
    EXPECT(atomic_load(&r_audio_bad) == 0u, "no_audio_block_is_damaged_by_a_shard_or_a_command");
    EXPECT(atomic_load(&r_frames_bad) == 0u, "no_frame_comes_out_different_from_what_was_sent");
    EXPECT(atomic_load(&r_audio_ok) >= audio_sent - 1u, "every_audio_block_arrived_whole");   /* -1: the held-back carry of padded mode */
    EXPECT(atomic_load(&r_frames_ok) * 100u >= FRAMES * 95u, "at_least_95_percent_of_the_frames_arrived");
    gm_close(g_h);
    free(pcm);
    printf("%s (test_send_lock)\n", g_fail ? "SOME CASES FAILED" : "ALL CASES PASSED");
    return g_fail ? 1 : 0;
}
#endif
