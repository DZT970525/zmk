extern void zmk_hold_tap_consume_scroll(int position) __attribute__((weak));
/*
 * TrackPoint DPI scaling on the SOFLE central (left) half.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_trackpoint_dpi

#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>

#include <drivers/input_processor.h>

#include "pointing_dpi.h"
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPOINT_CONTROL)
#include <zitaotech/zmk/trackpoint.h>
#endif

/* Linear TrackPoint DPI curve:
 * DPI 10 = 0.6x, DPI 100 = 2.5x, with equal spacing between each step.
 * scale = (19 * dpi + 350) / 900
 */
#define TRACKPOINT_SCALE_DPI_MULTIPLIER 19
#define TRACKPOINT_SCALE_OFFSET 350
#define TRACKPOINT_SCALE_DIVISOR 900

#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPOINT_CONTROL)

/*
 * Linear acceleration state, one entry per axis (0 = X, 1 = Y).
 *
 * While samples keep arriving the same way along an axis, the accumulated time
 * grows and the multiplier ramps linearly from x1.00 up to the configured
 * ceiling.  A pause, the end of the movement, or a reversal restarts the ramp.
 * There is a single TrackPoint, so the state lives here rather than in the
 * processor's per-instance state (which ZMK owns).
 */
struct trackpoint_accel_state {
    int64_t last_sample_ms;
    uint32_t elapsed_ms;
    int8_t last_sign;
};

static struct trackpoint_accel_state accel_state[2];

/*
 * Wheel steps are scaled by the scroll speed, so a fraction of a notch has to
 * carry over to the next report instead of being rounded away.
 */
static int32_t scroll_remainder;

/* Same idea as the pointer's ramp, but slower (the scroll key is held while
 * this builds, and wheel steps arrive much further apart than pointer samples). */
static struct trackpoint_accel_state scroll_accel_state;

static uint32_t scroll_accel_factor_hundredths(int32_t value) {
    uint32_t ceiling = zitaotech_trackpoint_scroll_accel();
    struct trackpoint_accel_state *state = &scroll_accel_state;
    int8_t sign = value > 0 ? 1 : -1;
    int64_t now = k_uptime_get();
    int64_t gap = now - state->last_sample_ms;

    if (state->last_sample_ms != 0 && sign == state->last_sign &&
        gap <= CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_ACCEL_IDLE_MS) {
        state->elapsed_ms = MIN(state->elapsed_ms + (uint32_t)gap,
                                CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_ACCEL_RAMP_MS);
    } else {
        state->elapsed_ms = 0;
    }

    state->last_sign = sign;
    state->last_sample_ms = now;

    if (ceiling <= CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_ACCEL_MIN) {
        return 100; /* off */
    }

    return 100 + (ceiling - 100) * state->elapsed_ms /
                     CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_ACCEL_RAMP_MS;
}

static uint32_t accel_factor_hundredths(int axis, int32_t value) {
    uint32_t ceiling = zitaotech_trackpoint_accel();
    struct trackpoint_accel_state *state = &accel_state[axis];
    int8_t sign = value > 0 ? 1 : -1;
    int64_t now = k_uptime_get();
    int64_t gap = now - state->last_sample_ms;

    if (state->last_sample_ms != 0 && sign == state->last_sign &&
        gap <= CONFIG_ZITAOTECH_TRACKPOINT_ACCEL_IDLE_MS) {
        state->elapsed_ms =
            MIN(state->elapsed_ms + (uint32_t)gap, CONFIG_ZITAOTECH_TRACKPOINT_ACCEL_RAMP_MS);
    } else {
        state->elapsed_ms = 0;
    }

    state->last_sign = sign;
    state->last_sample_ms = now;

    if (ceiling <= CONFIG_ZITAOTECH_TRACKPOINT_ACCEL_MIN) {
        return 100; /* off: no ramp at all */
    }

    return 100 + (ceiling - 100) * state->elapsed_ms /
                     CONFIG_ZITAOTECH_TRACKPOINT_ACCEL_RAMP_MS;
}

#endif // IS_ENABLED(CONFIG_ZITAOTECH_TRACKPOINT_CONTROL)

static int trackpoint_dpi_handle_event(const struct device *dev, struct input_event *event,
                                       uint32_t param1, uint32_t param2,
                                       struct zmk_input_processor_state *state) {
    ARG_UNUSED(dev);
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);

#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPOINT_CONTROL)
    if (event->type == INPUT_EV_REL &&
        (event->code == INPUT_REL_WHEEL || event->code == INPUT_REL_HWHEEL)) {
        /* Consume a short mouse tap only when this held mode key actually scrolls. */
        if (event->value != 0 && zitaotech_trackpoint_scroll_key_pressed() &&
            zmk_hold_tap_consume_scroll) {
            zmk_hold_tap_consume_scroll(zitaotech_trackpoint_scroll_key_position());
        }
        /* Studio flips either scroll axis on top of the driver's own signs. */
        bool inverted = event->code == INPUT_REL_WHEEL
                            ? zitaotech_trackpoint_scroll_invert_y()
                            : zitaotech_trackpoint_scroll_invert_x();
        int32_t value = inverted ? -event->value : event->value;
        int32_t multiplier = (int32_t)zitaotech_trackpoint_scroll_multiplier();
        multiplier = multiplier * (int32_t)scroll_accel_factor_hundredths(value) / 100;
        int32_t scaled = value * multiplier + scroll_remainder;

        event->value = scaled / 100;
        scroll_remainder = scaled - event->value * 100;
        return ZMK_INPUT_PROC_CONTINUE;
    }
#endif

    if (event->type != INPUT_EV_REL ||
        (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int32_t multiplier =
        sofle_trackpoint_dpi_get() * TRACKPOINT_SCALE_DPI_MULTIPLIER + TRACKPOINT_SCALE_OFFSET;
    int32_t divisor = TRACKPOINT_SCALE_DIVISOR;
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPOINT_CONTROL)
    /* Burst mode multiplies the pointer speed on top of the DPI-derived
     * factor; it is 1 when burst mode is off. */
    multiplier *= (int32_t)zitaotech_trackpoint_pointer_multiplier();

    /* Per-direction fine tuning: a TrackPoint is rarely equally sensitive the
     * two ways along an axis, so the sign of this sample picks the factor. */
    if (event->value != 0) {
        bool positive = event->value > 0;
        enum zitaotech_trackpoint_axis axis =
            event->code == INPUT_REL_X
                ? (positive ? ZITAOTECH_TRACKPOINT_AXIS_X_POSITIVE
                            : ZITAOTECH_TRACKPOINT_AXIS_X_NEGATIVE)
                : (positive ? ZITAOTECH_TRACKPOINT_AXIS_Y_POSITIVE
                            : ZITAOTECH_TRACKPOINT_AXIS_Y_NEGATIVE);
        multiplier = multiplier * (int32_t)zitaotech_trackpoint_axis_scale(axis) / 100;
        multiplier = multiplier * (int32_t)accel_factor_hundredths(event->code == INPUT_REL_X ? 0 : 1,
                                                                  event->value) /
                     100;
    }
#endif
    int32_t scaled_value = event->value * multiplier;

    if (state && state->remainder) {
        scaled_value += *state->remainder;
    }

    event->value = scaled_value / divisor;
    if (state && state->remainder) {
        *state->remainder = scaled_value - event->value * divisor;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api trackpoint_dpi_driver_api = {
    .handle_event = trackpoint_dpi_handle_event,
};

#define TRACKPOINT_DPI_INST(n)                                                                     \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                 \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &trackpoint_dpi_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TRACKPOINT_DPI_INST)
