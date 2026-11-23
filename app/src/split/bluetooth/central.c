#include <zmk/screen_transfer.h>
/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/types.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/init.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/stdlib.h>
#include <zmk/ble.h>
#include <zmk/behavior.h>
#include <zmk/sensors.h>
#include <zmk/split/transport/central.h>
#include <zmk/split/bluetooth/uuid.h>
#include <zmk/split/bluetooth/service.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/pointing/input_split.h>
#include <zmk/hid_indicators_types.h>
#include <zmk/physical_layouts.h>
#include <zmk/split/role.h>
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_CLIENT)
#include <zmk/split/dongle_presence.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_PEER_SELECTION)
#include <zmk/split/peer_selection.h>
#endif

static int start_scanning(void);

#define POSITION_STATE_DATA_LEN 16

enum peripheral_slot_state {
    PERIPHERAL_SLOT_STATE_OPEN,
    PERIPHERAL_SLOT_STATE_CONNECTING,
    PERIPHERAL_SLOT_STATE_CONNECTED,
};

#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE))
#include <zmk/events/dongle_battery_state_changed.h>
ZMK_EVENT_IMPL(zmk_dongle_battery_state_changed);
#endif

struct peripheral_slot {
    enum peripheral_slot_state state;
#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE))
    uint8_t battery_side; /* 0 unknown, 1 left, 2 right */
    uint8_t display_battery_level;
    bool display_battery_valid;
#endif
    struct bt_conn *conn;
    struct bt_gatt_discover_params discover_params;
    struct bt_gatt_subscribe_params subscribe_params;
    struct bt_gatt_subscribe_params sensor_subscribe_params;
    struct bt_gatt_discover_params sub_discover_params;
#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && IS_ENABLED(CONFIG_SHIELD_LPM_VIEW)
    uint16_t image_handle;
    struct bt_gatt_exchange_params image_mtu_params;
    struct bt_gatt_subscribe_params image_sub;
#endif
    uint16_t run_behavior_handle;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    struct bt_gatt_subscribe_params batt_lvl_subscribe_params;
    struct bt_gatt_read_params batt_lvl_read_params;
#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING) */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
    uint16_t update_hid_indicators;
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
    uint16_t relay_event_handle;
    struct bt_gatt_subscribe_params relay_event_subscribe_params;
    // Dedicated CCC discovery state: bt_gatt_subscribe() refuses to reuse a
    // discover_params whose previous CCC discovery is still queued (it returns
    // -EBUSY), and this subscription happens right after the sensor one, so it
    // cannot share slot->sub_discover_params.
    struct bt_gatt_discover_params relay_event_sub_discover_params;
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
    uint16_t selected_physical_layout_handle;
    uint8_t position_state[POSITION_STATE_DATA_LEN];
    uint8_t changed_positions[POSITION_STATE_DATA_LEN];
};

#if IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT)

static const struct bt_uuid *gatt_ccc_uuid = BT_UUID_GATT_CCC;
static const struct bt_uuid *gatt_cpf_uuid = BT_UUID_GATT_CPF;

struct peripheral_input_slot {
    struct bt_conn *conn;
    struct bt_gatt_subscribe_params sub;
    uint8_t reg;
    bool reg_valid;
};

#define COUNT_INPUT_SPLIT(n) +1

static struct peripheral_input_slot
    peripheral_input_slots[(0 DT_FOREACH_STATUS_OKAY(zmk_input_split, COUNT_INPUT_SPLIT))];

static bool input_slot_is_open(size_t i) {
    return i < ARRAY_SIZE(peripheral_input_slots) && peripheral_input_slots[i].conn == NULL;
}

#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_PEER_SELECTION)
struct bt_conn *zmk_keypoint_input_peer_connection(uint8_t input_reg) {
    if (!zmk_split_role_is_central()) return NULL;
    for (size_t i = 0; i < ARRAY_SIZE(peripheral_input_slots); i++) {
        if (peripheral_input_slots[i].conn && peripheral_input_slots[i].reg_valid &&
            peripheral_input_slots[i].reg == input_reg)
            return bt_conn_ref(peripheral_input_slots[i].conn);
    }
    return NULL;
}
#endif

static bool input_slot_is_pending(size_t i) {
    return i < ARRAY_SIZE(peripheral_input_slots) && peripheral_input_slots[i].conn != NULL &&
           (!peripheral_input_slots[i].sub.value_handle ||
            !peripheral_input_slots[i].sub.ccc_handle || !peripheral_input_slots[i].reg_valid);
}

static int reserve_next_open_input_slot(struct peripheral_input_slot **slot, struct bt_conn *conn) {
    for (size_t i = 0; i < ARRAY_SIZE(peripheral_input_slots); i++) {
        if (input_slot_is_open(i)) {
            peripheral_input_slots[i].conn = conn;

            // Clear out any previously set values
            peripheral_input_slots[i].sub.value_handle = 0;
            peripheral_input_slots[i].sub.ccc_handle = 0;
            peripheral_input_slots[i].reg = 0;
            peripheral_input_slots[i].reg_valid = false;
            *slot = &peripheral_input_slots[i];
            return i;
        }
    }

    return -ENOMEM;
}

static int find_pending_input_slot(struct peripheral_input_slot **slot, struct bt_conn *conn) {
    for (size_t i = 0; i < ARRAY_SIZE(peripheral_input_slots); i++) {
        if (peripheral_input_slots[i].conn == conn && input_slot_is_pending(i)) {
            *slot = &peripheral_input_slots[i];
            return i;
        }
    }

    return -ENODEV;
}

void release_peripheral_input_subs(struct bt_conn *conn) {
    for (size_t i = 0; i < ARRAY_SIZE(peripheral_input_slots); i++) {
        if (peripheral_input_slots[i].conn == conn) {
            peripheral_input_slots[i].conn = NULL;
            zmk_input_split_peripheral_disconnected(peripheral_input_slots[i].reg);
        }
    }
}

#endif // IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT)

static zmk_split_transport_central_status_changed_cb_t transport_status_cb;
static bool is_enabled;

static struct peripheral_slot peripherals[ZMK_SPLIT_BLE_PERIPHERAL_COUNT];

#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN)
static atomic_t keypoint_split_inputs, keypoint_split_input_drops;
#endif
static bool is_scanning = false;
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_PEER_SELECTION)
/* Retry after the controller frees connection resources; leave active scans alone. */
static void scan_retry_cb(struct k_work *work) {
    ARG_UNUSED(work);
    start_scanning();
}
static K_WORK_DELAYABLE_DEFINE(scan_retry_work, scan_retry_cb);
static void split_central_recycled(void) {
    k_work_reschedule(&scan_retry_work, K_MSEC(100));
}
#endif

static const struct bt_uuid_128 split_service_uuid = BT_UUID_INIT_128(ZMK_SPLIT_BT_SERVICE_UUID);

struct peripheral_event_wrapper {
    uint8_t source;
    struct zmk_split_transport_peripheral_event event;
};

K_MSGQ_DEFINE(peripheral_event_msgq, sizeof(struct peripheral_event_wrapper),
              CONFIG_ZMK_SPLIT_BLE_CENTRAL_POSITION_QUEUE_SIZE, 4);

void peripheral_event_work_callback(struct k_work *work);

K_WORK_DEFINE(peripheral_event_work, peripheral_event_work_callback);

int peripheral_slot_index_for_conn(struct bt_conn *conn) {
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        if (peripherals[i].conn == conn) {
            return i;
        }
    }

    return -EINVAL;
}

struct peripheral_slot *peripheral_slot_for_conn(struct bt_conn *conn) {
    int idx = peripheral_slot_index_for_conn(conn);
    if (idx < 0) {
        return NULL;
    }

    return &peripherals[idx];
}

