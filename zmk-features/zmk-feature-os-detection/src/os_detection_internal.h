/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cormoran/os-detection/os_detection.h>

/* --- USB fingerprint (os_detection_usb.c -> classifier) --- */

/* Small fixed set of "interesting" wLength values seen in real captures;
 * anything else falls into `string_wlength_other`. See docs/fingerprints.md
 * for the data these buckets are derived from. */
enum usb_fp_wlength_bucket {
    USB_FP_WLENGTH_255 = 0, /* Linux tends to request the full 255-byte buffer once */
    USB_FP_WLENGTH_2,       /* both OSes probe the descriptor header (2 bytes) first */
    USB_FP_WLENGTH_COUNT,
};

struct usb_fp_stats {
    uint16_t string_wlength_hist[USB_FP_WLENGTH_COUNT];
    uint16_t string_wlength_other;
    uint16_t string_request_count;
    bool bos_requested;
    uint16_t bos_wlength;
};

enum zmk_os zmk_os_classify_usb(const struct usb_fp_stats *stats);

/* Called by os_detection_usb.c once a USB enumeration cycle has settled. */
void zmk_os_detection_report_usb(enum zmk_os detected);

/* --- BLE fingerprint (os_detection_ble.c -> classifier) --- */

struct ble_fp_stats {
    uint16_t att_mtu;
    uint16_t initial_conn_interval; /* 1.25ms units, as reported by bt_conn_cb */
    uint8_t report_map_reads;
    uint8_t hids_info_reads;
    uint8_t report_ref_reads;
    uint8_t pnp_id_reads;
    uint8_t appearance_reads;
    bool ancs_or_ams_present; /* only meaningful when the GATT client probe ran */
};

enum zmk_os zmk_os_classify_ble(const struct ble_fp_stats *stats);

/* Called by os_detection_ble.c once a BLE fingerprint has settled for a profile. */
void zmk_os_detection_report_ble_detected(uint8_t profile_index, enum zmk_os detected);

/* --- Settings-backed override + detected-cache (os_detection_settings.c) ---
 * Only meaningful when CONFIG_ZMK_OS_DETECTION_BLE && CONFIG_ZMK_CUSTOM_SETTINGS;
 * os_detection_core.c guards every call site with IS_ENABLED() so these
 * symbols only need to exist under that combination. */
enum zmk_os zmk_os_detection_settings_get_override(uint8_t profile_index);
int zmk_os_detection_settings_set_override(uint8_t profile_index, enum zmk_os os);
enum zmk_os zmk_os_detection_settings_get_detected_cache(uint8_t profile_index);
int zmk_os_detection_settings_set_detected_cache(uint8_t profile_index, enum zmk_os os);

/* --- Queries used by the Custom Studio RPC handler --- */

bool zmk_os_detection_usb_connected(void);
enum zmk_os zmk_os_detection_usb_detected(void);

int zmk_os_detection_ble_profile_count(void);
bool zmk_os_detection_ble_profile_bonded(uint8_t profile_index);
bool zmk_os_detection_ble_profile_connected(uint8_t profile_index);
enum zmk_os zmk_os_detection_ble_profile_detected(uint8_t profile_index);
enum zmk_os zmk_os_detection_ble_profile_override(uint8_t profile_index);
enum zmk_os zmk_os_detection_ble_profile_effective(uint8_t profile_index);
/* Returns -ENOTSUP when BLE detection or custom settings aren't compiled in,
 * -EINVAL for an out-of-range profile_index. */
int zmk_os_detection_set_ble_override(uint8_t profile_index, enum zmk_os os);
/* -1 when no BLE profile is the active endpoint (USB active, or nothing connected). */
int zmk_os_detection_active_ble_profile_index(void);
/* The profile slot currently selected in ZMK's BLE profile switcher, valid
 * regardless of whether USB or BLE is the active transport right now.
 * 0 when BLE is not enabled. */
uint8_t zmk_os_detection_selected_ble_profile_index(void);
