/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <cormoran/zmk/custom_settings.h>
#include <zmk/ble.h>

#include "os_detection_internal.h"

/* One array setting per key: "ble_detected" (last auto-guess, cached across
 * reboots) and "ble_override" (manual override from the Custom Studio RPC web
 * UI, ZMK_OS_UNKNOWN == AUTO). Each has one element per BLE profile, indexed by
 * profile.
 *
 * zmk-feature-custom-settings' P3 rework replaced the old
 * ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE (one STRUCT_SECTION_ITERABLE
 * descriptor per element) with a single ZMK_CUSTOM_SETTING_ARRAY_DEFINE that
 * owns one contiguous backing buffer for the whole array - so we register the
 * two arrays once here instead of expanding a per-index macro in a #if ladder.
 * The elements are still reached individually with
 * zmk_custom_setting_find_array_element() below.
 *
 * The defaults array is a plain pointer (not a compound literal) and both
 * arrays share it since every element defaults to ZMK_OS_UNKNOWN; it is sized
 * for the 8-profile ceiling (>= ZMK_BLE_PROFILE_COUNT), the array registration
 * only consumes its first ZMK_BLE_PROFILE_COUNT entries. NO_CONSTRAINT is used
 * instead of a range constraint - see the ZMK_CUSTOM_SETTING_RANGE_INT32
 * pitfall noted in DESIGN.md. */

/* CONFIG_ZMK_BLE_PROFILE_COUNT (== CONFIG_BT_MAX_PAIRED, minus split
 * peripherals) defaults to 5; support up to 8 to leave headroom. */
BUILD_ASSERT(ZMK_BLE_PROFILE_COUNT <= 8,
             "zmk-feature-os-detection only defines settings for up to 8 BLE profiles");

ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32_DEFINE(os_detection_ble_defaults, ZMK_OS_UNKNOWN,
                                              ZMK_OS_UNKNOWN, ZMK_OS_UNKNOWN, ZMK_OS_UNKNOWN,
                                              ZMK_OS_UNKNOWN, ZMK_OS_UNKNOWN, ZMK_OS_UNKNOWN,
                                              ZMK_OS_UNKNOWN);

ZMK_CUSTOM_SETTING_ARRAY_DEFINE(os_detection_ble_detected, "cormoran__os_detection", "ble_detected",
                                ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_BLE_PROFILE_COUNT,
                                ZMK_BLE_PROFILE_COUNT, os_detection_ble_defaults,
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PERSONAL,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

ZMK_CUSTOM_SETTING_ARRAY_DEFINE(os_detection_ble_override, "cormoran__os_detection", "ble_override",
                                ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_BLE_PROFILE_COUNT,
                                ZMK_BLE_PROFILE_COUNT, os_detection_ble_defaults,
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PERSONAL,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

static const struct zmk_custom_setting *find_setting(const char *key, uint8_t profile_index) {
    return zmk_custom_setting_find_array_element("cormoran__os_detection", key, profile_index);
}

static enum zmk_os read_setting(const char *key, uint8_t profile_index) {
    const struct zmk_custom_setting *setting = find_setting(key, profile_index);
    if (!setting) {
        return ZMK_OS_UNKNOWN;
    }
    struct zmk_custom_setting_value value;
    if (zmk_custom_setting_read(setting, &value) != 0) {
        return ZMK_OS_UNKNOWN;
    }
    return (enum zmk_os)value.int32_value;
}

static int write_setting(const char *key, uint8_t profile_index, enum zmk_os os,
                         enum zmk_custom_setting_write_mode mode) {
    const struct zmk_custom_setting *setting = find_setting(key, profile_index);
    if (!setting) {
        return -ENODEV;
    }
    return zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(os), mode);
}

enum zmk_os zmk_os_detection_settings_get_override(uint8_t profile_index) {
    return read_setting("ble_override", profile_index);
}

int zmk_os_detection_settings_set_override(uint8_t profile_index, enum zmk_os os) {
    if (os != ZMK_OS_UNKNOWN && os != ZMK_OS_WINDOWS && os != ZMK_OS_MACOS && os != ZMK_OS_LINUX &&
        os != ZMK_OS_IOS && os != ZMK_OS_ANDROID) {
        return -EINVAL;
    }
    return write_setting("ble_override", profile_index, os, ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
}

enum zmk_os zmk_os_detection_settings_get_detected_cache(uint8_t profile_index) {
    return read_setting("ble_detected", profile_index);
}

int zmk_os_detection_settings_set_detected_cache(uint8_t profile_index, enum zmk_os os) {
    return write_setting("ble_detected", profile_index, os, ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
}
