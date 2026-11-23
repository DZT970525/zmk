/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* MOTION pulses also occur between packets; an edge alone is not a new touch.
 * Use a 200 ms successful-sample gap heuristic instead.
 */
struct bbp_touch_start_filter {
    uint32_t last_sample_ms;
    uint8_t discarded;
    bool started;
};

/* Call once per successful raw packet, including a zero-motion packet.
 * Failed I2C reads do not count toward the two discarded samples.
 */
static inline bool bbp_touch_sample_accept(struct bbp_touch_start_filter *s,
                                          uint32_t now, bool *new_touch) {
    *new_touch = !s->started || now - s->last_sample_ms >= 200;
    if (*new_touch) {
        s->started = true;
        s->discarded = 0;
    }
    s->last_sample_ms = now;
    if (s->discarded < 2) {
        s->discarded++;
        return false;
    }
    return true;
}
