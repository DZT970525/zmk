/* Q20 BLE mouse queue: discard stale relative motion, not control state. */
#pragma once

struct q20_mouse_queued_report {
    struct zmk_hid_mouse_report_body report;
    uint32_t queued_at;
};

struct q20_mouse_age_state {
    zmk_mouse_button_flags_t buttons;
    bool buttons_valid;
};

static inline bool q20_mouse_prepare(struct q20_mouse_queued_report *item, uint32_t now,
                                     const struct q20_mouse_age_state *state) {
    if ((uint32_t)(now - item->queued_at) <= CONFIG_ZITAOTECH_Q20_MOUSE_MAX_AGE_MS) {
        return true;
    }
    item->report.d_x = 0;
    item->report.d_y = 0;
    return !state->buttons_valid || item->report.buttons != state->buttons ||
           item->report.d_scroll_x != 0 || item->report.d_scroll_y != 0;
}
