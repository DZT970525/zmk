/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/hid_indicators_types.h>
#include <zmk/sensors.h>
#include <zephyr/sys/util.h>

enum zmk_split_transport_connections_status {
    ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED = 0,
    ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED,
    ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED,
};

struct zmk_split_transport_status {
    bool available;
    bool enabled;
    enum zmk_split_transport_connections_status connections;
};

/*
 * Payload of a relayed event (see zmk/event_manager.h).  It has to fit into a
 * single encrypted ATT write on the split service, which caps the whole struct
 * at CONFIG_BT_L2CAP_TX_MTU - 3 bytes; the event type and data limits below are
 * configured to keep it around 35 bytes.  The event type is kept NUL
 * terminated so both halves can compare it with strcmp().
 */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
struct zmk_split_relay_event_payload {
    uint8_t event_type_size;
    uint8_t event_data_size;
    char event_type[CONFIG_ZMK_SPLIT_RELAY_EVENT_TYPE_NAME_LEN + 1];
    uint8_t event_data[CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN];
} __packed;
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)

typedef struct zmk_split_transport_status (*zmk_split_transport_get_status_t)(void);
typedef int (*zmk_split_transport_set_enabled_t)(bool enabled);

enum zmk_split_transport_peripheral_event_type {
    ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
    ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT,
    ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,
    ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT,
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
    ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_RELAY_EVENT,
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
};

struct zmk_split_transport_peripheral_event {
    enum zmk_split_transport_peripheral_event_type type;

    union {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
        struct zmk_split_relay_event_payload relay_event;
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)

        struct {
            uint8_t position;
            uint8_t pressed;
        } key_position_event;

        struct {
            struct zmk_sensor_channel_data channel_data;

            uint8_t sensor_index;
        } sensor_event;

        struct {
            uint8_t reg;
            uint8_t sync;
            uint8_t type;
            uint16_t code;
            int32_t value;
        } input_event;

        struct {
            uint8_t level;
        } battery_event;
    } data;
} __packed;

enum zmk_split_transport_central_command_type {
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_POLL_EVENTS,
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_INVOKE_BEHAVIOR,
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_PHYSICAL_LAYOUT,
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_HID_INDICATORS,
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_RELAY_EVENT,
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
} __packed;

struct zmk_split_transport_central_command {
    enum zmk_split_transport_central_command_type type;

    union {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
        struct zmk_split_relay_event_payload relay_event;
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)

        struct {
            char behavior_dev[16];
            uint32_t param1, param2;
            uint32_t position;
            uint8_t event_source;
            uint8_t state;
        } invoke_behavior;

        struct {
            uint8_t layout_idx;
        } set_physical_layout;

        struct {
            zmk_hid_indicators_t indicators;
        } set_hid_indicators;
    } data;
} __packed;
