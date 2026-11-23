/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/settings/settings.h>
#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>

#include <zmk/ble.h>
#include <zmk/endpoints_types.h>
#include <zmk/hog.h>
#include <zmk/hid.h>
#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
#include <zmk/pointing/resolution_multipliers.h>
#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

enum {
    HIDS_REMOTE_WAKE = BIT(0),
    HIDS_NORMALLY_CONNECTABLE = BIT(1),
};

struct hids_info {
    uint16_t version; /* version number of base USB HID Specification */
    uint8_t code;     /* country HID Device hardware is localized for. */
    uint8_t flags;
} __packed;

struct hids_report {
    uint8_t id;   /* report id */
    uint8_t type; /* report type */
} __packed;

static struct hids_info info = {
    .version = 0x0000,
    .code = 0x00,
    .flags = HIDS_NORMALLY_CONNECTABLE | HIDS_REMOTE_WAKE,
};

enum {
    HIDS_INPUT = 0x01,
    HIDS_OUTPUT = 0x02,
    HIDS_FEATURE = 0x03,
};

static struct hids_report input = {
    .id = ZMK_HID_REPORT_ID_KEYBOARD,
    .type = HIDS_INPUT,
};

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

static struct hids_report led_indicators = {
    .id = ZMK_HID_REPORT_ID_LEDS,
    .type = HIDS_OUTPUT,
};

#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

static struct hids_report consumer_input = {
    .id = ZMK_HID_REPORT_ID_CONSUMER,
    .type = HIDS_INPUT,
};

#if IS_ENABLED(CONFIG_ZMK_POINTING)

static struct hids_report mouse_input = {
    .id = ZMK_HID_REPORT_ID_MOUSE,
    .type = HIDS_INPUT,
};

#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)

static struct hids_report mouse_feature = {
    .id = ZMK_HID_REPORT_ID_MOUSE,
    .type = HIDS_FEATURE,
};

#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)

#endif // IS_ENABLED(CONFIG_ZMK_POINTING)

static bool host_requests_notification = false;
static uint8_t ctrl_point;
// static uint8_t proto_mode;

static ssize_t read_hids_info(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                              uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
                             sizeof(struct hids_info));
}

static ssize_t read_hids_report_ref(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                    void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
                             sizeof(struct hids_report));
}

static ssize_t read_hids_report_map(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                    void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, zmk_hid_report_desc,
                             sizeof(zmk_hid_report_desc));
}

static ssize_t read_hids_input_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                      void *buf, uint16_t len, uint16_t offset) {
    struct zmk_hid_keyboard_report_body *report_body = &zmk_hid_get_keyboard_report()->body;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, report_body,
                             sizeof(struct zmk_hid_keyboard_report_body));
}

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
static ssize_t write_hids_leds_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                      const void *buf, uint16_t len, uint16_t offset,
                                      uint8_t flags) {
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len != sizeof(struct zmk_hid_led_report_body)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    struct zmk_hid_led_report_body *report = (struct zmk_hid_led_report_body *)buf;
    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile < 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    struct zmk_endpoint_instance endpoint = {.transport = ZMK_TRANSPORT_BLE,
                                             .ble = {
                                                 .profile_index = profile,
                                             }};
    zmk_hid_indicators_process_report(report, endpoint);

    return len;
}

#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

static ssize_t read_hids_consumer_input_report(struct bt_conn *conn,
                                               const struct bt_gatt_attr *attr, void *buf,
                                               uint16_t len, uint16_t offset) {
    struct zmk_hid_consumer_report_body *report_body = &zmk_hid_get_consumer_report()->body;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, report_body,
                             sizeof(struct zmk_hid_consumer_report_body));
}

#if IS_ENABLED(CONFIG_ZMK_POINTING)

static ssize_t read_hids_mouse_input_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                            void *buf, uint16_t len, uint16_t offset) {
    struct zmk_hid_mouse_report_body *report_body = &zmk_hid_get_mouse_report()->body;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, report_body,
                             sizeof(struct zmk_hid_mouse_report_body));
}

#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)

