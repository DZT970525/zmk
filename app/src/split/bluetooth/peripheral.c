/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci_types.h>

#include "peripheral.h"
#include "service.h"

#if IS_ENABLED(CONFIG_SETTINGS)

#include <zephyr/settings/settings.h>

#endif

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/ble.h>
#include <zmk/split/role.h>
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_PEER_SELECTION)
#include <zmk/split/peer_selection.h>
static uint8_t peer_ad_marker[] = {0xff, 0xff, 0xd1, 1, ZMK_KEYPOINT_PEER_DONGLE, 0};
#endif
#include <zmk/split/bluetooth/uuid.h>

static const struct bt_data zmk_ble_ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_PEER_SELECTION)
    BT_DATA(BT_DATA_MANUFACTURER_DATA, peer_ad_marker, sizeof(peer_ad_marker)),
#endif
#if !IS_ENABLED(CONFIG_ZMK_KEYPOINT_PEER_SELECTION)
    BT_DATA_BYTES(BT_DATA_UUID16_SOME, 0x0f, 0x18 /* Battery Service */
                  ),
#endif
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, ZMK_SPLIT_BT_SERVICE_UUID)};

static bool is_connected = false;

static bool is_bonded = false;

#define SPLIT_REPEAT_FAST_ADV                                                                        \
    (IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE_RIGHT) || IS_ENABLED(CONFIG_BOARD_ZITAOTECH_KEYPOINT_RIGHT))

#if !IS_ENABLED(CONFIG_ZMK_KEYPOINT_PEER_SELECTION)
static void each_bond(const struct bt_bond_info *info, void *user_data) {
    bt_addr_le_t *addr = (bt_addr_le_t *)user_data;

    if (bt_addr_le_cmp(&info->addr, BT_ADDR_LE_NONE) != 0) {
        bt_addr_le_copy(addr, &info->addr);
    }
}
#else
struct peer_bond_lookup { const bt_addr_le_t *address; bool found; };
static void selected_peer_bond(const struct bt_bond_info *info, void *user_data) {
    struct peer_bond_lookup *lookup = user_data;
    if (bt_addr_le_eq(&info->addr, lookup->address)) lookup->found = true;
}
#endif

static int start_advertising(bool low_duty) {
    bt_addr_le_t central_addr = bt_addr_le_none;

#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_PEER_SELECTION)
    int ret = zmk_keypoint_peer_target_address(&central_addr);
    if (ret && ret != -ENOENT) return ret;
    if (!ret) {
        struct peer_bond_lookup lookup = {.address = &central_addr};
        bt_foreach_bond(BT_ID_DEFAULT, selected_peer_bond, &lookup);
        if (!lookup.found) central_addr = bt_addr_le_none;
    }
    peer_ad_marker[4] = zmk_keypoint_peer_target();
    peer_ad_marker[5] = zmk_keypoint_local_peer_kind();
#else
    bt_foreach_bond(BT_ID_DEFAULT, each_bond, &central_addr);
#endif

    if (bt_addr_le_cmp(&central_addr, BT_ADDR_LE_NONE) != 0) {
        is_bonded = true;
        struct bt_le_adv_param adv_param = low_duty ? *BT_LE_ADV_CONN_DIR_LOW_DUTY(&central_addr)
                                                    : *BT_LE_ADV_CONN_DIR(&central_addr);
        return bt_le_adv_start(&adv_param, NULL, 0, NULL, 0);
    } else {
        is_bonded = false;
        return bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, zmk_ble_ad, ARRAY_SIZE(zmk_ble_ad), NULL, 0);
    }
};

static bool low_duty_advertising = false;
static bool enabled = false;

static void advertising_cb(struct k_work *work) {
    if (SPLIT_REPEAT_FAST_ADV && (!enabled || is_connected)) {
        return;
    }

    const int err = start_advertising(low_duty_advertising);
    if (err < 0) {
        LOG_ERR("Failed to start advertising (%d)", err);
    }
}

K_WORK_DEFINE(advertising_work, advertising_cb);

int zmk_split_bt_peripheral_refresh_advertising(void) {
    if (!enabled || zmk_split_role_is_central()) { return -ENOTSUP; }
    if (is_connected) { return -EBUSY; }
    int ret=bt_le_adv_stop();
    if (ret && ret!=-EALREADY) { return ret; }
    low_duty_advertising=false;
    ret=k_work_submit(&advertising_work);
    return ret<0 ? ret : 0;
}

static void connected(struct bt_conn *conn, uint8_t err) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RUNTIME_ROLE)
    if (zmk_split_role_is_central()) {
        return;
    }
#endif
    is_connected = (err == 0);

    raise_zmk_split_peripheral_status_changed(
        (struct zmk_split_peripheral_status_changed){.connected = is_connected});

    if (err == BT_HCI_ERR_ADV_TIMEOUT && enabled) {
        /* On these right halves, recycled() starts another fast directed burst
         * once the controller's single connection slot is available again. */
        if (!SPLIT_REPEAT_FAST_ADV) {
            low_duty_advertising = true;
            k_work_submit(&advertising_work);
        }
    }
}

