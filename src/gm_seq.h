/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GM_SEQ_H
#define GM_SEQ_H

#include <stdint.h>

/* Written from the wire protocol's ordering semantics, not from any prior
 * source: a lexicographic comparison on the pair (frame, order), with frame
 * as the primary key and order only breaking ties when frame is equal. The
 * comparison is a plain, non-modular one -- a numerically smaller frame is
 * always older, even at the uint32_t boundary; there is no notion of wrap
 * or turn to account for. Strict equality of both members of the pair is
 * not "newer". */

static inline int gm_seq_is_newer(uint32_t frame, uint8_t order, uint32_t last_frame, uint8_t last_order)
{
    if (frame != last_frame) {
        return frame > last_frame;
    }
    return order > last_order;
}

#endif /* GM_SEQ_H */