static ssize_t read_hids_mouse_feature_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                              void *buf, uint16_t len, uint16_t offset) {

    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile < 0) {
        LOG_DBG("   BT_ATT_ERR_UNLIKELY");
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    struct zmk_endpoint_instance endpoint = {
        .transport = ZMK_TRANSPORT_BLE,
        .ble = {.profile_index = profile},
    };

    struct zmk_pointing_resolution_multipliers mult =
        zmk_pointing_resolution_multipliers_get_profile(endpoint);

    struct zmk_hid_mouse_resolution_feature_report_body report = {
        .wheel_res = mult.wheel,
        .hwheel_res = mult.hor_wheel,
    };

    return bt_gatt_attr_read(conn, attr, buf, len, offset, &report,
                             sizeof(struct zmk_hid_mouse_resolution_feature_report_body));
}

static ssize_t write_hids_mouse_feature_report(struct bt_conn *conn,
                                               const struct bt_gatt_attr *attr, const void *buf,
                                               uint16_t len, uint16_t offset, uint8_t flags) {
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len != sizeof(struct zmk_hid_mouse_resolution_feature_report_body)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    struct zmk_hid_mouse_resolution_feature_report_body *report =
        (struct zmk_hid_mouse_resolution_feature_report_body *)buf;
    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile < 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    struct zmk_endpoint_instance endpoint = {.transport = ZMK_TRANSPORT_BLE,
                                             .ble = {
                                                 .profile_index = profile,
                                             }};
    zmk_pointing_resolution_multipliers_process_report(report, endpoint);

    return len;
}

#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)

#endif // IS_ENABLED(CONFIG_ZMK_POINTING)

// static ssize_t write_proto_mode(struct bt_conn *conn,
//                                 const struct bt_gatt_attr *attr,
//                                 const void *buf, uint16_t len, uint16_t offset,
//                                 uint8_t flags)
// {
//     printk("PROTO CHANGED\n");
//     return 0;
// }

static void input_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    host_requests_notification = (value == BT_GATT_CCC_NOTIFY) ? 1 : 0;
}

static ssize_t write_ctrl_point(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    uint8_t *value = attr->user_data;

    if (offset + len > sizeof(ctrl_point)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(value + offset, buf, len);

    return len;
}

/* HID Service Declaration */
BT_GATT_SERVICE_DEFINE(
    hog_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
    //    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_PROTOCOL_MODE, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
    //                           BT_GATT_PERM_WRITE, NULL, write_proto_mode, &proto_mode),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_hids_info,
                           NULL, &info),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ, BT_GATT_PERM_READ_ENCRYPT,
                           read_hids_report_map, NULL, NULL),

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT, read_hids_input_report, NULL, NULL),
    BT_GATT_CCC(input_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &input),

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT, read_hids_consumer_input_report, NULL, NULL),
    BT_GATT_CCC(input_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &consumer_input),

#if IS_ENABLED(CONFIG_ZMK_POINTING)
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT, read_hids_mouse_input_report, NULL, NULL),
    BT_GATT_CCC(input_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &mouse_input),

#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           read_hids_mouse_feature_report, write_hids_mouse_feature_report, NULL),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &mouse_feature),
#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)

#endif // IS_ENABLED(CONFIG_ZMK_POINTING)

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT, NULL,
                           write_hids_leds_report, NULL),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &led_indicators),
#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, write_ctrl_point, &ctrl_point));

#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))
#define KEYPOINT_HOG_GUARDED 1
static atomic_t hid_notify_errors, hid_notify_last_error;
static bool keypoint_hid_ready(struct bt_conn *conn, unsigned attr) {
    return conn && bt_conn_get_security(conn) >= BT_SECURITY_L2 &&
           bt_gatt_is_subscribed(conn, &hog_svc.attrs[attr], BT_GATT_CCC_NOTIFY);
}
static bool keypoint_report_ready(unsigned attr) {
    struct bt_conn *conn = zmk_ble_active_profile_conn();
    bool ready = keypoint_hid_ready(conn, attr);
    if (conn) bt_conn_unref(conn);
    return ready;
}
#endif
K_THREAD_STACK_DEFINE(hog_q_stack, CONFIG_ZMK_BLE_THREAD_STACK_SIZE);

struct k_work_q hog_work_q;

K_MSGQ_DEFINE(zmk_hog_keyboard_msgq, sizeof(struct zmk_hid_keyboard_report_body),
              CONFIG_ZMK_BLE_KEYBOARD_REPORT_QUEUE_SIZE, 4);