int release_peripheral_slot(int index) {
    if (index < 0 || index >= ZMK_SPLIT_BLE_PERIPHERAL_COUNT) {
        return -EINVAL;
    }

    struct peripheral_slot *slot = &peripherals[index];

    if (slot->state == PERIPHERAL_SLOT_STATE_OPEN) {
        return -EINVAL;
    }

    LOG_DBG("Releasing peripheral slot at %d", index);

    if (slot->conn != NULL) {
        bt_conn_unref(slot->conn);
        slot->conn = NULL;
    }
    slot->state = PERIPHERAL_SLOT_STATE_OPEN;
#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE))
    slot->battery_side = 0;
    slot->display_battery_valid = false;
#endif

    // Raise events releasing any active positions from this peripheral
    for (int i = 0; i < POSITION_STATE_DATA_LEN; i++) {
        for (int j = 0; j < 8; j++) {
            if (slot->position_state[i] & BIT(j)) {
                uint32_t position = (i * 8) + j;
                struct peripheral_event_wrapper ev = {
                    .source = index,
                    .event = {.type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
                              .data = {.key_position_event = {
                                           .position = position,
                                           .pressed = false,
                                       }}}};

                k_msgq_put(&peripheral_event_msgq, &ev, K_NO_WAIT);
                k_work_submit(&peripheral_event_work);
            }
        }
    }

    for (int i = 0; i < POSITION_STATE_DATA_LEN; i++) {
        slot->position_state[i] = 0U;
        slot->changed_positions[i] = 0U;
    }

    // Clean up previously discovered handles;
    slot->subscribe_params.value_handle = 0;
    slot->run_behavior_handle = 0;
    slot->selected_physical_layout_handle = 0;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
    slot->update_hid_indicators = 0;
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && IS_ENABLED(CONFIG_SHIELD_LPM_VIEW)
    slot->image_handle = 0;
    memset(&slot->image_sub, 0, sizeof(slot->image_sub));
#endif
    slot->relay_event_handle = 0;
    memset(&slot->relay_event_subscribe_params, 0, sizeof(slot->relay_event_subscribe_params));
    memset(&slot->relay_event_sub_discover_params, 0,
           sizeof(slot->relay_event_sub_discover_params));
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)

    return 0;
}

int reserve_peripheral_slot(const bt_addr_le_t *addr) {
    int i = zmk_ble_put_peripheral_addr(addr);
    if (i >= 0) {
        if (peripherals[i].state == PERIPHERAL_SLOT_STATE_OPEN) {
            // Be sure the slot is fully reinitialized.
            release_peripheral_slot(i);
            peripherals[i].state = PERIPHERAL_SLOT_STATE_CONNECTING;
            return i;
        }
    }

    return -ENOMEM;
}

int release_peripheral_slot_for_conn(struct bt_conn *conn) {
    int idx = peripheral_slot_index_for_conn(conn);
    if (idx < 0) {
        return idx;
    }

    return release_peripheral_slot(idx);
}

int confirm_peripheral_slot_conn(struct bt_conn *conn) {
    int idx = peripheral_slot_index_for_conn(conn);
    if (idx < 0) {
        return idx;
    }

    peripherals[idx].state = PERIPHERAL_SLOT_STATE_CONNECTED;
    return 0;
}

static void notify_transport_status(void);

static void notify_status_work_cb(struct k_work *_work) { notify_transport_status(); }

static K_WORK_DEFINE(notify_status_work, notify_status_work_cb);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)

