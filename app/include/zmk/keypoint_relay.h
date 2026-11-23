/* SPDX-License-Identifier: MIT */
#pragma once
#include <zephyr/bluetooth/conn.h>
#include <zmk/event_manager.h>

/* Radio coexistence comparison: 6 * 1.25 ms = 7.5 ms, zero latency.
 * Keep both connection creation and source retuning on the same interval. */
#define KEYPOINT_RELAY_ACTIVE_INTERVAL 6

#define KEYPOINT_RELAY_SERVICE BT_UUID_128_ENCODE(0xbaf0a660,0xea4d,0x4fbe,0xa448,0x7d839ed0bdb0)
#define KEYPOINT_RELAY_REPORT BT_UUID_128_ENCODE(0xbaf0a661,0xea4d,0x4fbe,0xa448,0x7d839ed0bdb0)
#define KEYPOINT_RELAY_CONTROL BT_UUID_128_ENCODE(0xbaf0a662,0xea4d,0x4fbe,0xa448,0x7d839ed0bdb0)
/* A versioned, fixed-size HID ABI. Fits the default ATT MTU: no keymap data. */
struct keypoint_relay_packet { uint8_t length; uint8_t data[20]; };
struct keypoint_relay_status {
    uint8_t left, right, wpm, layer, mods;
    uint8_t product; /* 0 unknown, 1 SOFLE, 2 KEYPOINT; local state, not report ABI */
    char label[15];
};
struct zmk_keypoint_relay_changed { bool connected; };
ZMK_EVENT_DECLARE(zmk_keypoint_relay_changed);
void zmk_keypoint_relay_start(void);
bool zmk_keypoint_relay_connected(void);
bool zmk_keypoint_relay_is_connection(struct bt_conn *conn);
bool zmk_keypoint_relay_routed(void);
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
bool zmk_keypoint_relay_manual_enabled(void);
int zmk_keypoint_relay_set_manual_enabled(bool enabled);
int zmk_keypoint_relay_toggle_route(void);
#endif
int zmk_keypoint_relay_send(uint8_t report_id, const void *body, size_t length);
int zmk_keypoint_relay_send_telemetry(const uint8_t *bytes, size_t length);
struct keypoint_relay_status zmk_keypoint_relay_status(void);

/* Experimental confirmation protocol, isolated from production relay UUIDs. */
bool zmk_mac_pair_pending(void);
bool zmk_mac_pair_connecting(void);
bool zmk_mac_pair_allowed(struct bt_conn *conn);
bool zmk_mac_pair_key(uint32_t position, bool pressed);

void zmk_receiver_relay_diagnostics(uint8_t out[24]);

int zmk_receiver_request_source_diagnostics(void);
int zmk_receiver_source_diagnostics(uint8_t out[23]);