void send_keyboard_report_callback(struct k_work *work) {
#if KEYPOINT_HOG_GUARDED
    if (!keypoint_report_ready(5)) { k_msgq_purge(&zmk_hog_keyboard_msgq); return; }
    int processed=0;
#endif
    struct zmk_hid_keyboard_report_body report;

    while (k_msgq_get(&zmk_hog_keyboard_msgq, &report, K_NO_WAIT) == 0) {
        struct bt_conn *conn = zmk_ble_active_profile_conn();
        if (conn == NULL) {
            return;
        }

#if KEYPOINT_HOG_GUARDED
        if (!keypoint_hid_ready(conn, 5)) {
            bt_conn_unref(conn); k_msgq_purge(&zmk_hog_keyboard_msgq); return;
        }
#endif
        struct bt_gatt_notify_params notify_params = {
            .attr = &hog_svc.attrs[5],
            .data = &report,
            .len = sizeof(report),
        };

        int err = bt_gatt_notify_cb(conn, &notify_params);
#if KEYPOINT_HOG_GUARDED
        if (err) { atomic_inc(&hid_notify_errors); atomic_set(&hid_notify_last_error, err); }
#endif
        if (err == -EPERM) {
            bt_conn_set_security(conn, BT_SECURITY_L2);
        } else if (err) {
            LOG_DBG("Error notifying %d", err);
        }

        bt_conn_unref(conn);
#if KEYPOINT_HOG_GUARDED
        if (++processed >= 12) { k_work_submit_to_queue(&hog_work_q, work); break; }
#endif
    }
}

K_WORK_DEFINE(hog_keyboard_work, send_keyboard_report_callback);

int zmk_hog_send_keyboard_report(struct zmk_hid_keyboard_report_body *report) {
#if KEYPOINT_HOG_GUARDED
    if (!keypoint_report_ready(5)) { k_msgq_purge(&zmk_hog_keyboard_msgq); return 0; }
#endif
#if KEYPOINT_HOG_GUARDED
    int err = k_msgq_put(&zmk_hog_keyboard_msgq, report, K_NO_WAIT);
#else
    int err = k_msgq_put(&zmk_hog_keyboard_msgq, report, K_MSEC(100));
#endif
    if (err) {
        switch (err) {
#if KEYPOINT_HOG_GUARDED
        case -ENOMSG:
#endif
        case -EAGAIN: {
            LOG_WRN("Keyboard message queue full, popping first message and queueing again");
            struct zmk_hid_keyboard_report_body discarded_report;
            k_msgq_get(&zmk_hog_keyboard_msgq, &discarded_report, K_NO_WAIT);
            return zmk_hog_send_keyboard_report(report);
        }
        default:
            LOG_WRN("Failed to queue keyboard report to send (%d)", err);
            return err;
        }
    }

    k_work_submit_to_queue(&hog_work_q, &hog_keyboard_work);

    return 0;
};

K_MSGQ_DEFINE(zmk_hog_consumer_msgq, sizeof(struct zmk_hid_consumer_report_body),
              CONFIG_ZMK_BLE_CONSUMER_REPORT_QUEUE_SIZE, 4);

void send_consumer_report_callback(struct k_work *work) {
#if KEYPOINT_HOG_GUARDED
    if (!keypoint_report_ready(9)) { k_msgq_purge(&zmk_hog_consumer_msgq); return; }
    int processed=0;
#endif
    struct zmk_hid_consumer_report_body report;

    while (k_msgq_get(&zmk_hog_consumer_msgq, &report, K_NO_WAIT) == 0) {
        struct bt_conn *conn = zmk_ble_active_profile_conn();
        if (conn == NULL) {
            return;
        }

#if KEYPOINT_HOG_GUARDED
        if (!keypoint_hid_ready(conn, 9)) {
            bt_conn_unref(conn); k_msgq_purge(&zmk_hog_consumer_msgq); return;
        }
#endif
        struct bt_gatt_notify_params notify_params = {
            .attr = &hog_svc.attrs[9],
            .data = &report,
            .len = sizeof(report),
        };

        int err = bt_gatt_notify_cb(conn, &notify_params);
#if KEYPOINT_HOG_GUARDED
        if (err) { atomic_inc(&hid_notify_errors); atomic_set(&hid_notify_last_error, err); }
#endif
        if (err == -EPERM) {
            bt_conn_set_security(conn, BT_SECURITY_L2);
        } else if (err) {
            LOG_DBG("Error notifying %d", err);
        }

        bt_conn_unref(conn);
#if KEYPOINT_HOG_GUARDED
        if (++processed >= 12) { k_work_submit_to_queue(&hog_work_q, work); break; }
#endif
    }
};