static uint8_t split_central_relay_event_notify_func(struct bt_conn *conn,
                                                     struct bt_gatt_subscribe_params *params,
                                                     const void *data, uint16_t length) {
    if (!data) {
        LOG_DBG("[RELAY EVENT UNSUBSCRIBED]");
        params->value_handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    if (length != sizeof(struct zmk_split_relay_event_payload)) {
        LOG_WRN("Ignoring relay event notify with incorrect data length (%d)", length);
        return BT_GATT_ITER_CONTINUE;
    }

    struct peripheral_event_wrapper event_wrapper = {
        .source = peripheral_slot_index_for_conn(conn),
        .event = {.type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_RELAY_EVENT}};

    memcpy(&event_wrapper.event.data.relay_event, data,
           sizeof(event_wrapper.event.data.relay_event));
    // Never trust the peer's sizes; keep the event name terminated and reject
    // payloads that would not fit the buffers we own.
    event_wrapper.event.data.relay_event
        .event_type[CONFIG_ZMK_SPLIT_RELAY_EVENT_TYPE_NAME_LEN] = '\0';
    if (event_wrapper.event.data.relay_event.event_type_size >
            CONFIG_ZMK_SPLIT_RELAY_EVENT_TYPE_NAME_LEN ||
        event_wrapper.event.data.relay_event.event_data_size >
            CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN) {
        LOG_WRN("Malformed relay event notification");
        return BT_GATT_ITER_CONTINUE;
    }

    k_msgq_put(&peripheral_event_msgq, &event_wrapper, K_NO_WAIT);
    k_work_submit(&peripheral_event_work);

    return BT_GATT_ITER_CONTINUE;
}

#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)

#if ZMK_KEYMAP_HAS_SENSORS

static uint8_t split_central_sensor_notify_func(struct bt_conn *conn,
                                                struct bt_gatt_subscribe_params *params,
                                                const void *data, uint16_t length) {
    if (!data) {
        LOG_DBG("[UNSUBSCRIBED]");
        params->value_handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("[SENSOR NOTIFICATION] data %p length %u", data, length);

    if (length < offsetof(struct sensor_event, channel_data)) {
        LOG_WRN("Ignoring sensor notify with insufficient data length (%d)", length);
        return BT_GATT_ITER_STOP;
    }

    struct sensor_event sensor_event;
    memcpy(&sensor_event, data, MIN(length, sizeof(sensor_event)));
    if (sensor_event.channel_data_size != 1) {
        return BT_GATT_ITER_STOP;
    }

    struct peripheral_event_wrapper event_wrapper = {
        .source = peripheral_slot_index_for_conn(conn),
        .event = {.type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT,
                  .data = {.sensor_event = {
                               .channel_data = sensor_event.channel_data[0],
                               .sensor_index = sensor_event.sensor_index,
                           }}}};

    k_msgq_put(&peripheral_event_msgq, &event_wrapper, K_NO_WAIT);
    k_work_submit(&peripheral_event_work);

    return BT_GATT_ITER_CONTINUE;
}
#endif /* ZMK_KEYMAP_HAS_SENSORS */

#if IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT)

static int dispatch_split_input(uint8_t source, struct zmk_split_transport_peripheral_event event);

static uint8_t peripheral_input_event_notify_cb(struct bt_conn *conn,
                                                struct bt_gatt_subscribe_params *params,
                                                const void *data, uint16_t length) {
    if (!data) {
        LOG_DBG("[UNSUBSCRIBED]");
        params->value_handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("[INPUT EVENT] data %p length %u", data, length);

    if (length != sizeof(struct zmk_split_input_event_payload)) {
        LOG_WRN("Ignoring input event notify with incorrect data length (%d)", length);
        return BT_GATT_ITER_STOP;
    }

    struct zmk_split_input_event_payload payload;
    memcpy(&payload, data, MIN(length, sizeof(struct zmk_split_input_event_payload)));

    for (size_t i = 0; i < ARRAY_SIZE(peripheral_input_slots); i++) {
        if (&peripheral_input_slots[i].sub == params) {
            struct peripheral_event_wrapper event_wrapper = {
                .source = peripheral_slot_index_for_conn(conn),
                .event = {.type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,
                          .data = {.input_event = {
                                       .reg = peripheral_input_slots[i].reg,
                                       .sync = payload.sync,
                                       .code = payload.code,
                                       .type = payload.type,
                                       .value = payload.value,
                                   }}}};

#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) && IS_ENABLED(CONFIG_INPUT_MODE_THREAD)
            /* input_report(K_NO_WAIT) only queues the raw event to the existing
             * input thread. Do not run processors in BT RX, nor route it through
             * sysworkq first. Keep every input notification in arrival order. */
            atomic_inc(&keypoint_split_inputs);
            int err=dispatch_split_input(event_wrapper.source,event_wrapper.event);
            if(err)atomic_inc(&keypoint_split_input_drops);
#else
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN)
            atomic_inc(&keypoint_split_inputs);
            if(k_msgq_put(&peripheral_event_msgq, &event_wrapper, K_NO_WAIT))
                atomic_inc(&keypoint_split_input_drops);
#else
            k_msgq_put(&peripheral_event_msgq, &event_wrapper, K_NO_WAIT);
#endif
            k_work_submit(&peripheral_event_work);
#endif
            break;
        }
    }

    return BT_GATT_ITER_CONTINUE;
}

#endif

static uint8_t split_central_notify_func(struct bt_conn *conn,
                                         struct bt_gatt_subscribe_params *params, const void *data,
                                         uint16_t length) {
    struct peripheral_slot *slot = peripheral_slot_for_conn(conn);

    if (slot == NULL) {
        LOG_ERR("No peripheral state found for connection");
        return BT_GATT_ITER_CONTINUE;
    }

    if (!data) {
        LOG_DBG("[UNSUBSCRIBED]");
        params->value_handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("[NOTIFICATION] data %p length %u", data, length);

    for (int i = 0; i < POSITION_STATE_DATA_LEN; i++) {
        slot->changed_positions[i] = ((uint8_t *)data)[i] ^ slot->position_state[i];
        slot->position_state[i] = ((uint8_t *)data)[i];
    }
    LOG_HEXDUMP_DBG(slot->position_state, POSITION_STATE_DATA_LEN, "data");

    for (int i = 0; i < POSITION_STATE_DATA_LEN; i++) {
        for (int j = 0; j < 8; j++) {
            if (slot->changed_positions[i] & BIT(j)) {
                uint32_t position = (i * 8) + j;
                bool pressed = slot->position_state[i] & BIT(j);
                struct peripheral_event_wrapper ev = {
                    .source = peripheral_slot_index_for_conn(conn),
                    .event = {.type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
                              .data = {.key_position_event = {
                                           .position = position,
                                           .pressed = pressed,
                                       }}}};
                k_msgq_put(&peripheral_event_msgq, &ev, K_NO_WAIT);
                k_work_submit(&peripheral_event_work);
            }
        }
    }

    return BT_GATT_ITER_CONTINUE;
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)

#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE))
/* Input device IDs are fixed by the shields, unlike Bluetooth connection slots.
 * Keep early battery readings until CPF discovery identifies the physical side. */
static void dongle_display_battery_publish(struct peripheral_slot *slot, uint8_t level) {
    slot->display_battery_level = level;
    slot->display_battery_valid = true;
    if (slot->battery_side != 0) {
        raise_zmk_dongle_battery_state_changed((struct zmk_dongle_battery_state_changed){
            .side = slot->battery_side - 1, .level = level});
    }
}
static void dongle_display_battery_identify(struct peripheral_slot *slot, uint8_t reg) {
    if (reg == DT_REG_ADDR(DT_NODELABEL(bbtrackpad_split))) slot->battery_side = 1;
    else if (reg == DT_REG_ADDR(DT_NODELABEL(trackpoint_split))) slot->battery_side = 2;
    else return;
    if (slot->display_battery_valid) {
        dongle_display_battery_publish(slot, slot->display_battery_level);
    }
}
#endif

static uint8_t split_central_battery_level_notify_func(struct bt_conn *conn,
                                                       struct bt_gatt_subscribe_params *params,
                                                       const void *data, uint16_t length) {
    struct peripheral_slot *slot = peripheral_slot_for_conn(conn);

    if (!slot) {
        LOG_ERR("No peripheral state found for connection");
        return BT_GATT_ITER_CONTINUE;
    }

    if (!data) {
        LOG_DBG("[UNSUBSCRIBED]");
        params->value_handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    if (length == 0) {
        LOG_ERR("Zero length battery notification received");
        return BT_GATT_ITER_CONTINUE;
    }

    LOG_DBG("[BATTERY LEVEL NOTIFICATION] data %p length %u", data, length);
    uint8_t battery_level = ((uint8_t *)data)[0];
#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE))
    dongle_display_battery_publish(slot, battery_level);
#endif
    LOG_DBG("Battery level: %u", battery_level);

    struct peripheral_event_wrapper ev = {
        .source = peripheral_slot_index_for_conn(conn),
        .event = {.type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT,
                  .data = {.battery_event = {
                               .level = battery_level,
                           }}}};

    k_msgq_put(&peripheral_event_msgq, &ev, K_NO_WAIT);
    k_work_submit(&peripheral_event_work);

    return BT_GATT_ITER_CONTINUE;
}

static uint8_t split_central_battery_level_read_func(struct bt_conn *conn, uint8_t err,
                                                     struct bt_gatt_read_params *params,
                                                     const void *data, uint16_t length) {
    if (err > 0) {
        LOG_ERR("Error during reading peripheral battery level: %u", err);
        return BT_GATT_ITER_STOP;
    }

    struct peripheral_slot *slot = peripheral_slot_for_conn(conn);

    if (!slot) {
        LOG_ERR("No peripheral state found for connection");
        return BT_GATT_ITER_CONTINUE;
    }

    if (!data) {
        LOG_DBG("[READ COMPLETED]");
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("[BATTERY LEVEL READ] data %p length %u", data, length);

    if (length == 0) {
        LOG_ERR("Zero length battery notification received");
        return BT_GATT_ITER_CONTINUE;
    }

    uint8_t battery_level = ((uint8_t *)data)[0];
#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE))
    dongle_display_battery_publish(slot, battery_level);
#endif

    LOG_DBG("Battery level: %u", battery_level);

    struct peripheral_event_wrapper ev = {
        .source = peripheral_slot_index_for_conn(conn),
        .event = {.type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT,
                  .data = {.battery_event = {
                               .level = battery_level,
                           }}}};

    k_msgq_put(&peripheral_event_msgq, &ev, K_NO_WAIT);
    k_work_submit(&peripheral_event_work);

    return BT_GATT_ITER_CONTINUE;
}

#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING) */

static int split_central_subscribe(struct bt_conn *conn, struct bt_gatt_subscribe_params *params) {
    atomic_set(params->flags, BT_GATT_SUBSCRIBE_FLAG_NO_RESUB);
    int err = bt_gatt_subscribe(conn, params);
    switch (err) {
    case -EALREADY:
        LOG_DBG("[ALREADY SUBSCRIBED]");
        break;
    case 0:
        LOG_DBG("[SUBSCRIBED]");
        break;
    default:
        LOG_ERR("Subscribe failed (err %d)", err);
        break;
    }

    return err;
}

static int update_peripheral_selected_layout(struct peripheral_slot *slot, uint8_t layout_idx) {
    if (slot->state != PERIPHERAL_SLOT_STATE_CONNECTED) {
        return -ENOTCONN;
    }

    if (slot->selected_physical_layout_handle == 0) {
        // It appears that sometimes the peripheral is considered connected
        // before the GATT characteristics have been discovered. If this is
        // the case, the selected_physical_layout_handle will not yet be set.
        return -EAGAIN;
    }

    if (bt_conn_get_security(slot->conn) < BT_SECURITY_L2) {
        return -EAGAIN;
    }

    int err = bt_gatt_write_without_response(slot->conn, slot->selected_physical_layout_handle,
                                             &layout_idx, sizeof(layout_idx), true);

    if (err < 0) {
        LOG_ERR("Failed to write physical layout index to peripheral (err %d)", err);
    }

    return err;
}

static void update_peripherals_selected_physical_layout(struct k_work *_work) {
    uint8_t layout_idx = zmk_physical_layouts_get_selected();
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        if (peripherals[i].state != PERIPHERAL_SLOT_STATE_CONNECTED) {
            continue;
        }

        update_peripheral_selected_layout(&peripherals[i], layout_idx);
    }
}

K_WORK_DEFINE(update_peripherals_selected_layouts_work,
              update_peripherals_selected_physical_layout);

#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && IS_ENABLED(CONFIG_SHIELD_LPM_VIEW)
static void image_mtu_exchanged(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params) {
    LOG_INF("Image channel MTU %u (err %u)",bt_gatt_get_mtu(conn),err);
}
static uint8_t image_ack_notify(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                               const void *data, uint16_t len) {
    if (!data) { params->value_handle=0; return BT_GATT_ITER_STOP; }
    zitaotech_image_ack_receive(data,len);
    return BT_GATT_ITER_CONTINUE;
}
struct image_link_op { const void *data; size_t len; int result; bool write; };
static void image_link_conn(struct bt_conn *conn, void *user_data) {
    struct image_link_op *op=user_data;
    struct peripheral_slot *slot=&peripherals[0];
    if (conn != slot->conn || !slot->image_handle || !slot->image_sub.value_handle) return;
    /* bt_conn_foreach holds a connection reference across this callback. */
    op->result=op->write ? bt_gatt_write_without_response(conn,slot->image_handle,
                                                         op->data,op->len,false)
                         : bt_gatt_get_mtu(conn);
}
int zitaotech_image_link_mtu(void) {
    struct image_link_op op={.result=-ENOTCONN};
    bt_conn_foreach(BT_CONN_TYPE_LE,image_link_conn,&op);
    return op.result;
}
int zitaotech_image_link_send(const void *data, size_t len) {
    struct image_link_op op={.data=data,.len=len,.result=-ENOTCONN,.write=true};
    bt_conn_foreach(BT_CONN_TYPE_LE,image_link_conn,&op);
    return op.result;
}
#endif
static uint8_t split_central_chrc_discovery_func(struct bt_conn *conn,
                                                 const struct bt_gatt_attr *attr,
                                                 struct bt_gatt_discover_params *params) {
    if (!attr) {
        LOG_DBG("Discover complete");
        return BT_GATT_ITER_STOP;
    }

    /*
     * Only characteristic declarations have to carry a value here.  Descriptor
     * discoveries (the input-split CCC/CPF dance) can legitimately come back
     * empty: the peripheral's CPF descriptor stores the input device's `reg` in
     * user_data, so reg 0 is indistinguishable from "no data".  Bailing out for
     * those used to abort the whole walk, silently leaving every later handle
     * -- HID indicators, physical layout and the event-relay pair -- undiscovered.
     */
    if (!attr->user_data && params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
        LOG_ERR("Required user data not passed to discovery");
        return BT_GATT_ITER_STOP;
    }

    struct peripheral_slot *slot = peripheral_slot_for_conn(conn);
    if (slot == NULL) {
        LOG_ERR("No peripheral state found for connection");
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("[ATTRIBUTE] handle %u", attr->handle);
    const uint8_t discovery_type = params->type;
    const struct bt_uuid *discovery_uuid = params->uuid;
    switch (params->type) {
    case BT_GATT_DISCOVER_CHARACTERISTIC: {
        const struct bt_uuid *chrc_uuid = ((struct bt_gatt_chrc *)attr->user_data)->uuid;

        if (bt_uuid_cmp(chrc_uuid, BT_UUID_DECLARE_128(ZMK_SPLIT_BT_CHAR_POSITION_STATE_UUID)) ==
            0) {
            LOG_DBG("Found position state characteristic");
            slot->subscribe_params.disc_params = &slot->sub_discover_params;
            slot->subscribe_params.end_handle = slot->discover_params.end_handle;
            slot->subscribe_params.value_handle = bt_gatt_attr_value_handle(attr);
            slot->subscribe_params.notify = split_central_notify_func;
            slot->subscribe_params.value = BT_GATT_CCC_NOTIFY;
            split_central_subscribe(conn, &slot->subscribe_params);
#if ZMK_KEYMAP_HAS_SENSORS
        } else if (bt_uuid_cmp(chrc_uuid,
                               BT_UUID_DECLARE_128(ZMK_SPLIT_BT_CHAR_SENSOR_STATE_UUID)) == 0) {
            slot->discover_params.uuid = NULL;
            slot->discover_params.start_handle = attr->handle + 2;
            slot->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

            slot->sensor_subscribe_params.disc_params = &slot->sub_discover_params;
            slot->sensor_subscribe_params.end_handle = slot->discover_params.end_handle;
            slot->sensor_subscribe_params.value_handle = bt_gatt_attr_value_handle(attr);
            slot->sensor_subscribe_params.notify = split_central_sensor_notify_func;
            slot->sensor_subscribe_params.value = BT_GATT_CCC_NOTIFY;
            split_central_subscribe(conn, &slot->sensor_subscribe_params);
#endif /* ZMK_KEYMAP_HAS_SENSORS */
#if IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT)
        } else if (bt_uuid_cmp(chrc_uuid, BT_UUID_DECLARE_128(ZMK_SPLIT_BT_INPUT_EVENT_UUID)) ==
                   0) {
            LOG_DBG("Found an input characteristic");
            struct peripheral_input_slot *input_slot;
            int ret = reserve_next_open_input_slot(&input_slot, conn);
            if (ret < 0) {
                LOG_WRN("No available slot for peripheral input subscriptions (%d)", ret);

                slot->discover_params.uuid = NULL;
                slot->discover_params.start_handle = attr->handle + 1;
                slot->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
            } else {
                LOG_DBG("Reserved a slot for the input subscription");
                input_slot->sub.value_handle = bt_gatt_attr_value_handle(attr);

                slot->discover_params.uuid = gatt_ccc_uuid;
                slot->discover_params.start_handle = attr->handle;
                slot->discover_params.type = BT_GATT_DISCOVER_STD_CHAR_DESC;
            }
#endif // IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT)
        } else if (bt_uuid_cmp(chrc_uuid,
                               BT_UUID_DECLARE_128(ZMK_SPLIT_BT_CHAR_RUN_BEHAVIOR_UUID)) == 0) {
            LOG_DBG("Found run behavior handle");
            slot->discover_params.uuid = NULL;
            slot->discover_params.start_handle = attr->handle + 2;
            slot->run_behavior_handle = bt_gatt_attr_value_handle(attr);
        } else if (!bt_uuid_cmp(((struct bt_gatt_chrc *)attr->user_data)->uuid,
                                BT_UUID_DECLARE_128(ZMK_SPLIT_BT_SELECT_PHYS_LAYOUT_UUID))) {
            LOG_DBG("Found select physical layout handle");
            slot->selected_physical_layout_handle = bt_gatt_attr_value_handle(attr);
            k_work_submit(&update_peripherals_selected_layouts_work);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
        } else if (!bt_uuid_cmp(((struct bt_gatt_chrc *)attr->user_data)->uuid,
                                BT_UUID_DECLARE_128(ZMK_SPLIT_BT_UPDATE_HID_INDICATORS_UUID))) {
            LOG_DBG("Found update HID indicators handle");
            slot->update_hid_indicators = bt_gatt_attr_value_handle(attr);
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && IS_ENABLED(CONFIG_SHIELD_LPM_VIEW)
        } else if (!bt_uuid_cmp(((struct bt_gatt_chrc *)attr->user_data)->uuid,
                               BT_UUID_DECLARE_128(ZMK_SPLIT_BT_CHAR_IMAGE_UUID))) {
            slot->image_handle=bt_gatt_attr_value_handle(attr);
        } else if (!bt_uuid_cmp(((struct bt_gatt_chrc *)attr->user_data)->uuid,
                               BT_UUID_DECLARE_128(ZMK_SPLIT_BT_CHAR_IMAGE_ACK_UUID))) {
            slot->image_sub.value_handle=bt_gatt_attr_value_handle(attr);
            slot->image_sub.ccc_handle=slot->image_sub.value_handle+1;
            slot->image_sub.end_handle=slot->discover_params.end_handle;
            slot->image_sub.notify=image_ack_notify;
            slot->image_sub.value=BT_GATT_CCC_NOTIFY;
            split_central_subscribe(conn,&slot->image_sub);
            slot->image_mtu_params.func=image_mtu_exchanged;
            bt_gatt_exchange_mtu(conn,&slot->image_mtu_params);
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
        } else if (!bt_uuid_cmp(((struct bt_gatt_chrc *)attr->user_data)->uuid,
                                BT_UUID_DECLARE_128(ZMK_SPLIT_BT_CHAR_RELAY_EVENT_UUID))) {
            LOG_DBG("Found relay event handle");
            slot->relay_event_handle = bt_gatt_attr_value_handle(attr);
        } else if (!bt_uuid_cmp(((struct bt_gatt_chrc *)attr->user_data)->uuid,
                                BT_UUID_DECLARE_128(
                                    ZMK_SPLIT_BT_CHAR_RELAY_EVENT_NOTIFY_UUID))) {
            LOG_DBG("Found relay event notify characteristic");
            slot->discover_params.uuid = NULL;
            slot->discover_params.start_handle = attr->handle + 2;
            slot->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

            slot->relay_event_subscribe_params.disc_params =
                &slot->relay_event_sub_discover_params;
            slot->relay_event_subscribe_params.end_handle = slot->discover_params.end_handle;
            slot->relay_event_subscribe_params.value_handle = bt_gatt_attr_value_handle(attr);
            // The split service declares the notify characteristic followed
            // immediately by its CCC descriptor, so the descriptor handle is
            // deterministic.  Writing it directly skips the asynchronous CCC
            // discovery, which would otherwise be queued behind the sensor
            // subscription's own discovery and rejected with -EBUSY.
            slot->relay_event_subscribe_params.ccc_handle =
                bt_gatt_attr_value_handle(attr) + 1;
            slot->relay_event_subscribe_params.notify = split_central_relay_event_notify_func;
            slot->relay_event_subscribe_params.value = BT_GATT_CCC_NOTIFY;
            split_central_subscribe(conn, &slot->relay_event_subscribe_params);
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
        } else if (!bt_uuid_cmp(((struct bt_gatt_chrc *)attr->user_data)->uuid,
                                BT_UUID_BAS_BATTERY_LEVEL)) {
            LOG_DBG("Found battery level characteristics");
            slot->batt_lvl_subscribe_params.disc_params = &slot->sub_discover_params;
            slot->batt_lvl_subscribe_params.end_handle = slot->discover_params.end_handle;
            slot->batt_lvl_subscribe_params.value_handle = bt_gatt_attr_value_handle(attr);
            slot->batt_lvl_subscribe_params.notify = split_central_battery_level_notify_func;
            slot->batt_lvl_subscribe_params.value = BT_GATT_CCC_NOTIFY;
            split_central_subscribe(conn, &slot->batt_lvl_subscribe_params);

            slot->batt_lvl_read_params.func = split_central_battery_level_read_func;
            slot->batt_lvl_read_params.handle_count = 1;
            slot->batt_lvl_read_params.single.handle = bt_gatt_attr_value_handle(attr);
            slot->batt_lvl_read_params.single.offset = 0;
            bt_gatt_read(conn, &slot->batt_lvl_read_params);
#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING) */
        }
        break;
    }
    case BT_GATT_DISCOVER_STD_CHAR_DESC:
#if IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT)
        if (bt_uuid_cmp(slot->discover_params.uuid, BT_UUID_GATT_CCC) == 0) {
            LOG_DBG("Found input CCC descriptor");
            struct peripheral_input_slot *input_slot;
            int ret = find_pending_input_slot(&input_slot, conn);
            if (ret < 0) {
                LOG_DBG("No pending input slot (%d)", ret);
                slot->discover_params.uuid = NULL;
                slot->discover_params.start_handle = attr->handle + 1;
                slot->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
            } else {
                LOG_DBG("Found pending input slot");
                input_slot->sub.ccc_handle = attr->handle;

                slot->discover_params.uuid = gatt_cpf_uuid;
                slot->discover_params.start_handle = attr->handle + 1;
                slot->discover_params.type = BT_GATT_DISCOVER_STD_CHAR_DESC;
            }
        } else if (bt_uuid_cmp(slot->discover_params.uuid, BT_UUID_GATT_CPF) == 0) {
            LOG_DBG("Found input CPF descriptor");
            struct bt_gatt_cpf *cpf = attr->user_data;
            struct peripheral_input_slot *input_slot;
            int ret = find_pending_input_slot(&input_slot, conn);
            if (ret < 0) {
                LOG_DBG("No pending input slot (%d)", ret);
            } else {
                LOG_DBG("Found pending input slot");
                // The peripheral hands us the input device's reg in the CPF
                // description, and reg 0 reads back as an empty value, so treat
                // a missing CPF payload as reg 0 instead of dereferencing NULL.
                input_slot->reg = cpf ? cpf->description : 0;
                input_slot->reg_valid = true;
#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE)) && IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
                dongle_display_battery_identify(slot, input_slot->reg);
#endif
                input_slot->sub.notify = peripheral_input_event_notify_cb;
                input_slot->sub.value = BT_GATT_CCC_NOTIFY;
                int err = split_central_subscribe(conn, &input_slot->sub);
                if (err < 0) {
                    LOG_WRN("Failed to subscribe to input notifications %d", err);
                }
            }

            slot->discover_params.uuid = NULL;
            slot->discover_params.start_handle = attr->handle + 1;
            slot->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
        }
#endif // IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT)
        break;
    }

    bool subscribed = slot->run_behavior_handle && slot->subscribe_params.value_handle &&
                      slot->selected_physical_layout_handle;

#if ZMK_KEYMAP_HAS_SENSORS
    subscribed = subscribed && slot->sensor_subscribe_params.value_handle;
#endif /* ZMK_KEYMAP_HAS_SENSORS */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
    subscribed = subscribed && slot->update_hid_indicators;
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
    // Keep walking until the event-relay pair shows up.  A peripheral without
    // them (older firmware) just runs to the end of the service.
    subscribed =
        subscribed && slot->relay_event_handle && slot->relay_event_subscribe_params.value_handle;
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    subscribed = subscribed && slot->batt_lvl_subscribe_params.value_handle;
#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING) */
#if IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT)
    for (size_t i = 0; i < ARRAY_SIZE(peripheral_input_slots); i++) {
        if (input_slot_is_open(i) || input_slot_is_pending(i)) {
            subscribed = false;
            break;
        }
    }
#endif // IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT)

    /* A large MTU can put several attributes in one response. Stop parsing
     * that response when switching from characteristics to descriptors (or
     * CCC to CPF), and issue the next discovery explicitly. Otherwise the
     * next attribute in the old response is interpreted as the new type. */
    if (params->type != discovery_type || params->uuid != discovery_uuid) {
        int err = bt_gatt_discover(conn, params);
        if (err) {
            LOG_ERR("Failed to continue split discovery (%d)", err);
        }
        return BT_GATT_ITER_STOP;
    }
    return subscribed ? BT_GATT_ITER_STOP : BT_GATT_ITER_CONTINUE;
}

static uint8_t split_central_service_discovery_func(struct bt_conn *conn,
                                                    const struct bt_gatt_attr *attr,
                                                    struct bt_gatt_discover_params *params) {
    if (!attr) {
        LOG_DBG("Discover complete");
        (void)memset(params, 0, sizeof(*params));
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("[ATTRIBUTE] handle %u", attr->handle);

    struct peripheral_slot *slot = peripheral_slot_for_conn(conn);
    if (slot == NULL) {
        LOG_ERR("No peripheral state found for connection");
        return BT_GATT_ITER_STOP;
    }

    if (bt_uuid_cmp(slot->discover_params.uuid, BT_UUID_DECLARE_128(ZMK_SPLIT_BT_SERVICE_UUID)) !=
        0) {
        LOG_DBG("Found other service");
        return BT_GATT_ITER_CONTINUE;
    }

    LOG_DBG("Found split service");
    slot->discover_params.uuid = NULL;
    slot->discover_params.func = split_central_chrc_discovery_func;
    slot->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

    int err = bt_gatt_discover(conn, &slot->discover_params);
    if (err) {
        LOG_ERR("Failed to start discovering split service characteristics (err %d)", err);
    }
    return BT_GATT_ITER_STOP;
}

static void split_central_process_connection(struct bt_conn *conn) {
    int err;

    LOG_DBG("Current security for connection: %d", bt_conn_get_security(conn));

    struct peripheral_slot *slot = peripheral_slot_for_conn(conn);
    if (slot == NULL) {
        LOG_ERR("No peripheral state found for connection");
        return;
    }

    if (!slot->subscribe_params.value_handle) {
        slot->discover_params.uuid = &split_service_uuid.uuid;
        slot->discover_params.func = split_central_service_discovery_func;
        slot->discover_params.start_handle = 0x0001;
        slot->discover_params.end_handle = 0xffff;
        slot->discover_params.type = BT_GATT_DISCOVER_PRIMARY;

        err = bt_gatt_discover(slot->conn, &slot->discover_params);
        if (err) {
            LOG_ERR("Discover failed(err %d)", err);
            return;
        }
    }

    struct bt_conn_info info;

    bt_conn_get_info(conn, &info);

    LOG_DBG("New connection params: Interval: %d, Latency: %d, PHY: %d", info.le.interval,
            info.le.latency, info.le.phy->rx_phy);

    // Restart scanning if necessary.
    start_scanning();
}

static int stop_scanning(void) {
    LOG_DBG("Stopping peripheral scanning");

    int err = bt_le_scan_stop();
    if (err < 0 && err != -EALREADY) {
        LOG_ERR("Stop LE scan failed (err %d)", err);
        return err;
    }

    is_scanning = false;
    return 0;
}

static bool split_central_eir_found(const bt_addr_le_t *addr) {
    LOG_DBG("Found the split service");

    // Reserve peripheral slot. Once the central has bonded to its peripherals,
    // the peripheral MAC addresses will be validated internally and the slot
    // reservation will fail if there is a mismatch.
    int slot_idx = reserve_peripheral_slot(addr);
    if (slot_idx < 0) {
        LOG_INF("Unable to reserve peripheral slot (err %d)", slot_idx);
        return false;
    }
    struct peripheral_slot *slot = &peripherals[slot_idx];

    // Stop scanning so we can connect to the peripheral device.
    int err = stop_scanning();
    if (err < 0) {
        release_peripheral_slot(slot_idx);
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_PEER_SELECTION)
        k_work_reschedule(&scan_retry_work, K_MSEC(1000));
#endif
        return false;
    }

    LOG_DBG("Initiating new connection");
    struct bt_le_conn_param *param =
        BT_LE_CONN_PARAM(CONFIG_ZMK_SPLIT_BLE_PREF_INT, CONFIG_ZMK_SPLIT_BLE_PREF_INT,
                         CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY, CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT);
    err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, param, &slot->conn);
    if (err < 0) {
        LOG_ERR("Create conn failed (err %d) (create conn? 0x%04x)", err, BT_HCI_OP_LE_CREATE_CONN);
        release_peripheral_slot(slot_idx);
        start_scanning();
    }

    return false;
}

static bool split_central_eir_parse(struct bt_data *data, void *user_data) {
    bt_addr_le_t *addr = user_data;
    int i;

    LOG_DBG("[AD]: %u data_len %u", data->type, data->data_len);

    switch (data->type) {
    case BT_DATA_UUID128_SOME:
    case BT_DATA_UUID128_ALL:
        if (data->data_len % 16 != 0U) {
            LOG_ERR("AD malformed");
            return true;
        }

        for (i = 0; i < data->data_len; i += 16) {
            struct bt_uuid_128 uuid;

            if (!bt_uuid_create(&uuid.uuid, &data->data[i], 16)) {
                LOG_ERR("Unable to load UUID");
                continue;
            }

            if (bt_uuid_cmp(&uuid.uuid, BT_UUID_DECLARE_128(ZMK_SPLIT_BT_SERVICE_UUID)) != 0) {
                char uuid_str[BT_UUID_STR_LEN];
                char service_uuid_str[BT_UUID_STR_LEN];

                bt_uuid_to_str(&uuid.uuid, uuid_str, sizeof(uuid_str));
                bt_uuid_to_str(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_SERVICE_UUID), service_uuid_str,
                               sizeof(service_uuid_str));
                LOG_DBG("UUID %s does not match split UUID: %s", uuid_str, service_uuid_str);
                continue;
            }

            return split_central_eir_found(addr);
        }
    }

    return true;
}

#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_PEER_SELECTION)
struct peer_ad_check { bool found; bool matches; };
static bool peer_ad_check_target(struct bt_data *data, void *user_data) {
    struct peer_ad_check *check = user_data;
    if (data->type != BT_DATA_MANUFACTURER_DATA || data->data_len != 6 ||
        data->data[0] != 0xff || data->data[1] != 0xff || data->data[2] != 0xd1) return true;
    check->found = true;
    uint8_t local = zmk_keypoint_local_peer_kind();
    check->matches = data->data[3] == 1 && data->data[4] == local &&
        (data->data[5] == ZMK_KEYPOINT_PEER_RIGHT ||
         (local == ZMK_KEYPOINT_PEER_DONGLE && data->data[5] == ZMK_KEYPOINT_PEER_LEFT));
    return false;
}
#endif

static void split_central_device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                                       struct net_buf_simple *ad) {
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_CLIENT)
    if ((type==BT_GAP_ADV_TYPE_ADV_DIRECT_IND || type==BT_GAP_ADV_TYPE_ADV_IND) &&
        zmk_keypoint_presence_probe_advertisement(addr)) { return; }
#endif
    char dev[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(addr, dev, sizeof(dev));
    LOG_DBG("[DEVICE]: %s, AD evt type %u, AD data len %u, RSSI %i", dev, type, ad->len, rssi);

    /* We're only interested in connectable events */
    if (type == BT_GAP_ADV_TYPE_ADV_IND) {
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_PEER_SELECTION)
        struct net_buf_simple copy = *ad;
        struct peer_ad_check check = {0};
        bt_data_parse(&copy, peer_ad_check_target, &check);
        if (!check.found || !check.matches) return;
#endif
        bt_data_parse(ad, split_central_eir_parse, (void *)addr);
    } else if (type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
        split_central_eir_found(addr);
    }
}

static int start_scanning(void) {
    if (IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER)) return 0;
    if (!is_enabled) {
        LOG_DBG("Not scanning, we're disabled");
        return 0;
    }

    // No action is necessary if central is already scanning.
    if (is_scanning) {
        LOG_DBG("Scanning already running");
        return 0;
    }

    // If all the devices are connected, there is no need to scan.
    bool split_peripheral_missing = false;
    for (int i = 0; i < CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS; i++) {
        if (peripherals[i].conn == NULL) {
            split_peripheral_missing = true;
            break;
        }
    }
    bool has_unconnected = split_peripheral_missing;
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_CLIENT)
    has_unconnected=has_unconnected || !zmk_split_role_standalone_selected();
