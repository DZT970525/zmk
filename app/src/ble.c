/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/init.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci_types.h>

#if IS_ENABLED(CONFIG_SETTINGS)

#include <zephyr/settings/settings.h>

#endif

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/ble.h>
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
#include <zmk/keypoint_relay.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_SERVER) || IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_CLIENT)
#include <zmk/split/dongle_presence.h>
#endif
#include <zmk/keys.h>
#include <zmk/split/bluetooth/uuid.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

#if IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY)
#include <zmk/events/keycode_state_changed.h>

#define PASSKEY_DIGITS 6

static struct bt_conn *auth_passkey_entry_conn;
RING_BUF_DECLARE(passkey_entries, PASSKEY_DIGITS);

#endif /* IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY) */

enum advertising_type {
    ZMK_ADV_NONE,
    ZMK_ADV_DIR,
    ZMK_ADV_CONN,
} advertising_status;

#define CURR_ADV(adv) (adv << 4)

#define ZMK_ADV_CONN_NAME                                                                          \
    BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_NAME | BT_LE_ADV_OPT_FORCE_NAME_IN_AD,  \
                    BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL)

static struct zmk_ble_profile profiles[ZMK_BLE_PROFILE_COUNT];
static uint8_t active_profile;
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER)
static bool host_enabled = true;
#endif
bool zmk_ble_host_is_enabled(void) {
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER)
    return host_enabled;
#else
    return true;
#endif
}

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

BUILD_ASSERT(
    DEVICE_NAME_LEN <= CONFIG_BT_DEVICE_NAME_MAX,
    "ERROR: BLE device name is too long. Max length: " STRINGIFY(CONFIG_BT_DEVICE_NAME_MAX));

static struct bt_data zmk_ble_ad[] = {
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, BT_BYTES_LIST_LE16(CONFIG_BT_DEVICE_APPEARANCE)),
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_SOME, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL), /* HID Service */
                  BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)                        /* Battery Service */
                  ),
};

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static bt_addr_le_t peripheral_addrs[ZMK_SPLIT_BLE_PERIPHERAL_COUNT];

#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) */

static void raise_profile_changed_event(void) {
    raise_zmk_ble_active_profile_changed((struct zmk_ble_active_profile_changed){
        .index = active_profile, .profile = &profiles[active_profile]});
}

static void raise_profile_changed_event_callback(struct k_work *work) {
    raise_profile_changed_event();
}

K_WORK_DEFINE(raise_profile_changed_event_work, raise_profile_changed_event_callback);

bool zmk_ble_active_profile_is_open(void) { return zmk_ble_profile_is_open(active_profile); }

bool zmk_ble_profile_is_open(uint8_t index) {
    if (index >= ZMK_BLE_PROFILE_COUNT) {
        return false;
    }
    return !bt_addr_le_cmp(&profiles[index].peer, BT_ADDR_LE_ANY);
}

void set_profile_address(uint8_t index, const bt_addr_le_t *addr) {
    char setting_name[17];
    char addr_str[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

    memcpy(&profiles[index].peer, addr, sizeof(bt_addr_le_t));
    sprintf(setting_name, "ble/profiles/%d", index);
    LOG_DBG("Setting profile addr for %s to %s", setting_name, addr_str);
#if IS_ENABLED(CONFIG_SETTINGS)
    settings_save_one(setting_name, &profiles[index], sizeof(struct zmk_ble_profile));
#endif
    k_work_submit(&raise_profile_changed_event_work);
}

bool zmk_ble_active_profile_is_connected(void) {
    return zmk_ble_profile_is_connected(active_profile);
}

bool zmk_ble_profile_is_connected(uint8_t index) {
    if (index >= ZMK_BLE_PROFILE_COUNT) {
        return false;
    }
    struct bt_conn *conn;
    struct bt_conn_info info;
    bt_addr_le_t *addr = &profiles[index].peer;
    if (!bt_addr_le_cmp(addr, BT_ADDR_LE_ANY)) {
        return false;
    } else if ((conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr)) == NULL) {
        return false;
    }

    bt_conn_get_info(conn, &info);

    bt_conn_unref(conn);

    return info.state == BT_CONN_STATE_CONNECTED;
}

#define CHECKED_ADV_STOP()                                                                         \
    err = bt_le_adv_stop();                                                                        \
    advertising_status = ZMK_ADV_NONE;                                                             \
    if (err) {                                                                                     \
        LOG_ERR("Failed to stop advertising (err %d)", err);                                       \
        return err;                                                                                \
    }

#define CHECKED_DIR_ADV()                                                                          \
    addr = zmk_ble_active_profile_addr();                                                          \
    conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr);                                            \
    if (conn != NULL) { /* TODO: Check status of connection */                                     \
        LOG_DBG("Skipping advertising, profile host is already connected");                        \
        bt_conn_unref(conn);                                                                       \
        return 0;                                                                                  \
    }                                                                                              \
    err = bt_le_adv_start(BT_LE_ADV_CONN_DIR_LOW_DUTY(addr), zmk_ble_ad, ARRAY_SIZE(zmk_ble_ad),   \
                          NULL, 0);                                                                \
    if (err) {                                                                                     \
        LOG_ERR("Advertising failed to start (err %d)", err);                                      \
        return err;                                                                                \
    }                                                                                              \
    advertising_status = ZMK_ADV_DIR;

#define CHECKED_OPEN_ADV()                                                                         \
    err = bt_le_adv_start(ZMK_ADV_CONN_NAME, zmk_ble_ad, ARRAY_SIZE(zmk_ble_ad), NULL, 0);         \
    if (err) {                                                                                     \
        LOG_ERR("Advertising failed to start (err %d)", err);                                      \
        return err;                                                                                \
    }                                                                                              \
    advertising_status = ZMK_ADV_CONN;