K_WORK_DEFINE(hog_consumer_work, send_consumer_report_callback);

int zmk_hog_send_consumer_report(struct zmk_hid_consumer_report_body *report) {
#if KEYPOINT_HOG_GUARDED
    if (!keypoint_report_ready(9)) { k_msgq_purge(&zmk_hog_consumer_msgq); return 0; }
#endif
#if KEYPOINT_HOG_GUARDED
    int err = k_msgq_put(&zmk_hog_consumer_msgq, report, K_NO_WAIT);
#else
    int err = k_msgq_put(&zmk_hog_consumer_msgq, report, K_MSEC(100));
#endif
    if (err) {
        switch (err) {
#if KEYPOINT_HOG_GUARDED
        case -ENOMSG:
#endif
        case -EAGAIN: {
            LOG_WRN("Consumer message queue full, popping first message and queueing again");
            struct zmk_hid_consumer_report_body discarded_report;
            k_msgq_get(&zmk_hog_consumer_msgq, &discarded_report, K_NO_WAIT);
            return zmk_hog_send_consumer_report(report);
        }
        default:
            LOG_WRN("Failed to queue consumer report to send (%d)", err);
            return err;
        }
    }

    k_work_submit_to_queue(&hog_work_q, &hog_consumer_work);

    return 0;
};

#if IS_ENABLED(CONFIG_ZMK_POINTING)

#if (defined(CONFIG_ZITAOTECH_Q20_MOUSE_MAX_AGE_MS) && CONFIG_ZITAOTECH_Q20_MOUSE_MAX_AGE_MS > 0)
#include "q20_mouse_age.h"
typedef struct q20_mouse_queued_report hog_mouse_queue_item;
#else
typedef struct zmk_hid_mouse_report_body hog_mouse_queue_item;
#endif

K_MSGQ_DEFINE(zmk_hog_mouse_msgq, sizeof(hog_mouse_queue_item),
              CONFIG_ZMK_BLE_MOUSE_REPORT_QUEUE_SIZE, 4);

#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))
/* Link presence alone does not mean the host has subscribed to mouse HID.
 * Discard pre-handshake motion instead of filling queues during reconnect. */
static bool keypoint_mouse_subscribed(void) {
    struct bt_conn *conn=zmk_ble_active_profile_conn();
    if(!conn)return false;
    bool ready=keypoint_hid_ready(conn,13);
    bt_conn_unref(conn);
    return ready;
}
#endif
void send_mouse_report_callback(struct k_work *work) {
#if (defined(CONFIG_ZITAOTECH_Q20_MOUSE_MAX_AGE_MS) && CONFIG_ZITAOTECH_Q20_MOUSE_MAX_AGE_MS > 0)
    struct q20_mouse_age_state age_state = {0};
#endif
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))
    if(!keypoint_mouse_subscribed()) {
        k_msgq_purge(&zmk_hog_mouse_msgq);
#if (defined(CONFIG_ZITAOTECH_Q20_MOUSE_MAX_AGE_MS) && CONFIG_ZITAOTECH_Q20_MOUSE_MAX_AGE_MS > 0)
        age_state.buttons_valid = false;
#endif
        return;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))
    int processed=0;
#endif
    hog_mouse_queue_item item;
    while (k_msgq_get(&zmk_hog_mouse_msgq, &item, K_NO_WAIT) == 0) {
#if (defined(CONFIG_ZITAOTECH_Q20_MOUSE_MAX_AGE_MS) && CONFIG_ZITAOTECH_Q20_MOUSE_MAX_AGE_MS > 0)
        if (!q20_mouse_prepare(&item, k_uptime_get_32(), &age_state)) continue;
        struct zmk_hid_mouse_report_body report = item.report;
#else
        struct zmk_hid_mouse_report_body report = item;
#endif
        struct bt_conn *conn = zmk_ble_active_profile_conn();
        if (conn == NULL) {
            return;
        }

#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))
        if(!keypoint_hid_ready(conn,13)) {
            bt_conn_unref(conn); k_msgq_purge(&zmk_hog_mouse_msgq); return;
        }
#endif
        struct bt_gatt_notify_params notify_params = {
            .attr = &hog_svc.attrs[13],
            .data = &report,
            .len = sizeof(report),
        };

        int err = bt_gatt_notify_cb(conn, &notify_params);
#if (defined(CONFIG_ZITAOTECH_Q20_MOUSE_MAX_AGE_MS) && CONFIG_ZITAOTECH_Q20_MOUSE_MAX_AGE_MS > 0)
        if (!err) {
            age_state.buttons = report.buttons;
            age_state.buttons_valid = true;
        } else {
            age_state.buttons_valid = false;
        }
#endif
#if KEYPOINT_HOG_GUARDED
        if (err) { atomic_inc(&hid_notify_errors); atomic_set(&hid_notify_last_error, err); }
#endif
        if (err == -EPERM) {
            bt_conn_set_security(conn, BT_SECURITY_L2);
        } else if (err) {
            LOG_DBG("Error notifying %d", err);
        }

        bt_conn_unref(conn);
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))
        if(++processed>=CONFIG_ZMK_BLE_MOUSE_REPORT_QUEUE_SIZE) {
            k_work_submit_to_queue(&hog_work_q,work); break;
        }