#endif
    if (!has_unconnected) {
        LOG_DBG("All devices are connected, scanning is unnecessary");
        return 0;
    }

    // Start scanning otherwise.
    is_scanning = true;
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_PEER_SELECTION)
    struct bt_le_scan_param scan = {
        .type = BT_LE_SCAN_TYPE_PASSIVE,
        .interval = 1600, /* 1000 ms, in 0.625 ms units */
        .window = 16,     /* 10 ms */
    };
#if defined(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT)
    /* Sample more often while the right is missing. Keep presence-only scans sparse. */
    if (split_peripheral_missing) {
        scan.interval = 320; /* 200 ms */
        scan.window = 8;     /* 5 ms */
    }
#endif
    int err = bt_le_scan_start(&scan, split_central_device_found);
#else
    int err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, split_central_device_found);
#endif
    if (err == -EALREADY) return 0;
    if (err < 0) {
        is_scanning = false;
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_PEER_SELECTION)
        k_work_reschedule(&scan_retry_work, K_MSEC(1000));
#endif
        LOG_ERR("Scanning failed to start (err %d)", err);
        return err;
    }

    LOG_DBG("Scanning successfully started");
    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_CLIENT)
int zmk_keypoint_presence_scan_pause(void) { return stop_scanning(); }
int zmk_keypoint_presence_scan_resume(void) { return start_scanning(); }
#endif