#include <zmk/split/role.h>
int update_advertising(void) {
    if (!zmk_split_role_is_central()) {
        return 0;
    }
    int err = 0;
    bt_addr_le_t *addr;
    struct bt_conn *conn;
    enum advertising_type desired_adv = ZMK_ADV_NONE;

    if (!zmk_ble_host_is_enabled()) {
        desired_adv = ZMK_ADV_NONE;
    } else if (zmk_ble_active_profile_is_open()) {
        desired_adv = ZMK_ADV_CONN;
    } else if (!zmk_ble_active_profile_is_connected()) {
        desired_adv = ZMK_ADV_CONN;
        // Need to fix directed advertising for privacy centrals. See
        // https://github.com/zephyrproject-rtos/zephyr/pull/14984 char
        // addr_str[BT_ADDR_LE_STR_LEN]; bt_addr_le_to_str(zmk_ble_active_profile_addr(), addr_str,
        // sizeof(addr_str));

        // LOG_DBG("Directed advertising to %s", addr_str);
        // desired_adv = ZMK_ADV_DIR;
    }
    LOG_DBG("advertising from %d to %d", advertising_status, desired_adv);

    switch (desired_adv + CURR_ADV(advertising_status)) {
    case ZMK_ADV_NONE + CURR_ADV(ZMK_ADV_DIR):
    case ZMK_ADV_NONE + CURR_ADV(ZMK_ADV_CONN):
        CHECKED_ADV_STOP();
        break;
    case ZMK_ADV_DIR + CURR_ADV(ZMK_ADV_DIR):
    case ZMK_ADV_DIR + CURR_ADV(ZMK_ADV_CONN):
        CHECKED_ADV_STOP();
        CHECKED_DIR_ADV();
        break;
    case ZMK_ADV_DIR + CURR_ADV(ZMK_ADV_NONE):
        CHECKED_DIR_ADV();
        break;
    case ZMK_ADV_CONN + CURR_ADV(ZMK_ADV_DIR):
        CHECKED_ADV_STOP();
        CHECKED_OPEN_ADV();
        break;
    case ZMK_ADV_CONN + CURR_ADV(ZMK_ADV_NONE):
        CHECKED_OPEN_ADV();
        break;
    }

    return 0;
};

