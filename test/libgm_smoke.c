/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libgm_smoke.exe — end-to-end acceptance binary.
 *
 * Sends a short, deterministic Groovy v2 sequence to a daemon:
 *   1. CMD_INIT       (compression=0, sample_rate=3, channels=2)
 *   2. CMD_SWITCHRES  (gridlee modeline — same as the GroovyMAME capture
 *                      verified on hardware, JVC envelope-safe at 6.90625 MHz)
 *   3. CMD_BLIT_VSYNC (frame_id=1, vsync=0, all-0x80 mid-gray frame)
 *   4. CMD_CLOSE
 *
 * Usage: libgm_smoke.exe <receiver_ip> [port]
 * receiver_ip is REQUIRED, no default -- a stale fallback address once sent
 * this binary at an empty machine after the DHCP lease moved. Resolve the
 * receiver by its hardware address with `ip neigh`, never a written-down
 * IP: it changes with the lease.
 * Port defaults to 32100.
 *
 * Acceptance: after
 * this binary runs, the daemon's stderr/journalctl logs a `frame_complete`
 * event for the smoke-emitted frame. Verify via SSH:
 *   ssh root@<receiver_ip> 'journalctl -t crt-bridge | tail -5 | grep frame_complete'
 *
 * Wire-format note: this is the FIRST libgm-emitted byte stream that the
 * daemon parser sees end-to-end. If the gridlee modeline values diverge
 * from the receiver's frequency-envelope guard, the receiver will reject
 * the modeline before any blit lands — that is intentional defence in
 * depth, not a smoke regression.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gm.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s <receiver_ip> [port]\n"
            "receiver address required, no default.\n"
            "Resolve it by hardware address with `ip neigh`.\n",
            argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    unsigned    port = (argc > 2) ? (unsigned)atoi(argv[2]) : 32100u;

    gm_handle *h = gm_init(ip, (uint16_t)port);
    if (!h) { fprintf(stderr, "gm_init failed\n"); return 1; }

    gm_init_params init = {
        .compression = 0,    /* raw, MVP */
        .sample_rate = 0x03, /* matches the GroovyMAME capture `02 00 03 02` */
        .channels    = 0x02, /* matches the GroovyMAME capture */
    };
    if (gm_send_init(h, &init) != 0) {
        fprintf(stderr, "gm_send_init failed\n");
        gm_close(h);
        return 2;
    }

    /* gridlee modeline — matches the GroovyMAME 0.287 captured CMD_SWITCHRES
     * baseline and the JVC frequency envelope verified on hardware. */
    gm_modeline mode = {
        .pclock_mhz = 6.90625,
        .h_active = 341, .h_begin = 355, .h_end = 387, .h_total = 442,
        .v_active = 240, .v_begin = 243, .v_end = 246, .v_total = 264,
        .interlace = 0,
    };
    if (gm_send_switchres(h, &mode) != 0) {
        fprintf(stderr, "gm_send_switchres failed\n");
        gm_close(h);
        return 3;
    }

    /* One synthetic frame: 341 * 240 * 3 = 245520 bytes of 0x80 mid-gray.
     * That's ~167 MTU-sized UDP datagrams plus the 11-byte header. */
    size_t bytes = (size_t)mode.h_active * (size_t)mode.v_active * 3u;
    uint8_t *frame = (uint8_t *)malloc(bytes);
    if (!frame) {
        fprintf(stderr, "malloc(%zu) failed\n", bytes);
        gm_close(h);
        return 4;
    }
    memset(frame, 0x80, bytes);

    if (gm_send_blit(h, 1u, 0u, frame, bytes) != 0) {
        fprintf(stderr, "gm_send_blit failed\n");
        free(frame);
        gm_close(h);
        return 5;
    }

    free(frame);
    gm_send_close(h);
    gm_close(h);
    printf("libgm_smoke: sent CMD_INIT + CMD_SWITCHRES + CMD_BLIT_VSYNC "
           "(gridlee modeline, %zu payload bytes) to %s:%u\n",
           bytes, ip, port);
    return 0;
}