static void split_central_connected(struct bt_conn *conn, uint8_t conn_err) {
    if (IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER)) return;
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_CLIENT)
    if (zmk_keypoint_presence_probe_connection(conn)) { return; }
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RUNTIME_ROLE)
    if (!zmk_split_role_is_central()) {
        return;
    }
#endif
    char addr[BT_ADDR_LE_STR_LEN];
    struct bt_conn_info info;

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    bt_conn_get_info(conn, &info);

    if (info.role != BT_CONN_ROLE_CENTRAL) {
        LOG_DBG("SKIPPING FOR ROLE %d", info.role);
        return;
    }

    if (conn_err) {
        LOG_ERR("Failed to connect to %s (%u)", addr, conn_err);

        release_peripheral_slot_for_conn(conn);

        start_scanning();
        return;
    }

    LOG_DBG("Connected: %s", addr);

    confirm_peripheral_slot_conn(conn);
    split_central_process_connection(conn);
    k_work_submit(&notify_status_work);
}

static void split_central_disconnected(struct bt_conn *conn, uint8_t reason) {
    if (IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER)) return;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RUNTIME_ROLE)
    /* Host and dongle connections share Bluetooth callbacks. They are not
     * right-hand slots and must never generate a peripheral battery event. */
    if (!zmk_split_role_is_central() || !peripheral_slot_for_conn(conn)) {
        return;
    }