#endif
    }
};

K_WORK_DEFINE(hog_mouse_work, send_mouse_report_callback);

int zmk_hog_send_mouse_report(struct zmk_hid_mouse_report_body *report) {
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE)) || (IS_ENABLED(CONFIG_BOARD_BBP9981) && !IS_ENABLED(CONFIG_ZITAOTECH_Q20_LEGACY_BLE))
    if(!keypoint_mouse_subscribed()) { k_msgq_purge(&zmk_hog_mouse_msgq); return 0; }
#if (defined(CONFIG_ZITAOTECH_Q20_MOUSE_MAX_AGE_MS) && CONFIG_ZITAOTECH_Q20_MOUSE_MAX_AGE_MS > 0)
    hog_mouse_queue_item item = { .report = *report, .queued_at = k_uptime_get_32() };
    const hog_mouse_queue_item *queued_report = &item;
#else
    const hog_mouse_queue_item *queued_report = report;
#endif
    int result=k_msgq_put(&zmk_hog_mouse_msgq,queued_report,K_NO_WAIT);
    if(result==-ENOMSG || result==-EAGAIN) {
        hog_mouse_queue_item discarded;
        k_msgq_get(&zmk_hog_mouse_msgq,&discarded,K_NO_WAIT);
        result=k_msgq_put(&zmk_hog_mouse_msgq,queued_report,K_NO_WAIT);
    }
    k_work_submit_to_queue(&hog_work_q,&hog_mouse_work);
    return result;
#else
    int err = k_msgq_put(&zmk_hog_mouse_msgq, report, K_MSEC(100));
    if (err) {
        switch (err) {
#if KEYPOINT_HOG_GUARDED
        case -ENOMSG:
#endif
        case -EAGAIN: {
            LOG_WRN("Consumer message queue full, popping first message and queueing again");
            struct zmk_hid_mouse_report_body discarded_report;
            k_msgq_get(&zmk_hog_mouse_msgq, &discarded_report, K_NO_WAIT);
            return zmk_hog_send_mouse_report(report);
        }
        default:
            LOG_WRN("Failed to queue mouse report to send (%d)", err);
            return err;
        }
    }

    k_work_submit_to_queue(&hog_work_q, &hog_mouse_work);

    return 0;
#endif
};
#endif // IS_ENABLED(CONFIG_ZMK_POINTING)

#if KEYPOINT_HOG_GUARDED
void zmk_hog_diagnostics(uint8_t bytes[8]) {
    bytes[0]=k_msgq_num_used_get(&zmk_hog_keyboard_msgq);
#if IS_ENABLED(CONFIG_ZMK_POINTING)
    bytes[1]=k_msgq_num_used_get(&zmk_hog_mouse_msgq);
#else
    bytes[1]=0;
#endif
    bytes[2]=k_msgq_num_used_get(&zmk_hog_consumer_msgq);
    bytes[3]=(keypoint_report_ready(5)?1:0)|(keypoint_report_ready(13)?2:0);
    sys_put_le16(atomic_get(&hid_notify_errors),bytes+4);
    sys_put_le16((uint16_t)atomic_get(&hid_notify_last_error),bytes+6);
}
#endif
static int zmk_hog_init(void) {
    static const struct k_work_queue_config queue_config = {.name = "HID Over GATT Send Work"};
    k_work_queue_start(&hog_work_q, hog_q_stack, K_THREAD_STACK_SIZEOF(hog_q_stack),
                       CONFIG_ZMK_BLE_THREAD_PRIORITY, &queue_config);

    return 0;
}

SYS_INIT(zmk_hog_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);
