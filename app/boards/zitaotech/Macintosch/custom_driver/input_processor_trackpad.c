#include <zmk/pointing_profiles.h>
/* Raw A320 processing for the dongle central only. Ordinary boards use a320.c. */
#define DT_DRV_COMPAT zmk_input_processor_dongle_trackpad
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>
#include <zmk/event_manager.h>
#include <zmk/matrix.h>
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
#include "../custom_driver_left/trackpad_led.h"
#endif
#include <zmk/events/position_state_changed.h>
#include <zitaotech/zmk/trackpad.h>
#include <dt-bindings/zmk/keys.h>

static atomic_t held[ZMK_KEYMAP_LEN];
static int positions(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev && ev->position < ARRAY_SIZE(held)) atomic_set(&held[ev->position], ev->state);
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(dongle_trackpad_keys, positions);
ZMK_SUBSCRIPTION(dongle_trackpad_keys, zmk_position_state_changed);
static bool pressed(int32_t p) { return p >= 0 && p < ARRAY_SIZE(held) && atomic_get(&held[p]); }
static int32_t pending_x, arrow_x, arrow_y, wheel_rem[2], pointer_rem[2];
static uint32_t last_motion;
static int last_mode = -1;
static int handle(const struct device *dev, struct input_event *ev, uint32_t p1, uint32_t p2,
                  struct zmk_input_processor_state *state) {
    ARG_UNUSED(dev); ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(state);
    if (ev->type != INPUT_EV_REL || (ev->code != INPUT_REL_X && ev->code != INPUT_REL_Y))
        return ZMK_INPUT_PROC_CONTINUE;
    bool arrow = (zitaotech_trackpad_mode() == ZITAOTECH_TRACKPAD_MODE_ARROW) ^
                 pressed(zitaotech_trackpad_arrow_key_position());
    bool scroll = !arrow && (pressed(zitaotech_trackpad_scroll_key_position()) ||
                 (zitaotech_trackpad_caps_lock_scroll_enabled() && (zitaotech_caps_indicators() & 2)));
    int mode = arrow ? 2 : scroll ? (zitaotech_trackpad_smooth_scroll_enabled() ? 4 : 3) : 0;
    uint32_t now = k_uptime_get_32();
    static uint32_t seen_epoch;
    uint32_t current_epoch = zmk_pointing_profile_epoch ? zmk_pointing_profile_epoch() : 0;
    if (mode != last_mode || now - last_motion > 150 || seen_epoch != current_epoch) {
        seen_epoch = current_epoch;
        arrow_x = arrow_y = wheel_rem[0] = wheel_rem[1] = 0;
        pointer_rem[0] = pointer_rem[1] = pending_x = 0;
        last_mode = mode;
    }
    last_motion = now;
    int axis = ev->code == INPUT_REL_X ? 0 : 1;
    int32_t value = ev->value;
    if (arrow) {
        if (!axis) { pending_x = value; }
        else {
            arrow_x += pending_x; arrow_y += value; pending_x = 0;
            int threshold = zitaotech_trackpad_arrow_swipe_threshold();
            if (abs(arrow_x) >= threshold || abs(arrow_y) >= threshold) {
                uint16_t usage = abs(arrow_x) >= abs(arrow_y)
                    ? (arrow_x > 0 ? HID_USAGE_KEY_KEYBOARD_RIGHTARROW : HID_USAGE_KEY_KEYBOARD_LEFTARROW)
                    : (arrow_y > 0 ? HID_USAGE_KEY_KEYBOARD_DOWNARROW : HID_USAGE_KEY_KEYBOARD_UPARROW);
                zitaotech_trackpad_relay_arrow_tap(usage);
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
                indicator_tp_arrow_fired();
#endif
                arrow_x = arrow_y = 0;
            }
        }
        ev->value = 0;
    } else if (scroll) {
        if (!axis) pending_x = value;
        int magnitude = MIN(MAX(abs(pending_x), abs(value)), 128);
        int divisor = 2 * (24 - 18 * magnitude * magnitude / (128 * 128));
        bool smooth = mode == 4;
        int precision = smooth ? 16 : 1;
        bool invert = axis ? zitaotech_trackpad_scroll_invert_y() : zitaotech_trackpad_scroll_invert_x();
        /* Match A320's native horizontal-positive / vertical-negative wheel signs. */
        int signed_value = value * (axis ? -1 : 1) * (invert ? -1 : 1);
        int scaled = signed_value * precision * (int)zitaotech_trackpad_scroll_speed_percent() * (int)zitaotech_trackpad_scroll_multiplier() / 100 + wheel_rem[axis];
        int output = magnitude > 2 ? scaled / divisor : 0;
        wheel_rem[axis] = (scaled - output * divisor) * 3 / 4;
#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
        if (!smooth) output *= 16;
#endif
        ev->code = axis ? INPUT_REL_WHEEL : INPUT_REL_HWHEEL;
        ev->value = output;
    } else {
        int mult = (40 + zitaotech_trackpad_dpi_value()) * 3 * zitaotech_trackpad_pointer_multiplier();
        if (pressed(zitaotech_trackpad_fast_key_position())) mult *= 2;
        else if (pressed(zitaotech_trackpad_slow_key_position())) mult /= 2;
        int scaled = value * mult + pointer_rem[axis];
        ev->value = scaled / 400;
        pointer_rem[axis] = scaled - ev->value * 400;
    }
    return ZMK_INPUT_PROC_CONTINUE;
}
static const struct zmk_input_processor_driver_api api = {.handle_event = handle};
#define INST(n) DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);
DT_INST_FOREACH_STATUS_OKAY(INST)