#endif
    char addr[BT_ADDR_LE_STR_LEN];
    int err;

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_DBG("Disconnected: %s (reason %d)", addr, reason);
#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE)) && IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    struct peripheral_slot *display_slot = peripheral_slot_for_conn(conn);
    if (display_slot) dongle_display_battery_publish(display_slot, 0);
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    struct peripheral_event_wrapper ev = {
        .source = peripheral_slot_index_for_conn(conn),
        .event = {.type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT,
                  .data = {.battery_event = {
                               .level = 0,
                           }}}};

    k_msgq_put(&peripheral_event_msgq, &ev, K_NO_WAIT);
    k_work_submit(&peripheral_event_work);
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)

#if IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT)
    release_peripheral_input_subs(conn);
#endif

    err = release_peripheral_slot_for_conn(conn);

    if (err < 0) {
        LOG_WRN("Failed to release peripheral slot (%d)", err);
    }

    k_work_submit(&notify_status_work);

    start_scanning();
}

static void split_central_security_changed(struct bt_conn *conn, bt_security_t level,
                                           enum bt_security_err err) {
    struct peripheral_slot *slot = peripheral_slot_for_conn(conn);
    if (!slot || !slot->selected_physical_layout_handle) {
        return;
    }

    if (err > 0) {
        LOG_DBG("Skipping updating the physical layout for peripheral with security error");
        return;
    }

    if (level < BT_SECURITY_L2) {
        LOG_DBG("Skipping updating the physical layout for peripheral with insufficient security");
        return;
    }

    k_work_submit(&update_peripherals_selected_layouts_work);
}