static void recycled(void) {
    if (enabled) {
        low_duty_advertising = false;
        k_work_submit(&advertising_work);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RUNTIME_ROLE)
    if (zmk_split_role_is_central()) {
        return;
    }
#endif
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_DBG("Disconnected from %s (reason 0x%02x)", addr, reason);

    is_connected = false;

    raise_zmk_split_peripheral_status_changed(
        (struct zmk_split_peripheral_status_changed){.connected = is_connected});
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (!err) {
        LOG_DBG("Security changed: %s level %u", addr, level);
    } else {
        LOG_ERR("Security failed: %s level %u err %d", addr, level, err);
    }
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
                             uint16_t timeout) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_DBG("%s: interval %d latency %d timeout %d", addr, interval, latency, timeout);
}

static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
    .recycled = recycled,
    .security_changed = security_changed,
    .le_param_updated = le_param_updated,
};

static void auth_pairing_complete(struct bt_conn *conn, bool bonded) { is_bonded = bonded; }

static struct bt_conn_auth_info_cb zmk_peripheral_ble_auth_info_cb = {
    .pairing_complete = auth_pairing_complete,
};

bool zmk_split_bt_peripheral_is_connected(void) { return is_connected; }

bool zmk_split_bt_peripheral_is_bonded(void) { return is_bonded; }

static zmk_split_transport_peripheral_status_changed_cb_t transport_status_cb;

static int
split_peripheral_bt_set_status_callback(zmk_split_transport_peripheral_status_changed_cb_t cb) {
    transport_status_cb = cb;
    return 0;
}

static void find_first_conn(struct bt_conn *conn, void *data) {
    struct bt_conn **cp = (struct bt_conn **)data;

    *cp = conn;
}

static int split_peripheral_bt_set_enabled(bool en) {
    int err;

    enabled = en;
    if (en) {
        k_work_submit(&advertising_work);
        return 0;
    } else {
        struct bt_conn *conn = NULL;
        bt_conn_foreach(BT_CONN_TYPE_LE, find_first_conn, &conn);
        if (conn) {
            err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            if (err < 0) {
                LOG_WRN("Failed to disconnect connection to central (%d)", err);
            }
        }

        err = bt_le_adv_stop();

        if (err < 0) {
            LOG_WRN("Failed to stop advertising (%d)", err);
        }

        return 0;
    }
}

static void notify_transport_status(void);

static void notify_status_work_cb(struct k_work *_work) { notify_transport_status(); }

static K_WORK_DEFINE(notify_status_work, notify_status_work_cb);

static bool settings_loaded = false;

static struct zmk_split_transport_status split_peripheral_bt_get_status(void) {
    return (struct zmk_split_transport_status){
        .available = !IS_ENABLED(CONFIG_ZMK_BLE_CLEAR_BONDS_ON_START) && settings_loaded,
        .enabled = enabled,
        .connections = zmk_split_bt_peripheral_is_connected()
                           ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED
                           : ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED,
    };
}

static const struct zmk_split_transport_peripheral_api peripheral_api = {
    .report_event = zmk_split_transport_peripheral_bt_report_event,
    .set_enabled = split_peripheral_bt_set_enabled,
    .set_status_callback = split_peripheral_bt_set_status_callback,
    .get_status = split_peripheral_bt_get_status,
};

ZMK_SPLIT_TRANSPORT_PERIPHERAL_REGISTER(bt_peripheral, &peripheral_api,
                                        CONFIG_ZMK_SPLIT_BLE_PRIORITY);

struct zmk_split_transport_peripheral *zmk_split_transport_peripheral_bt(void) {
    return &bt_peripheral;
}

static void notify_transport_status(void) {
    if (transport_status_cb) {
        transport_status_cb(&bt_peripheral, split_peripheral_bt_get_status());
    }
}

static int zmk_peripheral_ble_complete_startup(void) {
#if IS_ENABLED(CONFIG_ZMK_BLE_CLEAR_BONDS_ON_START)
    LOG_WRN("Clearing all existing BLE bond information from the keyboard");

    bt_unpair(BT_ID_DEFAULT, NULL);
#else
    bt_conn_cb_register(&conn_callbacks);
    bt_conn_auth_info_cb_register(&zmk_peripheral_ble_auth_info_cb);

    low_duty_advertising = false;

    settings_loaded = true;
    k_work_submit(&notify_status_work);
#endif

    return 0;
}

#if IS_ENABLED(CONFIG_SETTINGS)

static int peripheral_ble_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                     void *cb_arg) {
    return 0;
}

static struct settings_handler ble_peripheral_settings_handler = {
    .name = "ble_peripheral",
    .h_set = peripheral_ble_handle_set,
    .h_commit = zmk_peripheral_ble_complete_startup};

#endif // IS_ENABLED(CONFIG_SETTINGS)

static int zmk_peripheral_ble_init(void) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_DUAL_ROLE_BUILD)
    int err = bt_enable(NULL);

    if (err) {
        LOG_ERR("BLUETOOTH FAILED (%d)", err);
        return err;
    }
#endif

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_register(&ble_peripheral_settings_handler);
#else
    zmk_peripheral_ble_complete_startup();
#endif

    return 0;
}

SYS_INIT(zmk_peripheral_ble_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);
