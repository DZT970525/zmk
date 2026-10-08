/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cormoran/os-detection/os_detection.h>
#include "os_detection_internal.h"

#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/endpoints.h>

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

ZMK_EVENT_IMPL(zmk_os_changed);

static enum zmk_os usb_detected = ZMK_OS_UNKNOWN;

#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE)
static enum zmk_os ble_detected[ZMK_BLE_PROFILE_COUNT];
#endif

static enum zmk_os last_raised = ZMK_OS_UNKNOWN;

bool zmk_os_detection_usb_connected(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_hid_ready();
#else
    return false;
#endif
}

enum zmk_os zmk_os_detection_usb_detected(void) { return usb_detected; }

int zmk_os_detection_ble_profile_count(void) {
#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE)
    return ZMK_BLE_PROFILE_COUNT;
#else
    return 0;
#endif
}

bool zmk_os_detection_ble_profile_bonded(uint8_t profile_index) {
#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE)
    return !zmk_ble_profile_is_open(profile_index);
#else
    return false;
#endif
}

bool zmk_os_detection_ble_profile_connected(uint8_t profile_index) {
#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE)
    return zmk_ble_profile_is_connected(profile_index);
#else
    return false;
#endif
}

enum zmk_os zmk_os_detection_ble_profile_detected(uint8_t profile_index) {
#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE)
    if (profile_index >= ZMK_BLE_PROFILE_COUNT) {
        return ZMK_OS_UNKNOWN;
    }
    return ble_detected[profile_index];
#else
    return ZMK_OS_UNKNOWN;
#endif
}

enum zmk_os zmk_os_detection_ble_profile_override(uint8_t profile_index) {
#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE) && IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS)
    if (profile_index >= ZMK_BLE_PROFILE_COUNT) {
        return ZMK_OS_UNKNOWN;
    }
    return zmk_os_detection_settings_get_override(profile_index);
#else
    return ZMK_OS_UNKNOWN;
#endif
}

enum zmk_os zmk_os_detection_ble_profile_effective(uint8_t profile_index) {
    enum zmk_os override = zmk_os_detection_ble_profile_override(profile_index);
    if (override != ZMK_OS_UNKNOWN) {
        return override;
    }
    return zmk_os_detection_ble_profile_detected(profile_index);
}

int zmk_os_detection_active_ble_profile_index(void) {
#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE)
    struct zmk_endpoint_instance endpoint = zmk_endpoint_get_selected();
    if (endpoint.transport != ZMK_TRANSPORT_BLE) {
        return -1;
    }
    return endpoint.ble.profile_index;
#else
    return -1;
#endif
}

uint8_t zmk_os_detection_selected_ble_profile_index(void) {
#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE)
    int index = zmk_ble_active_profile_index();
    return index < 0 ? 0 : (uint8_t)index;
#else
    return 0;
#endif
}

enum zmk_os zmk_os_detection_current(void) {
    struct zmk_endpoint_instance endpoint = zmk_endpoint_get_selected();
    switch (endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        return zmk_os_detection_usb_connected() ? usb_detected : ZMK_OS_UNKNOWN;
#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE)
    case ZMK_TRANSPORT_BLE:
        return zmk_os_detection_ble_profile_effective(endpoint.ble.profile_index);
#endif
    default:
        return ZMK_OS_UNKNOWN;
    }
}

static void raise_if_changed(void) {
    enum zmk_os current = zmk_os_detection_current();
    if (current == last_raised) {
        return;
    }
    last_raised = current;
    LOG_INF("os detection: effective OS for active endpoint changed to %d", current);
    raise_zmk_os_changed((struct zmk_os_changed){.os = current});
}

void zmk_os_detection_report_usb(enum zmk_os detected) {
    if (usb_detected == detected) {
        return;
    }
    LOG_INF("os detection: USB fingerprint settled on %d", detected);
    usb_detected = detected;
    raise_if_changed();
}

void zmk_os_detection_report_ble_detected(uint8_t profile_index, enum zmk_os detected) {
#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE)
    if (profile_index >= ZMK_BLE_PROFILE_COUNT) {
        return;
    }
    if (ble_detected[profile_index] == detected) {
        return;
    }
    LOG_INF("os detection: BLE fingerprint for profile %d settled on %d", profile_index, detected);
    ble_detected[profile_index] = detected;
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS)
    zmk_os_detection_settings_set_detected_cache(profile_index, detected);
#endif
    raise_if_changed();
#endif
}

int zmk_os_detection_set_ble_override(uint8_t profile_index, enum zmk_os os) {
#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE) && IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS)
    if (profile_index >= ZMK_BLE_PROFILE_COUNT) {
        return -EINVAL;
    }
    int rc = zmk_os_detection_settings_set_override(profile_index, os);
    if (rc == 0) {
        raise_if_changed();
    }
    return rc;
#else
    return -ENOTSUP;
#endif
}

static int os_detection_listener(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        if (!zmk_usb_is_hid_ready()) {
            usb_detected = ZMK_OS_UNKNOWN;
        }
    }
#endif
    /* Endpoint switches (USB<->BLE, or BLE profile change) can change the
     * effective OS even when no new fingerprint was observed. */
    raise_if_changed();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(os_detection_core, os_detection_listener);
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(os_detection_core, zmk_usb_conn_state_changed);
#endif
ZMK_SUBSCRIPTION(os_detection_core, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE)
ZMK_SUBSCRIPTION(os_detection_core, zmk_ble_active_profile_changed);
#endif

/* --- Layer auto-switch ---
 * Off by default (CONFIG_ZMK_OS_DETECTION_LAYER_AUTO_SWITCH=n): this
 * overlaps with https://github.com/cormoran/zmk-feature-default-layer,
 * which is the recommended way to auto-switch a default layer per host.
 */

#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_LAYER_AUTO_SWITCH)

#include <zmk/keymap.h>

static int8_t active_os_layer = -1;

static int8_t layer_for_os(enum zmk_os os) {
    switch (os) {
    case ZMK_OS_WINDOWS:
        return CONFIG_ZMK_OS_DETECTION_LAYER_WINDOWS;
    case ZMK_OS_MACOS:
        return CONFIG_ZMK_OS_DETECTION_LAYER_MACOS;
    case ZMK_OS_LINUX:
        return CONFIG_ZMK_OS_DETECTION_LAYER_LINUX;
    case ZMK_OS_IOS:
        return CONFIG_ZMK_OS_DETECTION_LAYER_IOS;
    case ZMK_OS_ANDROID:
        return CONFIG_ZMK_OS_DETECTION_LAYER_ANDROID;
    case ZMK_OS_UNKNOWN:
    default:
        return CONFIG_ZMK_OS_DETECTION_LAYER_UNKNOWN;
    }
}

static int os_detection_layer_listener(const zmk_event_t *eh) {
    const struct zmk_os_changed *ev = as_zmk_os_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    int8_t next_layer = layer_for_os(ev->os);
    if (next_layer == active_os_layer) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (active_os_layer >= 0 && active_os_layer < ZMK_KEYMAP_LAYERS_LEN) {
        zmk_keymap_layer_deactivate(active_os_layer, false);
    }
    if (next_layer >= 0 && next_layer < ZMK_KEYMAP_LAYERS_LEN) {
        zmk_keymap_layer_activate(next_layer, false);
    }
    active_os_layer = next_layer;
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(os_detection_layer, os_detection_layer_listener);
ZMK_SUBSCRIPTION(os_detection_layer, zmk_os_changed);

#endif /* IS_ENABLED(CONFIG_ZMK_OS_DETECTION_LAYER_AUTO_SWITCH) */