static struct bt_conn_cb conn_callbacks = {
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_PEER_SELECTION)
    .recycled = split_central_recycled,
#endif
    .connected = split_central_connected,
    .disconnected = split_central_disconnected,
    .security_changed = split_central_security_changed,
};

K_THREAD_STACK_DEFINE(split_central_split_run_q_stack,
                      CONFIG_ZMK_SPLIT_BLE_CENTRAL_SPLIT_RUN_STACK_SIZE);

struct k_work_q split_central_split_run_q;

struct central_cmd_wrapper {
    uint8_t source;
    struct zmk_split_transport_central_command cmd;
};

K_MSGQ_DEFINE(zmk_split_central_split_run_msgq, sizeof(struct central_cmd_wrapper),
              CONFIG_ZMK_SPLIT_BLE_CENTRAL_SPLIT_RUN_QUEUE_SIZE, 4);

void split_central_split_run_callback(struct k_work *work) {
    struct central_cmd_wrapper payload_wrapper;

    LOG_DBG("");

    while (k_msgq_get(&zmk_split_central_split_run_msgq, &payload_wrapper, K_NO_WAIT) == 0) {
        if (peripherals[payload_wrapper.source].state != PERIPHERAL_SLOT_STATE_CONNECTED) {
            LOG_ERR("Source not connected");
            continue;
        }

        switch (payload_wrapper.cmd.type) {
        case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_INVOKE_BEHAVIOR: {
            if (!peripherals[payload_wrapper.source].run_behavior_handle) {
                LOG_ERR("Run behavior handle not found");
                continue;
            }

            struct zmk_split_run_behavior_payload payload = {
                .data = {
                    .param1 = payload_wrapper.cmd.data.invoke_behavior.param1,
                    .param2 = payload_wrapper.cmd.data.invoke_behavior.param2,
                    .position = payload_wrapper.cmd.data.invoke_behavior.position,
                    .source = payload_wrapper.cmd.data.invoke_behavior.event_source,
                    .state = payload_wrapper.cmd.data.invoke_behavior.state ? 1 : 0,
                }};
            const size_t payload_dev_size = sizeof(payload.behavior_dev);
            if (strlcpy(payload.behavior_dev, payload_wrapper.cmd.data.invoke_behavior.behavior_dev,
                        payload_dev_size) >= payload_dev_size) {
                LOG_ERR("Truncated behavior label %s to %s before invoking peripheral behavior",
                        payload_wrapper.cmd.data.invoke_behavior.behavior_dev,
                        payload.behavior_dev);
            }

            int err = bt_gatt_write_without_response(
                peripherals[payload_wrapper.source].conn,
                peripherals[payload_wrapper.source].run_behavior_handle, &payload,
                sizeof(struct zmk_split_run_behavior_payload), true);

            if (err) {
                LOG_ERR("Failed to write the behavior characteristic (err %d)", err);
            }
            break;
        }
        case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_PHYSICAL_LAYOUT:
            update_peripheral_selected_layout(
                &peripherals[payload_wrapper.source],
                payload_wrapper.cmd.data.set_physical_layout.layout_idx);
            break;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
        case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_HID_INDICATORS:
            LOG_WRN("do the indicators dance");
            if (peripherals[payload_wrapper.source].update_hid_indicators == 0) {
                // It appears that sometimes the peripheral is considered connected
                // before the GATT characteristics have been discovered. If this is
                // the case, the update_hid_indicators handle will not yet be set.
                LOG_WRN("NO HANDLE TO SET ON PERIPHERAL");
                break;
            }

            int err = bt_gatt_write_without_response(
                peripherals[payload_wrapper.source].conn,
                peripherals[payload_wrapper.source].update_hid_indicators,
                &payload_wrapper.cmd.data.set_hid_indicators.indicators,
                sizeof(payload_wrapper.cmd.data.set_hid_indicators.indicators), true);

            if (err) {
                LOG_ERR("Failed to write HID indicator characteristic (err %d)", err);
            }
            break;
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
        case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_RELAY_EVENT: {
            if (peripherals[payload_wrapper.source].relay_event_handle == 0) {
                // Same caveat as the HID indicator handle above: the peripheral
                // can be marked connected before the characteristics are
                // discovered.
                LOG_WRN("No relay event handle on peripheral %d", payload_wrapper.source);
                break;
            }

            int err = bt_gatt_write_without_response(
                peripherals[payload_wrapper.source].conn,
                peripherals[payload_wrapper.source].relay_event_handle,
                &payload_wrapper.cmd.data.relay_event, sizeof(payload_wrapper.cmd.data.relay_event),
                true);

            if (err) {
                LOG_ERR("Failed to write relay event characteristic (err %d)", err);
            }
            break;
        }
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
        default:
            LOG_WRN("Unsupported wrapped central command type %d", payload_wrapper.cmd.type);
            return;
        }
    }
}

