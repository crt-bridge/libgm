/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_gm_seq.c -- direct unit test of gm_seq_is_newer(), one REQUIRE per row
 * of the case table in gm_seq.h's written specification (a clean-room
 * rewrite: this file and the specification are the only inputs the
 * author of gm_seq.h read).
 *
 * REQUIRE follows the same pattern as test/libgm_input_smoke.c: a
 * named case, a counter, and a FAIL line naming the failing case and
 * condition on stderr before returning 1.
 */

#include "gm_seq.h"

#include <stdint.h>
#include <stdio.h>

static int g_checks = 0;

#define REQUIRE(cond, case_name) do { \
    g_checks++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s] (%s:%d): %s\n", (case_name), __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

int main(void)
{
    /* # 1 -- frame strictly greater: order does not enter into it. */
    REQUIRE(gm_seq_is_newer(200u, 0u, 100u, 250u) == 1, "case-01-frame-greater");

    /* # 2 -- frame strictly smaller: order does not enter into it, no wrap. */
    REQUIRE(gm_seq_is_newer(50u, 255u, 100u, 0u) == 0, "case-02-frame-smaller-no-wrap");

    /* # 3 -- same frame, order strictly greater. */
    REQUIRE(gm_seq_is_newer(100u, 10u, 100u, 5u) == 1, "case-03-same-frame-order-greater");

    /* # 4 -- strictly identical pair: equality returns 0. */
    REQUIRE(gm_seq_is_newer(100u, 5u, 100u, 5u) == 0, "case-04-equal-pair");

    /* # 5 -- same frame, order strictly smaller (decreasing). */
    REQUIRE(gm_seq_is_newer(100u, 4u, 100u, 5u) == 0, "case-05-same-frame-order-smaller");

    /* # 6 -- low bounds of both keys, identical pair. */
    REQUIRE(gm_seq_is_newer(0u, 0u, 0u, 0u) == 0, "case-06-low-bounds-equal-pair");

    /* # 7 -- low bound of frame, equality broken by order. */
    REQUIRE(gm_seq_is_newer(0u, 1u, 0u, 0u) == 1, "case-07-low-bound-frame-order-tiebreak");

    /* # 8 -- high bound of frame, strictly greater despite a reference order
     * at its maximum. */
    REQUIRE(gm_seq_is_newer(4294967295u, 0u, 4294967294u, 255u) == 1,
            "case-08-high-bound-frame-greater-over-max-order");

    /* # 9 -- high bound of frame, EQUAL, received order strictly smaller
     * than the reference. */
    REQUIRE(gm_seq_is_newer(4294967295u, 0u, 4294967295u, 255u) == 0,
            "case-09-high-bound-frame-equal-order-smaller");

    /* # 10 -- same frame, order at its high bound, strictly greater than the
     * reference. */
    REQUIRE(gm_seq_is_newer(100u, 255u, 100u, 254u) == 1,
            "case-10-order-high-bound-greater");

    /* # 11 -- same frame, received order at its low bound against a
     * reference at its high bound: proof there is NO circular wrap on
     * order. */
    REQUIRE(gm_seq_is_newer(100u, 0u, 100u, 255u) == 0,
            "case-11-order-low-vs-high-reference-no-wrap");

    /* # 12 -- frame strictly greater wins even when the reference order is
     * at its maximum: same proof as row 8, at the other end of the frame
     * scale. */
    REQUIRE(gm_seq_is_newer(1u, 0u, 0u, 255u) == 1,
            "case-12-frame-greater-over-max-order-reference");

    printf("ALL %d gm_seq_is_newer cases PASSED\n", g_checks);
    return 0;
}