static void update_advertising_callback(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(update_advertising_work, update_advertising_callback);
static void update_advertising_callback(struct k_work *work) {
    ARG_UNUSED(work);
    int ret = update_advertising();
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) || ((IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((IS_ENABLED(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
    /* A disconnected connection may not have recycled its controller slot yet. */
    if (ret == -ENOMEM || ret == -EAGAIN || ret == -EBUSY)
        k_work_reschedule(&update_advertising_work, K_MSEC(100));
#endif
}
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) || ((IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((IS_ENABLED(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
static atomic_t host_tune_reset = ATOMIC_INIT(1);
static int64_t host_param_requested_at = -5000;
static atomic_t host_disconnect_reason, host_param_requests;
uint8_t zmk_ble_host_param_request_count(void) { return atomic_get(&host_param_requests); }
uint8_t zmk_ble_last_host_disconnect_reason(void) { return atomic_get(&host_disconnect_reason); }
static void keypoint_host_tune(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(keypoint_host_tune_work, keypoint_host_tune);
/* Idle computer BLE is still scheduled by the controller even when all input
 * goes through the receiver. Keep the bond/connection, but reduce its airtime. */
static bool host_link_idle(void) {
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
    return zmk_keypoint_relay_routed();
#else
    return false;
#endif
}
static bool host_link_matches(uint16_t interval,uint16_t latency,uint16_t timeout) {
    if(host_link_idle())return interval>=24 && interval<=40 && latency==4 && timeout>=400;
    return interval>=CONFIG_BT_PERIPHERAL_PREF_MIN_INT && interval<=CONFIG_BT_PERIPHERAL_PREF_MAX_INT && !latency && timeout>=CONFIG_BT_PERIPHERAL_PREF_TIMEOUT;
}
static void keypoint_host_tune(struct k_work *work) {
    ARG_UNUSED(work);
    static int requested_mode=-1;
#if !IS_ENABLED(CONFIG_ZITAOTECH_BBP9981_SOFLE_BLE) && ((defined(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)))
    static bool security_requested;
#endif
    struct bt_conn *conn=zmk_ble_active_profile_conn();
    if(!conn)return;
    int mode=host_link_idle()?1:0;
    bool reset=atomic_cas(&host_tune_reset,1,0);
    if(reset || requested_mode!=mode){
        requested_mode=mode;host_param_requested_at=-5000;atomic_clear(&host_param_requests);
#if !IS_ENABLED(CONFIG_ZITAOTECH_BBP9981_SOFLE_BLE) && ((defined(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)))
        security_requested=false;
#endif
    }
    struct bt_conn_info info;
    if(bt_conn_get_security(conn)<BT_SECURITY_L2){
#if !IS_ENABLED(CONFIG_ZITAOTECH_BBP9981_SOFLE_BLE) && ((defined(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)))
        /* Give bonded centrals time to restore encryption themselves. Repeated
         * Security Requests during their reconnect sequence can cause a loop.
         * security_changed resumes parameter tuning after encryption succeeds. */
        if (!security_requested) {
            security_requested=true;
            int ret=bt_conn_set_security(conn,BT_SECURITY_L2);
            if (ret && ret!=-EALREADY && ret!=-EBUSY)
                LOG_WRN("Host reconnect security request failed (%d)",ret);
        }
#else
        int ret=bt_conn_set_security(conn,BT_SECURITY_L2);
        if(!ret || ret==-EALREADY || ret==-EBUSY)k_work_reschedule(&keypoint_host_tune_work,K_MSEC(500));
#endif
        bt_conn_unref(conn);return;
    }
#if IS_ENABLED(CONFIG_ZMK_SOFLE_TEST_DISABLE_HOST_PARAM_TUNE)
    /* Isolate the effect of host parameter requests without changing SMP. */
    bt_conn_unref(conn);
    return;
#endif
    if(!bt_conn_get_info(conn,&info) && info.state==BT_CONN_STATE_CONNECTED &&
       info.role==BT_CONN_ROLE_PERIPHERAL && !host_link_matches(info.le.interval,info.le.latency,info.le.timeout)) {
        if(atomic_get(&host_param_requests)>=3){bt_conn_unref(conn);return;}
        int64_t now=k_uptime_get();
        if(now-host_param_requested_at<5000){
            k_work_reschedule(&keypoint_host_tune_work,K_MSEC(5000-(now-host_param_requested_at)));
            bt_conn_unref(conn);return;
        }
        const struct bt_le_conn_param *params=mode?BT_LE_CONN_PARAM(24,40,4,400):
            BT_LE_CONN_PARAM(CONFIG_BT_PERIPHERAL_PREF_MIN_INT,CONFIG_BT_PERIPHERAL_PREF_MAX_INT,0,CONFIG_BT_PERIPHERAL_PREF_TIMEOUT);
        bt_conn_le_param_update(conn,params);
        /* Count even rejected/busy attempts; never run an endless 500ms loop.
         * Verify the actual negotiated values, including hosts declining changes. */
        host_param_requested_at=now;atomic_inc(&host_param_requests);
        k_work_reschedule(&keypoint_host_tune_work,K_SECONDS(5));
    }
    bt_conn_unref(conn);
}
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
static int relay_host_route_changed(const zmk_event_t *event) {
    ARG_UNUSED(event);
    /* Repeated relay/status events do not reset the per-route retry budget. */
    k_work_reschedule(&keypoint_host_tune_work,K_MSEC(100));
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(relay_host_route,relay_host_route_changed);
ZMK_SUBSCRIPTION(relay_host_route,zmk_keypoint_relay_changed);
#endif
static void keypoint_host_recycled(void) {
    if (zmk_split_role_is_central())
        k_work_reschedule(&update_advertising_work, K_NO_WAIT);
}
#endif
int zmk_ble_host_set_enabled(bool enabled) {
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER)
    if (host_enabled == enabled) return 0;
    uint8_t value = enabled;
    int ret = settings_save_one("ble/host_enabled", &value, sizeof(value));
    if (ret) return ret;
    host_enabled = enabled;
    /* Only host-profile bonds use identity 0; relay identity 1 is untouched. */
    if (!enabled) for (unsigned i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) zmk_ble_prof_disconnect(i);
    k_work_reschedule(&update_advertising_work, K_NO_WAIT);
    k_work_submit(&raise_profile_changed_event_work);
    return 0;
#else
    return -ENOTSUP;
#endif
}

static void clear_profile_bond(uint8_t profile) {
    if (bt_addr_le_cmp(&profiles[profile].peer, BT_ADDR_LE_ANY)) {
        bt_unpair(BT_ID_DEFAULT, &profiles[profile].peer);
        set_profile_address(profile, BT_ADDR_LE_ANY);
    }
}

void zmk_ble_clear_bonds(void) {
    LOG_DBG("zmk_ble_clear_bonds()");

    clear_profile_bond(active_profile);
    update_advertising();
};

void zmk_ble_clear_all_bonds(void) {
    LOG_DBG("zmk_ble_clear_all_bonds()");

    // Unpair all profiles
    for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        clear_profile_bond(i);
    }

    // Automatically switch to profile 0
    zmk_ble_prof_select(0);
    update_advertising();
};

int zmk_ble_active_profile_index(void) { return active_profile; }

int zmk_ble_profile_index(const bt_addr_le_t *addr) {
    for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        if (bt_addr_le_cmp(addr, &profiles[i].peer) == 0) {
            return i;
        }
    }
    return -ENODEV;
}

bt_addr_le_t *zmk_ble_profile_address(uint8_t index) {
    if (index >= ZMK_BLE_PROFILE_COUNT) {
        return (bt_addr_le_t *)(BT_ADDR_LE_NONE);
    }
    return &profiles[index].peer;
}

#if IS_ENABLED(CONFIG_SETTINGS)
static void ble_save_profile_work(struct k_work *work) {
    settings_save_one("ble/active_profile", &active_profile, sizeof(active_profile));
}

static struct k_work_delayable ble_save_work;
#endif

static int ble_save_profile(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    return k_work_reschedule(&ble_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
#else
    return 0;
#endif
}

int zmk_ble_prof_select(uint8_t index) {
    if (index >= ZMK_BLE_PROFILE_COUNT) {
        return -ERANGE;
    }

    LOG_DBG("profile %d", index);
    if (active_profile == index) {
        return 0;
    }

    active_profile = index;
    ble_save_profile();
    /* Highlight the selected slot before doing Bluetooth controller work. */
    raise_profile_changed_event();
    k_work_reschedule(&update_advertising_work, K_NO_WAIT);
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) || ((IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((IS_ENABLED(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
    atomic_set(&host_tune_reset, 1);
    k_work_reschedule(&keypoint_host_tune_work, K_MSEC(100));
#endif

    return 0;
};

int zmk_ble_prof_next(void) {
    LOG_DBG("");
    return zmk_ble_prof_select((active_profile + 1) % ZMK_BLE_PROFILE_COUNT);
};

int zmk_ble_prof_prev(void) {
    LOG_DBG("");
    return zmk_ble_prof_select((active_profile + ZMK_BLE_PROFILE_COUNT - 1) %
                               ZMK_BLE_PROFILE_COUNT);
};

int zmk_ble_prof_disconnect(uint8_t index) {
    if (index >= ZMK_BLE_PROFILE_COUNT)
        return -ERANGE;

    bt_addr_le_t *addr = &profiles[index].peer;
    struct bt_conn *conn;
    int result;

    if (!bt_addr_le_cmp(addr, BT_ADDR_LE_ANY)) {
        return -ENODEV;
    } else if ((conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr)) == NULL) {
        return -ENODEV;
    }

    result = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    LOG_DBG("Disconnected from profile %d: %d", index, result);

    bt_conn_unref(conn);
    return result;
}

bt_addr_le_t *zmk_ble_active_profile_addr(void) { return &profiles[active_profile].peer; }

struct bt_conn *zmk_ble_active_profile_conn(void) {
    struct bt_conn *conn;
    bt_addr_le_t *addr = zmk_ble_active_profile_addr();

    if (!bt_addr_le_cmp(addr, BT_ADDR_LE_ANY)) {
        LOG_WRN("Not sending, no active address for current profile");
        return NULL;
    } else if ((conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr)) == NULL) {
        LOG_WRN("Not sending, not connected to active profile");
        return NULL;
    }

    return conn;
}

char *zmk_ble_active_profile_name(void) { return profiles[active_profile].name; }

int zmk_ble_set_device_name(char *name) {
    // Copy new name to advertising parameters
    int err = bt_set_name(name);
    LOG_DBG("New device name: %s", name);
    if (err) {
        LOG_ERR("Failed to set new device name (err %d)", err);
        return err;
    }
    if (advertising_status == ZMK_ADV_CONN) {
        // Stop current advertising so it can restart with new name
        err = bt_le_adv_stop();
        advertising_status = ZMK_ADV_NONE;
        if (err) {
            LOG_ERR("Failed to stop advertising (err %d)", err);
            return err;
        }
    }
    return update_advertising();
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

int zmk_ble_put_peripheral_addr(const bt_addr_le_t *addr) {
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        // If the address is recognized and already stored in settings, return
        // index and no additional action is necessary.
        if (bt_addr_le_cmp(&peripheral_addrs[i], addr) == 0) {
            LOG_DBG("Found existing peripheral address in slot %d", i);
            return i;
        } else {
            char addr_str[BT_ADDR_LE_STR_LEN];
            bt_addr_le_to_str(&peripheral_addrs[i], addr_str, sizeof(addr_str));
            LOG_DBG("peripheral slot %d occupied by %s", i, addr_str);
        }

        // If the peripheral address slot is open, store new peripheral in the
        // slot and return index. This compares against BT_ADDR_LE_ANY as that
        // is the zero value.
        if (bt_addr_le_cmp(&peripheral_addrs[i], BT_ADDR_LE_ANY) == 0) {
            char addr_str[BT_ADDR_LE_STR_LEN];
            bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
            LOG_DBG("Storing peripheral %s in slot %d", addr_str, i);
            bt_addr_le_copy(&peripheral_addrs[i], addr);

#if IS_ENABLED(CONFIG_SETTINGS)
            char setting_name[32];
            sprintf(setting_name, "ble/peripheral_addresses/%d", i);
            settings_save_one(setting_name, addr, sizeof(bt_addr_le_t));
#endif // IS_ENABLED(CONFIG_SETTINGS)
            return i;
        }
    }

    // The peripheral does not match a known peripheral and there is no
    // available slot.
    return -ENOMEM;
}

#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) */

#if IS_ENABLED(CONFIG_SETTINGS)

static int ble_profiles_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                   void *cb_arg) {
    const char *next;

    LOG_DBG("Setting BLE value %s", name);
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER)
    if (!strcmp(name, "host_enabled")) {
        uint8_t value;
        if (len != 1 || read_cb(cb_arg, &value, 1) != 1 || value > 1) return -EINVAL;
        host_enabled = value;
        return 0;
    }
#endif

    if (settings_name_steq(name, "profiles", &next) && next) {
        char *endptr;
        uint8_t idx = strtoul(next, &endptr, 10);
        if (*endptr != '\0') {
            LOG_WRN("Invalid profile index: %s", next);
            return -EINVAL;
        }

        if (len != sizeof(struct zmk_ble_profile)) {
            LOG_ERR("Invalid profile size (got %d expected %d)", len,
                    sizeof(struct zmk_ble_profile));
            return -EINVAL;
        }

        if (idx >= ZMK_BLE_PROFILE_COUNT) {
            LOG_WRN("Profile address for index %d is larger than max of %d", idx,
                    ZMK_BLE_PROFILE_COUNT);
            return -EINVAL;
        }

        int err = read_cb(cb_arg, &profiles[idx], sizeof(struct zmk_ble_profile));
        if (err <= 0) {
            LOG_ERR("Failed to handle profile address from settings (err %d)", err);
            return err;
        }

        char addr_str[BT_ADDR_LE_STR_LEN];
        bt_addr_le_to_str(&profiles[idx].peer, addr_str, sizeof(addr_str));

        LOG_DBG("Loaded %s address for profile %d", addr_str, idx);
    } else if (settings_name_steq(name, "active_profile", &next) && !next) {
        if (len != sizeof(active_profile)) {
            return -EINVAL;
        }

        int err = read_cb(cb_arg, &active_profile, sizeof(active_profile));
        if (err <= 0) {
            LOG_ERR("Failed to handle active profile from settings (err %d)", err);
            return err;
        }
        /* Older builds exposed surplus bond slots as host profiles. */
        if (active_profile >= ZMK_BLE_PROFILE_COUNT) {
            active_profile = 0;
        }
    }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    else if (settings_name_steq(name, "peripheral_addresses", &next) && next) {
        if (len != sizeof(bt_addr_le_t)) {
            return -EINVAL;
        }

        int i = atoi(next);
        if (i < 0 || i >= ZMK_SPLIT_BLE_PERIPHERAL_COUNT) {
            LOG_ERR("Failed to store peripheral address in memory");
        } else {
            int err = read_cb(cb_arg, &peripheral_addrs[i], sizeof(bt_addr_le_t));
            if (err <= 0) {
                LOG_ERR("Failed to handle peripheral address from settings (err %d)", err);
                return err;
            }
        }
    }
#endif

    return 0;
};

static int zmk_ble_complete_startup(void);

static struct settings_handler profiles_handler = {
    .name = "ble", .h_set = ble_profiles_handle_set, .h_commit = zmk_ble_complete_startup};

#endif /* IS_ENABLED(CONFIG_SETTINGS) */

static bool is_conn_active_profile(const struct bt_conn *conn) {
    return bt_addr_le_cmp(bt_conn_get_dst(conn), &profiles[active_profile].peer) == 0;
}

#if (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) || IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) || IS_ENABLED(CONFIG_BOARD_BBP9981)) && IS_ENABLED(CONFIG_ZMK_USB_LOGGING)
static void q20_log_bond(const struct bt_bond_info *bond, void *data) {
    ARG_UNUSED(data);
    char address[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(&bond->addr,address,sizeof(address));
    LOG_INF("Host restored bond: %s, selected profile match=%d", address,
            bt_addr_le_eq(&bond->addr,zmk_ble_active_profile_addr()));
}
#endif

#if (defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))
struct host_bond_lookup { const bt_addr_le_t *peer; bool found; };
static void host_find_bond(const struct bt_bond_info *bond, void *data) {
    struct host_bond_lookup *lookup=data;
    if (bt_addr_le_eq(&bond->addr,lookup->peer)) lookup->found=true;
}
static void host_start_first_pairing(struct bt_conn *conn) {
    struct host_bond_lookup lookup={.peer=bt_conn_get_dst(conn)};
    bt_foreach_bond(BT_ID_DEFAULT,host_find_bond,&lookup);
    /* The reconnect grace period is only for a bond we actually restored.
     * A new host needs an immediate security request to start first pairing. */
    if (!lookup.found && bt_conn_get_security(conn)<BT_SECURITY_L2) {
        int ret=bt_conn_set_security(conn,BT_SECURITY_L2);
        if (ret && ret!=-EALREADY && ret!=-EBUSY)
            LOG_WRN("First pairing security request failed (%d)",ret);
    }
}
#endif

static void connected(struct bt_conn *conn, uint8_t err) {
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
    if (zmk_keypoint_relay_is_connection(conn)) return;
#endif
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_SERVER)
    if (zmk_keypoint_presence_is_control_connection(conn)) { return; }
#endif
    if (!zmk_split_role_is_central()) {
        return;
    }
    char addr[BT_ADDR_LE_STR_LEN];
    struct bt_conn_info info;
    LOG_DBG("Connected thread: %p", k_current_get());

    bt_conn_get_info(conn, &info);

    if (info.role != BT_CONN_ROLE_PERIPHERAL) {
        LOG_DBG("SKIPPING FOR ROLE %d", info.role);
        return;
    }

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    advertising_status = ZMK_ADV_NONE;

    if (err) {
        LOG_WRN("Failed to connect to %s (%u)", addr, err);
        k_work_reschedule(&update_advertising_work, K_NO_WAIT);
        return;
    }

    LOG_DBG("Connected %s", addr);
#if !IS_ENABLED(CONFIG_ZITAOTECH_BBP9981_SOFLE_BLE) && ((defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)))
    host_start_first_pairing(conn);
#endif
#if (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) || IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) || IS_ENABLED(CONFIG_BOARD_BBP9981)) && IS_ENABLED(CONFIG_ZMK_USB_LOGGING)
    LOG_INF("Host reconnect: profile=%u match=%d security=%u", active_profile,
            is_conn_active_profile(conn),bt_conn_get_security(conn));
    bt_foreach_bond(BT_ID_DEFAULT,q20_log_bond,NULL);
#endif

    k_work_reschedule(&update_advertising_work, K_NO_WAIT);

    if (is_conn_active_profile(conn)) {
        LOG_DBG("Active profile connected");
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) || ((IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((IS_ENABLED(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
        atomic_set(&host_tune_reset, 1);
#if !IS_ENABLED(CONFIG_ZITAOTECH_BBP9981_SOFLE_BLE) && ((defined(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)))
        k_work_reschedule(&keypoint_host_tune_work, K_SECONDS(2));
#else
        k_work_reschedule(&keypoint_host_tune_work, K_MSEC(100));
#endif
#endif
        k_work_submit(&raise_profile_changed_event_work);
    }
}

#if defined(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || defined(CONFIG_BOARD_SOFLE_DONGLE) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) || ((defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
static void host_pair_cancel(struct bt_conn *conn);
#endif
static void disconnected(struct bt_conn *conn, uint8_t reason) {
#if defined(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || defined(CONFIG_BOARD_SOFLE_DONGLE) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) || ((defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
    host_pair_cancel(conn);
#endif
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
    if (zmk_keypoint_relay_is_connection(conn)) return;
#endif
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_SERVER)
    if (zmk_keypoint_presence_is_control_connection(conn)) { return; }
#endif
    if (!zmk_split_role_is_central()) {
        return;
    }
    char addr[BT_ADDR_LE_STR_LEN];
    struct bt_conn_info info;

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_DBG("Disconnected from %s (reason 0x%02x)", addr, reason);

    bt_conn_get_info(conn, &info);

    if (info.role != BT_CONN_ROLE_PERIPHERAL) {
        LOG_DBG("SKIPPING FOR ROLE %d", info.role);
        return;
    }

    // We need to do this in a work callback, otherwise the advertising update will still see the
    // connection for a profile as active, and not start advertising yet.
    k_work_reschedule(&update_advertising_work, K_NO_WAIT);

    if (is_conn_active_profile(conn)) {
        LOG_DBG("Active profile disconnected" );
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) || ((IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((IS_ENABLED(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
        atomic_set(&host_disconnect_reason, reason);
#endif
        k_work_submit(&raise_profile_changed_event_work);
    }
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (!err) {
        LOG_DBG("Security changed: %s level %u", addr, level);
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) || ((IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((IS_ENABLED(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
        if (is_conn_active_profile(conn) && level >= BT_SECURITY_L2) {
            k_work_reschedule(&keypoint_host_tune_work, K_MSEC(200));
            k_work_submit(&raise_profile_changed_event_work);
        }
#endif
    } else {
        LOG_ERR("Security failed: %s level %u err %d", addr, level, err);
    }
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
                             uint16_t timeout) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_DBG("%s: interval %d latency %d timeout %d", addr, interval, latency, timeout);
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) || ((IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((IS_ENABLED(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
    if (is_conn_active_profile(conn) && !host_link_matches(interval,latency,timeout))
        k_work_reschedule(&keypoint_host_tune_work, K_MSEC(200));
#endif
}

static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = security_changed,
    .le_param_updated = le_param_updated,
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) || ((IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((IS_ENABLED(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
    .recycled = keypoint_host_recycled,
#endif
};

/*
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_DBG("Passkey for %s: %06u", addr, passkey);
}
*/

#if IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY)

static void auth_passkey_entry(struct bt_conn *conn) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_DBG("Passkey entry requested for %s", addr);
    ring_buf_reset(&passkey_entries);
    auth_passkey_entry_conn = bt_conn_ref(conn);
}

#endif

#if defined(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || defined(CONFIG_BOARD_SOFLE_DONGLE) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) || ((defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
static void host_pair_cancel(struct bt_conn *conn);
#endif
static void auth_cancel(struct bt_conn *conn) {
#if defined(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || defined(CONFIG_BOARD_SOFLE_DONGLE) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) || ((defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
    host_pair_cancel(conn);
#endif
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

#if IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY)
    if (auth_passkey_entry_conn) {
        bt_conn_unref(auth_passkey_entry_conn);
        auth_passkey_entry_conn = NULL;
    }

    ring_buf_reset(&passkey_entries);
#endif

    LOG_DBG("Pairing cancelled: %s", addr);
}

#if defined(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || defined(CONFIG_BOARD_SOFLE_DONGLE) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) || ((defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
/* Experimental host replacement: keep the old slot until SMP succeeds. */
static struct k_spinlock host_pair_lock;
static struct bt_conn *host_pair_conn;
static uint8_t host_pair_slot;
static bt_addr_le_t host_pair_old;
static bool host_pair_completed, host_pair_committing;
static bool host_address_elsewhere(const bt_addr_le_t *address,uint8_t slot) {
    for(int i=0;i<ZMK_BLE_PROFILE_COUNT;i++)
        if(i!=slot && bt_addr_le_eq(address,&profiles[i].peer))return true;
    return false;
}
static bool host_pair_capture(struct bt_conn *conn) {
    k_spinlock_key_t key=k_spin_lock(&host_pair_lock);
    bool allowed=!host_pair_committing && (!host_pair_conn || host_pair_conn==conn);
    if(allowed && !host_pair_conn) {
        host_pair_conn=bt_conn_ref(conn);host_pair_slot=active_profile;
        bt_addr_le_copy(&host_pair_old,&profiles[active_profile].peer);host_pair_completed=false;
    } else if(allowed && host_pair_slot!=active_profile)allowed=false;
    k_spin_unlock(&host_pair_lock,key);return allowed;
}
static void host_pair_cancel(struct bt_conn *conn) {
    k_spinlock_key_t key=k_spin_lock(&host_pair_lock);
    struct bt_conn *old=host_pair_conn==conn ? host_pair_conn : NULL;
    if(old){host_pair_conn=NULL;host_pair_completed=false;}
    k_spin_unlock(&host_pair_lock,key);
    if(old)bt_conn_unref(old);
}
static int host_pair_store_profile(uint8_t slot,const bt_addr_le_t *peer) {
    struct zmk_ble_profile replacement=profiles[slot];
    bt_addr_le_copy(&replacement.peer,peer);
#if IS_ENABLED(CONFIG_SETTINGS)
    char setting_name[17];snprintf(setting_name,sizeof(setting_name),"ble/profiles/%u",slot);
    int rc=settings_save_one(setting_name,&replacement,sizeof(replacement));
    if(rc)return rc;
#endif
    profiles[slot]=replacement;k_work_submit(&raise_profile_changed_event_work);return 0;
}
static void host_pair_commit(struct k_work *work) {
    ARG_UNUSED(work);
    k_spinlock_key_t key=k_spin_lock(&host_pair_lock);
    struct bt_conn *conn=host_pair_completed ? host_pair_conn : NULL;
    uint8_t slot=host_pair_slot;bt_addr_le_t old=host_pair_old;
    if(conn){host_pair_conn=NULL;host_pair_completed=false;host_pair_committing=true;}
    k_spin_unlock(&host_pair_lock,key);
    if(!conn)return;
    const bt_addr_le_t *peer=bt_conn_get_dst(conn);
    struct bt_conn_info info;
    bool valid=!bt_conn_get_info(conn,&info) && info.state==BT_CONN_STATE_CONNECTED &&
        bt_conn_get_security(conn)>=BT_SECURITY_L2 && active_profile==slot &&
        bt_addr_le_eq(&profiles[slot].peer,&old) && !host_address_elsewhere(peer,slot) &&
        (bt_addr_le_eq(&old,peer) || !zmk_ble_profile_is_connected(slot));
    if(valid && host_pair_store_profile(slot,peer)==0) {
        /* Delete only the replaced host bond, never other slots or receiver identity 1. */
        if(!bt_addr_le_eq(&old,BT_ADDR_LE_ANY) && !bt_addr_le_eq(&old,peer) &&
           !host_address_elsewhere(&old,slot))bt_unpair(BT_ID_DEFAULT,&old);
        k_work_reschedule(&update_advertising_work,K_NO_WAIT);
    } else {
        bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        if(!bt_addr_le_eq(peer,&profiles[slot].peer) && !host_address_elsewhere(peer,slot))
            bt_unpair(BT_ID_DEFAULT,peer);
    }
    bt_conn_unref(conn);
    key=k_spin_lock(&host_pair_lock);host_pair_committing=false;k_spin_unlock(&host_pair_lock,key);
}
K_WORK_DEFINE(host_pair_commit_work,host_pair_commit);
#endif

static bool pairing_allowed_for_current_profile(struct bt_conn *conn) {
    if (!zmk_ble_host_is_enabled()) return false;
    if (!zmk_split_role_is_central()) {
        return true;
    }
#if defined(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || defined(CONFIG_BOARD_SOFLE_DONGLE) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) || ((defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
    if(host_address_elsewhere(bt_conn_get_dst(conn),active_profile))return false;
    if(zmk_ble_active_profile_is_open())return true;
    if(!IS_ENABLED(CONFIG_BT_SMP_ALLOW_UNAUTH_OVERWRITE))return false;
    if(bt_addr_le_eq(zmk_ble_active_profile_addr(),bt_conn_get_dst(conn)))return true;
    /* New host may replace the selected slot only while its old host is offline. */
    return !zmk_ble_active_profile_is_connected();
#else
    return zmk_ble_active_profile_is_open() ||
           (IS_ENABLED(CONFIG_BT_SMP_ALLOW_UNAUTH_OVERWRITE) &&
            bt_addr_le_cmp(zmk_ble_active_profile_addr(), bt_conn_get_dst(conn)) == 0);
#endif
}

static enum bt_security_err auth_pairing_accept(struct bt_conn *conn,
                                                const struct bt_conn_pairing_feat *const feat) {
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_CLIENT)
    if (zmk_keypoint_presence_probe_connection(conn)) {
        return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_SERVER)
    /* Probe connections must reuse the split bond, never take a host slot or
     * initiate a replacement pairing through the HID profile namespace. */
    if (zmk_keypoint_presence_is_control_connection(conn)) {
        return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
    if (zmk_keypoint_relay_is_connection(conn)) return zmk_mac_pair_allowed(conn) ? BT_SECURITY_ERR_SUCCESS : BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
#endif
    struct bt_conn_info info;
    bt_conn_get_info(conn, &info);

    LOG_DBG("role %d, open? %s", info.role, zmk_ble_active_profile_is_open() ? "yes" : "no");
    if (info.role == BT_CONN_ROLE_PERIPHERAL && !pairing_allowed_for_current_profile(conn)) {
        LOG_WRN("Rejecting pairing request to taken profile %d", active_profile);
        return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
    }

#if defined(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || defined(CONFIG_BOARD_SOFLE_DONGLE) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) || ((defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
    if(info.role==BT_CONN_ROLE_PERIPHERAL && !host_pair_capture(conn))return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
#endif
    return BT_SECURITY_ERR_SUCCESS;
};

static void auth_pairing_complete(struct bt_conn *conn, bool bonded) {
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
    if (zmk_keypoint_relay_is_connection(conn)) return;
#endif
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_SERVER)
    if (zmk_keypoint_presence_is_control_connection(conn)) { return; }
#endif
    if (!zmk_split_role_is_central()) {
        return;
    }
    struct bt_conn_info info;
    char addr[BT_ADDR_LE_STR_LEN];
    const bt_addr_le_t *dst = bt_conn_get_dst(conn);

    bt_addr_le_to_str(dst, addr, sizeof(addr));
    bt_conn_get_info(conn, &info);

    if (info.role != BT_CONN_ROLE_PERIPHERAL) {
        LOG_DBG("SKIPPING FOR ROLE %d", info.role);
        return;
    }

#if defined(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || defined(CONFIG_BOARD_SOFLE_DONGLE) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) || ((defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
    k_spinlock_key_t key=k_spin_lock(&host_pair_lock);
    bool captured=host_pair_conn==conn;
    if(captured)host_pair_completed=true;
    k_spin_unlock(&host_pair_lock,key);
    if(captured)k_work_submit(&host_pair_commit_work);
    else bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    return;
#endif
    if (!pairing_allowed_for_current_profile(conn)) {
        LOG_ERR("Pairing completed but current profile is not open: %s", addr);
        bt_unpair(BT_ID_DEFAULT, dst);
        return;
    }

    set_profile_address(active_profile, dst);
    update_advertising();
};

static struct bt_conn_auth_cb zmk_ble_auth_cb_display = {
    .pairing_accept = auth_pairing_accept,
// .passkey_display = auth_passkey_display,

#if IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY)
    .passkey_entry = auth_passkey_entry,
#endif
    .cancel = auth_cancel,
};

#if defined(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || defined(CONFIG_BOARD_SOFLE_DONGLE) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) || ((defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
static void auth_host_pairing_failed(struct bt_conn *conn,enum bt_security_err reason) {
#if (defined(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))
    LOG_WRN("Host pairing failed (security error %u)", reason);
#else
    ARG_UNUSED(reason);
#endif
    /* Relay pairing has its own identity and recovery handler. */
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
    if(zmk_keypoint_relay_is_connection(conn))return;
#endif
    struct bt_conn_info info;
    if(!bt_conn_get_info(conn,&info) && info.role==BT_CONN_ROLE_PERIPHERAL) {
        host_pair_cancel(conn);bt_conn_disconnect(conn,BT_HCI_ERR_AUTH_FAIL);
    }
}
#endif
static struct bt_conn_auth_info_cb zmk_ble_auth_info_cb_display = {
#if defined(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || defined(CONFIG_BOARD_SOFLE_DONGLE) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) || ((defined(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || ((defined(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (defined(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))))
    .pairing_failed = auth_host_pairing_failed,
#endif
    .pairing_complete = auth_pairing_complete,
};

static void zmk_ble_ready(int err) {
    LOG_DBG("ready? %d", err);
    if (err) {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return;
    }

    k_work_reschedule(&update_advertising_work, K_NO_WAIT);
}

static int zmk_ble_complete_startup(void) {

#if IS_ENABLED(CONFIG_ZMK_BLE_CLEAR_BONDS_ON_START)
    LOG_WRN("Clearing all existing BLE bond information from the keyboard");

    bt_unpair(BT_ID_DEFAULT, NULL);

    for (int i = 0; i < 8; i++) {
        char setting_name[15];
        sprintf(setting_name, "ble/profiles/%d", i);

        int err = settings_delete(setting_name);
        if (err) {
            LOG_ERR("Failed to delete setting: %d", err);
        }
    }

    // Hardcoding a reasonable hardcoded value of peripheral addresses
    // to clear so we properly clear a split central as well.
    for (int i = 0; i < 8; i++) {
        char setting_name[32];
        sprintf(setting_name, "ble/peripheral_addresses/%d", i);

        int err = settings_delete(setting_name);
        if (err) {
            LOG_ERR("Failed to delete setting: %d", err);
        }
    }

#endif // IS_ENABLED(CONFIG_ZMK_BLE_CLEAR_BONDS_ON_START)

    bt_conn_cb_register(&conn_callbacks);
    bt_conn_auth_cb_register(&zmk_ble_auth_cb_display);
    bt_conn_auth_info_cb_register(&zmk_ble_auth_info_cb_display);

    zmk_ble_ready(0);

    return 0;
}

static int zmk_ble_init(void) {
    int err = bt_enable(NULL);

    if (err < 0 && err != -EALREADY) {
        LOG_ERR("BLUETOOTH FAILED (%d)", err);
        return err;
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_register(&profiles_handler);
    k_work_init_delayable(&ble_save_work, ble_save_profile_work);
#else
    zmk_ble_complete_startup();
#endif

    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY)

static bool zmk_ble_numeric_usage_to_value(const zmk_key_t key, const zmk_key_t one,
                                           const zmk_key_t zero, uint8_t *value) {
    if (key < one || key > zero) {
        return false;
    }

    *value = (key == zero) ? 0 : (key - one + 1);
    return true;
}

static int zmk_ble_handle_key_user(struct zmk_keycode_state_changed *event) {
    zmk_key_t key = event->keycode;

    LOG_DBG("key %d", key);

    if (!auth_passkey_entry_conn) {
        LOG_DBG("No connection for passkey entry");
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (event->state) {
        LOG_DBG("Key press, ignoring");
        return ZMK_EV_EVENT_HANDLED;
    }

    if (key == HID_USAGE_KEY_KEYBOARD_ESCAPE) {
        bt_conn_auth_cancel(auth_passkey_entry_conn);
        return ZMK_EV_EVENT_HANDLED;
    }

    if (key == HID_USAGE_KEY_KEYBOARD_RETURN || key == HID_USAGE_KEY_KEYBOARD_RETURN_ENTER) {
        uint8_t digits[PASSKEY_DIGITS];
        uint32_t count = ring_buf_get(&passkey_entries, digits, PASSKEY_DIGITS);

        uint32_t passkey = 0;
        for (int i = 0; i < count; i++) {
            passkey = (passkey * 10) + digits[i];
        }

        LOG_DBG("Final passkey: %d", passkey);
        bt_conn_auth_passkey_entry(auth_passkey_entry_conn, passkey);
        bt_conn_unref(auth_passkey_entry_conn);
        auth_passkey_entry_conn = NULL;
        return ZMK_EV_EVENT_HANDLED;
    }

    uint8_t val;
    if (!(zmk_ble_numeric_usage_to_value(key, HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION,
                                         HID_USAGE_KEY_KEYBOARD_0_AND_RIGHT_PARENTHESIS, &val) ||
          zmk_ble_numeric_usage_to_value(key, HID_USAGE_KEY_KEYPAD_1_AND_END,
                                         HID_USAGE_KEY_KEYPAD_0_AND_INSERT, &val))) {
        LOG_DBG("Key not a number, ignoring");
        return ZMK_EV_EVENT_HANDLED;
    }

    if (ring_buf_space_get(&passkey_entries) <= 0) {
        uint8_t discard_val;
        ring_buf_get(&passkey_entries, &discard_val, 1);
    }
    ring_buf_put(&passkey_entries, &val, 1);
    LOG_DBG("value entered: %d, digits collected so far: %d", val,
            ring_buf_size_get(&passkey_entries));

    return ZMK_EV_EVENT_HANDLED;
}

static int zmk_ble_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *kc_state;

    kc_state = as_zmk_keycode_state_changed(eh);

    if (kc_state != NULL) {
        return zmk_ble_handle_key_user(kc_state);
    }

    return 0;
}

ZMK_LISTENER(zmk_ble, zmk_ble_listener);
ZMK_SUBSCRIPTION(zmk_ble, zmk_keycode_state_changed);
#endif /* IS_ENABLED(CONFIG_ZMK_BLE_PASSKEY_ENTRY) */

SYS_INIT(zmk_ble_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);
