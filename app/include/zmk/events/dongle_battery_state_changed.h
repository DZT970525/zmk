/* Logical left/right battery identity for KEYPOINT_DONGLE display only. */
#pragma once
#include <stdint.h>
#include <zmk/event_manager.h>
struct zmk_dongle_battery_state_changed {
    uint8_t side; /* 0 left, 1 right; independent of transport slot */
    uint8_t level;
};
ZMK_EVENT_DECLARE(zmk_dongle_battery_state_changed);