K_WORK_DEFINE(split_central_split_run_work, split_central_split_run_callback);

static int split_bt_invoke_behavior_payload(struct central_cmd_wrapper payload_wrapper) {
    LOG_DBG("");

    int err = k_msgq_put(&zmk_split_central_split_run_msgq, &payload_wrapper, K_MSEC(100));
    if (err) {
        switch (err) {
        case -EAGAIN: {
            LOG_WRN("Run command message queue full, popping first message and queueing again");
            struct central_cmd_wrapper discarded_report;
            k_msgq_get(&zmk_split_central_split_run_msgq, &discarded_report, K_NO_WAIT);
            return split_bt_invoke_behavior_payload(payload_wrapper);
        }
        default:
            LOG_WRN("Failed to queue behavior to send (%d)", err);
            return err;
        }
    }

    k_work_submit_to_queue(&split_central_split_run_q, &split_central_split_run_work);

    return 0;
};

static int finish_init();

static bool settings_loaded = false;

#if IS_ENABLED(CONFIG_SETTINGS)

static int central_ble_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    return 0;
}

static struct settings_handler ble_central_settings_handler = {
    .name = "ble_central", .h_set = central_ble_handle_set, .h_commit = finish_init};

#endif // IS_ENABLED(CONFIG_SETTINGS)

static int zmk_split_bt_central_init(void) {
    k_work_queue_start(&split_central_split_run_q, split_central_split_run_q_stack,
                       K_THREAD_STACK_SIZEOF(split_central_split_run_q_stack),
                       CONFIG_ZMK_BLE_THREAD_PRIORITY, NULL);
    bt_conn_cb_register(&conn_callbacks);

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_register(&ble_central_settings_handler);
    return 0;
#else
    return finish_init();
#endif // IS_ENABLED(CONFIG_SETTINGS)
}

SYS_INIT(zmk_split_bt_central_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);

static int zmk_split_bt_central_listener_cb(const zmk_event_t *eh) {
    if (as_zmk_physical_layout_selection_changed(eh)) {
        k_work_submit(&update_peripherals_selected_layouts_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_split_bt_central, zmk_split_bt_central_listener_cb);
ZMK_SUBSCRIPTION(zmk_split_bt_central, zmk_physical_layout_selection_changed);

static int split_central_bt_send_command(uint8_t source,
                                         struct zmk_split_transport_central_command cmd) {
    if (source >= ARRAY_SIZE(peripherals)) {
        return -EINVAL;
    }

    switch (cmd.type) {
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_HID_INDICATORS:
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_PHYSICAL_LAYOUT:
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_RELAY_EVENT:
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_INVOKE_BEHAVIOR: {
        struct central_cmd_wrapper wrapper = {.source = source, .cmd = cmd};
        return split_bt_invoke_behavior_payload(wrapper);
    }
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_POLL_EVENTS:
        return -ENOTSUP;
    default:
        return -ENOTSUP;
    }

    return 0;
}

static int split_central_bt_get_available_source_ids(uint8_t *sources) {
    int count = 0;
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        if (peripherals[i].state != PERIPHERAL_SLOT_STATE_CONNECTED) {
            continue;
        }

        sources[count++] = i;
    }

    return count;
}

static int split_central_bt_set_enabled(bool enabled) {
    is_enabled = enabled;
    if (enabled) {
        return start_scanning();
    } else {
        int err = stop_scanning();
        if (err < 0) {
            LOG_WRN("Failed to stop scanning for peripherals (%d)", err);
        }

        for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
            if (peripherals[i].state != PERIPHERAL_SLOT_STATE_CONNECTED) {
                continue;
            }

            err = bt_conn_disconnect(peripherals[i].conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            if (err < 0) {
                LOG_WRN("Failed to disconnect a peripheral (%d)", err);
            }
        }

        return 0;
    }
}

static int
split_central_bt_set_status_callback(zmk_split_transport_central_status_changed_cb_t cb) {
    transport_status_cb = cb;
    return 0;
}

static struct zmk_split_transport_status split_central_bt_get_status() {
    uint8_t _source_ids[ZMK_SPLIT_BLE_PERIPHERAL_COUNT];

    int count = split_central_bt_get_available_source_ids(_source_ids);

    enum zmk_split_transport_connections_status conn_status;

    if (count == 0) {
        conn_status = ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED;
    } else if (count == ZMK_SPLIT_BLE_PERIPHERAL_COUNT) {
        conn_status = ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED;
    } else {
        conn_status = ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED;
    }

    return (struct zmk_split_transport_status){
        .available = !IS_ENABLED(CONFIG_ZMK_BLE_CLEAR_BONDS_ON_START) && settings_loaded,
        .enabled = is_enabled,
        .connections = conn_status,
    };
}

static const struct zmk_split_transport_central_api central_api = {
    .send_command = split_central_bt_send_command,
    .get_available_source_ids = split_central_bt_get_available_source_ids,
    .set_enabled = split_central_bt_set_enabled,
    .set_status_callback = split_central_bt_set_status_callback,
    .get_status = split_central_bt_get_status,
};

ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER(bt_central, &central_api, CONFIG_ZMK_SPLIT_BLE_PRIORITY);
#if IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT)
static int dispatch_split_input(uint8_t source, struct zmk_split_transport_peripheral_event event) {
    return zmk_split_transport_central_peripheral_event_handler(&bt_central,source,event);
}
#endif

static void notify_transport_status(void) {
    if (transport_status_cb) {
        transport_status_cb(&bt_central, split_central_bt_get_status());
    }
}

static int finish_init() {
    settings_loaded = true;

    if (!transport_status_cb) {
        return 0;
    }

    return transport_status_cb(&bt_central, split_central_bt_get_status());
}

void peripheral_event_work_callback(struct k_work *work) {
    struct peripheral_event_wrapper ev;
    unsigned processed = 0;
    while (k_msgq_get(&peripheral_event_msgq, &ev, K_NO_WAIT) == 0) {
        LOG_DBG("Trigger key position state change of type %d", ev.event.type);
        zmk_split_transport_central_peripheral_event_handler(&bt_central, ev.source, ev.event);
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN)
        /* Continuous pointer input must not monopolize the system workqueue:
         * advertising and reconnect work use the same queue. Preserve FIFO. */
        if (++processed >= 16) {
            if (k_msgq_num_used_get(&peripheral_event_msgq)) k_work_submit(work);
            break;
        }
#else
        ARG_UNUSED(processed);
#endif
    }
}

#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN)
static void keypoint_split_diag_conn(struct bt_conn *conn,void *context) {
    struct bt_conn_info info;
    if(!peripheral_slot_for_conn(conn) || bt_conn_get_info(conn,&info) ||
       info.role!=BT_CONN_ROLE_CENTRAL)return;
    uint8_t *bytes=context;
    bytes[0]=info.state;
    bytes[1]=bt_conn_get_security(conn);
    sys_put_le16(info.le.interval,bytes+2);
    sys_put_le16(info.le.latency,bytes+4);
    sys_put_le16(info.le.timeout,bytes+6);
}
void zmk_split_central_link_diagnostics(uint8_t bytes[12]) {
    memset(bytes,0,12);
    bt_conn_foreach(BT_CONN_TYPE_LE,keypoint_split_diag_conn,bytes);
    sys_put_le16(atomic_get(&keypoint_split_inputs),bytes+8);
    sys_put_le16(atomic_get(&keypoint_split_input_drops),bytes+10);
}
#endif
