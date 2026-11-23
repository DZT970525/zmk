/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zmk/split/role.h>
#include <zmk/split/peer_selection.h>
BUILD_ASSERT(CONFIG_BT_MAX_PAIRED >= 2, "Named split peers require bonds for two centrals");

#define PEER_SERVICE_UUID BT_UUID_128_ENCODE(0xcf03e404, 0xea4d, 0x4fbe, 0xa448, 0x7d839ed0bdb0)
#define PEER_IDENTITY_UUID BT_UUID_128_ENCODE(0xcf03e405, 0xea4d, 0x4fbe, 0xa448, 0x7d839ed0bdb0)
#define PEER_TARGET_UUID BT_UUID_128_ENCODE(0xcf03e406, 0xea4d, 0x4fbe, 0xa448, 0x7d839ed0bdb0)
static struct bt_uuid_128 identity_uuid = BT_UUID_INIT_128(PEER_IDENTITY_UUID);
static struct bt_uuid_128 target_uuid = BT_UUID_INIT_128(PEER_TARGET_UUID);

/* These are central identities, not host profile numbers or bond-list order.
 * The address is only learned from an encrypted identity read on a bonded link. */
static bt_addr_le_t peer_addresses[2];
static bool peer_known[2];
static atomic_t selected_target = ATOMIC_INIT(ZMK_KEYPOINT_PEER_DONGLE);
static struct bt_conn *identity_conn;
static struct bt_gatt_read_params identity_read;
static atomic_t connection_verified;
static bool peer_settings_committed;
static bool identity_pending;
static bool identity_attempted;
static uint8_t learned_kind;
static bt_addr_le_t learned_address;
static struct bt_conn *switch_conn;
static bool switch_pending;
static uint16_t switch_nonce;
static uint8_t switch_target;
K_MUTEX_DEFINE(peer_lock);
static void identity_query_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(identity_query_work, identity_query_handler);

static atomic_t remote_switch_busy;
static int64_t remote_switch_deadline;
static void remote_switch_timeout(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(remote_switch_timeout_work,remote_switch_timeout);
static struct {
    struct bt_conn *conn;
    struct bt_gatt_discover_params discover;
    struct bt_gatt_write_params write;
    uint8_t command[4];
    zmk_keypoint_peer_switch_callback_t callback;
    void *user_data;
} remote_switch;

static void remote_switch_complete(int status) {
    k_mutex_lock(&peer_lock, K_FOREVER);
    if (!atomic_cas(&remote_switch_busy, 1, 2)) {
        k_mutex_unlock(&peer_lock);
        return;
    }
    struct bt_conn *conn = remote_switch.conn;
    k_work_cancel_delayable(&remote_switch_timeout_work);
    zmk_keypoint_peer_switch_callback_t callback = remote_switch.callback;
    void *user_data = remote_switch.user_data;
    bt_gatt_cancel(conn, &remote_switch.discover);
    bt_gatt_cancel(conn, &remote_switch.write);
    remote_switch.conn = NULL;
    bt_conn_unref(conn);
    atomic_clear(&remote_switch_busy);
    k_mutex_unlock(&peer_lock);
    if (callback) callback(status, user_data);
}
static void remote_switch_timeout(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&peer_lock,K_FOREVER);
    if (atomic_get(&remote_switch_busy)==1) {
        int64_t remaining=remote_switch_deadline-k_uptime_get();
        if (remaining>0) {
            k_work_reschedule(&remote_switch_timeout_work,K_MSEC(remaining));
        } else { remote_switch_complete(-ETIMEDOUT); }
    }
    k_mutex_unlock(&peer_lock);
}
static void remote_target_written(struct bt_conn *conn, uint8_t err,
                                  struct bt_gatt_write_params *params) {
    ARG_UNUSED(params);
    k_mutex_lock(&peer_lock, K_FOREVER);
    if (conn == remote_switch.conn) remote_switch_complete(err ? -EIO : 0);
    k_mutex_unlock(&peer_lock);
}
static uint8_t remote_target_found(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  struct bt_gatt_discover_params *params) {
    ARG_UNUSED(params);
    k_mutex_lock(&peer_lock, K_FOREVER);
    if (conn != remote_switch.conn || !atomic_get(&remote_switch_busy)) {
        k_mutex_unlock(&peer_lock);
        return BT_GATT_ITER_STOP;
    }
    if (!attr) {
        remote_switch_complete(-ENOTSUP);
        k_mutex_unlock(&peer_lock);
        return BT_GATT_ITER_STOP;
    }
    const struct bt_gatt_chrc *characteristic = attr->user_data;
    remote_switch.write = (struct bt_gatt_write_params){
        .func = remote_target_written, .handle = characteristic->value_handle,
        .offset = 0, .data = remote_switch.command, .length = sizeof(remote_switch.command)};
    int ret = bt_gatt_write(conn, &remote_switch.write);
    if (ret) remote_switch_complete(ret);
    k_mutex_unlock(&peer_lock);
    return BT_GATT_ITER_STOP;
}

static int peer_switch_remote_version(struct bt_conn *conn, enum zmk_keypoint_peer_kind target,
                                    uint16_t nonce, zmk_keypoint_peer_switch_callback_t callback,
                                    void *user_data, uint8_t version) {
    if (!zmk_split_role_is_central()) return -ENOTSUP;
    if (!conn || (target != ZMK_KEYPOINT_PEER_DONGLE && target != ZMK_KEYPOINT_PEER_LEFT))
        return -EINVAL;
    if (bt_conn_get_security(conn) < BT_SECURITY_L2) return -EACCES;
    k_mutex_lock(&peer_lock, K_FOREVER);
    if (!atomic_cas(&remote_switch_busy, 0, 1)) {
        k_mutex_unlock(&peer_lock);
        return -EBUSY;
    }
    remote_switch.conn = bt_conn_ref(conn);
    remote_switch_deadline=k_uptime_get()+8000;
    k_work_reschedule(&remote_switch_timeout_work,K_MSEC(8000));
    remote_switch.callback = callback;
    remote_switch.user_data = user_data;
    remote_switch.command[0] = version;
    remote_switch.command[1] = target;
    sys_put_le16(nonce, remote_switch.command + 2);
    remote_switch.discover = (struct bt_gatt_discover_params){
        .uuid = &target_uuid.uuid, .func = remote_target_found,
        .start_handle = 1, .end_handle = UINT16_MAX, .type = BT_GATT_DISCOVER_CHARACTERISTIC};
    int ret = bt_gatt_discover(conn, &remote_switch.discover);
    if (ret) {
        /* A start failure has no asynchronous completion callback. */
        remote_switch.callback = NULL;
        remote_switch_complete(ret);
    }
    k_mutex_unlock(&peer_lock);
    return ret;
}

int zmk_keypoint_peer_switch_remote(struct bt_conn *conn, enum zmk_keypoint_peer_kind target,
    uint16_t nonce,zmk_keypoint_peer_switch_callback_t callback,void *context) {
    return peer_switch_remote_version(conn,target,nonce,callback,context,1);
}
int zmk_keypoint_peer_stage_remote(struct bt_conn *conn, enum zmk_keypoint_peer_kind target,
    uint16_t nonce,zmk_keypoint_peer_switch_callback_t callback,void *context) {
    return peer_switch_remote_version(conn,target,nonce,callback,context,3);
}
int zmk_keypoint_peer_release_remote(struct bt_conn *conn, enum zmk_keypoint_peer_kind target,
    uint16_t nonce,zmk_keypoint_peer_switch_callback_t callback,void *context) {
    return peer_switch_remote_version(conn,target,nonce,callback,context,2);
}
enum zmk_keypoint_peer_kind zmk_keypoint_local_peer_kind(void) {
#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE))
    return ZMK_KEYPOINT_PEER_DONGLE;
#elif (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE_LEFT) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || IS_ENABLED(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT))
    return ZMK_KEYPOINT_PEER_LEFT;
#else
    return ZMK_KEYPOINT_PEER_RIGHT;
#endif
}

enum zmk_keypoint_peer_kind zmk_keypoint_peer_target(void) { return atomic_get(&selected_target); }
bool zmk_keypoint_peer_connection_verified(void) { return atomic_get(&connection_verified) != 0; }
bool zmk_keypoint_peer_is_verified_connection(struct bt_conn *conn) {
    return conn && conn==identity_conn && atomic_get(&connection_verified) &&
           bt_conn_get_security(conn)>=BT_SECURITY_L2;
}

int zmk_keypoint_peer_target_address(bt_addr_le_t *address) {
    if (!address) return -EINVAL;
    k_mutex_lock(&peer_lock, K_FOREVER);
    int slot = atomic_get(&selected_target) - 1;
    int ret = peer_known[slot] ? 0 : -ENOENT;
    if (!ret) bt_addr_le_copy(address, &peer_addresses[slot]);
    k_mutex_unlock(&peer_lock);
    return ret;
}

int zmk_keypoint_peer_set_target(enum zmk_keypoint_peer_kind target) {
    if (IS_ENABLED(CONFIG_ZMK_KEYPOINT_RIGHT_FIXED_LEFT) && target!=ZMK_KEYPOINT_PEER_LEFT) return -ENOTSUP;
    if (!peer_settings_committed) return -EAGAIN;
    if (target != ZMK_KEYPOINT_PEER_DONGLE && target != ZMK_KEYPOINT_PEER_LEFT) return -EINVAL;
    if (zmk_keypoint_local_peer_kind() != ZMK_KEYPOINT_PEER_RIGHT &&
        target != ZMK_KEYPOINT_PEER_DONGLE) return -EINVAL;
    uint8_t value = target;
    int ret = settings_save_one("keypoint_peers/target", &value, sizeof(value));
    if (!ret) {
        atomic_set(&selected_target, value);
        atomic_clear(&connection_verified);
    }
    /* The topology coordinator must close the old connection after its ACK.
     * Changing a target never deletes either master's bond. */
    return ret;
}

static int peer_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *arg) {
    if (strcmp(name, "target") && strcmp(name, "dongle") && strcmp(name, "left")) return -ENOENT;
    if (peer_settings_committed) return 0;
    if (!strcmp(name, "target")) {
        uint8_t value;
        if (len != sizeof(value)) return -EINVAL;
        int ret = read_cb(arg, &value, sizeof(value));
        if (ret < 0) return ret;
        if (ret != (int)sizeof(value) || value < 1 || value > 2) return -EINVAL;
        if (zmk_keypoint_local_peer_kind() != ZMK_KEYPOINT_PEER_RIGHT && value != 1)
            return -EINVAL;
        atomic_set(&selected_target, value);
        return 0;
    }
    int slot = !strcmp(name, "dongle") ? 0 : !strcmp(name, "left") ? 1 : -1;
    if (slot < 0) return -ENOENT;
    bt_addr_le_t address;
    if (len != sizeof(address)) return -EINVAL;
    int ret = read_cb(arg, &address, sizeof(address));
    if (ret < 0) return ret;
    if (ret != (int)sizeof(address) || address.type > BT_ADDR_LE_RANDOM_ID ||
        bt_addr_le_eq(&address, BT_ADDR_LE_NONE)) return -EINVAL;
    peer_addresses[slot] = address;
    peer_known[slot] = true;
    return 0;
}
static int peer_settings_commit(void) {
    if (IS_ENABLED(CONFIG_ZMK_KEYPOINT_RIGHT_FIXED_LEFT)) atomic_set(&selected_target,ZMK_KEYPOINT_PEER_LEFT);
    peer_settings_committed = true; return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(keypoint_peers, "keypoint_peers", NULL, peer_settings_set,
                               peer_settings_commit, NULL);

static ssize_t identity_value(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset) {
    const uint8_t value[] = {1, zmk_keypoint_local_peer_kind(), zmk_split_role_is_central()};
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

static void switch_disconnect_handler(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&peer_lock, K_FOREVER);
    struct bt_conn *conn = switch_conn;
    switch_conn = NULL;
    k_mutex_unlock(&peer_lock);
    if (conn) {
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(conn);
    }
}
K_WORK_DELAYABLE_DEFINE(switch_disconnect_work, switch_disconnect_handler);

/* Version, desired central kind, little-endian request nonce. ACK the durable
 * target before disconnecting. An identical retry on this link is idempotent. */
static ssize_t target_command(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(attr);
    if (offset) return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    if (len != 4) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    if (flags & BT_GATT_WRITE_FLAG_PREPARE) return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    const uint8_t *command = buf;
    if (zmk_keypoint_local_peer_kind() != ZMK_KEYPOINT_PEER_RIGHT || (command[0] < 1 || command[0] > 3) ||
        command[1] < 1 || command[1] > 2) return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    if (conn != identity_conn || bt_conn_get_security(conn) < BT_SECURITY_L2)
        return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    uint16_t nonce = sys_get_le16(command + 2);
    if (command[0]==2) {
        if (!switch_pending || switch_nonce!=nonce || switch_target!=command[1])
            return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
        k_mutex_lock(&peer_lock,K_FOREVER);
        if (!switch_conn) switch_conn=bt_conn_ref(conn);
        k_mutex_unlock(&peer_lock);
        k_work_reschedule(&switch_disconnect_work,K_MSEC(100));
        return len;
    }
    if (switch_pending) {
        return switch_nonce == nonce && switch_target == command[1]
            ? len : BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    }
    if (!atomic_get(&connection_verified)) return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    k_mutex_lock(&peer_lock, K_FOREVER);
    bool busy = switch_conn != NULL;
    k_mutex_unlock(&peer_lock);
    if (busy) return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    int ret = zmk_keypoint_peer_set_target(command[1]);
    if (ret) return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    switch_pending = true;
    switch_nonce = nonce;
    switch_target = command[1];
    if (command[0]==3 || (command[1]==ZMK_KEYPOINT_PEER_DONGLE && learned_kind==ZMK_KEYPOINT_PEER_LEFT)) {
        /* Keep the old master link until the left commits its boot role and
         * reboots. A lost write response can then be retried with the same
         * nonce instead of stranding the left without proof of our target.
         * Next advertising uses the already durable dongle selection. */
        return len;
    }
    k_mutex_lock(&peer_lock, K_FOREVER);
    switch_conn = bt_conn_ref(conn);
    k_mutex_unlock(&peer_lock);
    k_work_reschedule(&switch_disconnect_work, K_MSEC(100));
    return len;
}

BT_GATT_SERVICE_DEFINE(keypoint_peer_identity,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(PEER_SERVICE_UUID)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(PEER_IDENTITY_UUID), BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ_ENCRYPT, identity_value, NULL, NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(PEER_TARGET_UUID), BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE_ENCRYPT, NULL, target_command, NULL));

static void match_bond(const struct bt_bond_info *info, void *data) {
    bool *found = data;
    if (identity_conn && bt_addr_le_eq(&info->addr, bt_conn_get_dst(identity_conn))) *found = true;
}

static void save_identity(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&peer_lock, K_FOREVER);
    int slot = learned_kind - 1;
    bt_addr_le_t address = learned_address;
    k_mutex_unlock(&peer_lock);
    if (slot >= 0 && slot < 2) {
        const char *key = slot == 0 ? "keypoint_peers/dongle" : "keypoint_peers/left";
        int ret = settings_save_one(key, &address, sizeof(address));
        if (!ret) {
            k_mutex_lock(&peer_lock, K_FOREVER);
            peer_addresses[slot] = address;
            peer_known[slot] = true;
            k_mutex_unlock(&peer_lock);
        }
    }
}
K_WORK_DELAYABLE_DEFINE(save_identity_work, save_identity);

static uint8_t identity_received(struct bt_conn *conn, uint8_t err,
                                 struct bt_gatt_read_params *params,
                                 const void *data, uint16_t length) {
    ARG_UNUSED(params);
    if (conn != identity_conn) return BT_GATT_ITER_STOP;
    identity_pending = false;
    if (err) {
        /* A failed queued ATT read must not permanently leave an encrypted
         * split connection unauthorized for configuration export. Retry while
         * connected; an actual mismatched identity still fails closed below. */
        identity_attempted=false;
        k_work_reschedule(&identity_query_work,K_MSEC(1000));
        return BT_GATT_ITER_STOP;
    }
    if (!data || length != 3) return BT_GATT_ITER_STOP;
    const uint8_t *identity = data;
    bool bonded = false;
    bt_foreach_bond(BT_ID_DEFAULT, match_bond, &bonded);
    if (!bonded || bt_conn_get_security(conn) < BT_SECURITY_L2 || identity[0] != 1 ||
        identity[1] != atomic_get(&selected_target) || identity[2] != 1) return BT_GATT_ITER_STOP;
    k_mutex_lock(&peer_lock, K_FOREVER);
    learned_kind = identity[1];
    bt_addr_le_copy(&learned_address, bt_conn_get_dst(conn));
    k_mutex_unlock(&peer_lock);
    atomic_set(&connection_verified, 1);
    k_work_reschedule(&save_identity_work, K_MSEC(20));
    return BT_GATT_ITER_STOP;
}

static void read_identity(struct bt_conn *conn) {
    if (identity_conn != conn || identity_pending || identity_attempted ||
        bt_conn_get_security(conn) < BT_SECURITY_L2) return;
    identity_read = (struct bt_gatt_read_params){
        .func = identity_received, .handle_count = 0,
        .by_uuid = {.start_handle = 1, .end_handle = UINT16_MAX, .uuid = &identity_uuid.uuid}};
    identity_pending = true;
    int ret = bt_gatt_read(conn, &identity_read);
    if (ret) {
        identity_pending = false;
        if (ret == -EAGAIN || ret == -ENOMEM || ret == -ENOBUFS || ret == -EBUSY)
            k_work_reschedule(&identity_query_work, K_MSEC(100));
    }
    else identity_attempted = true;
}

static void identity_query_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (identity_conn) read_identity(identity_conn);
}

static void peer_connected(struct bt_conn *conn, uint8_t err) {
    if (err || zmk_split_role_is_central() || identity_conn) return;
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) || info.role != BT_CONN_ROLE_PERIPHERAL) return;
    identity_conn = bt_conn_ref(conn);
    switch_pending = false;
    atomic_clear(&connection_verified);
    identity_attempted = false;
    k_work_reschedule(&identity_query_work, K_MSEC(25));
}
static void peer_security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err) {
    if (!err && level >= BT_SECURITY_L2 && conn == identity_conn)
        k_work_reschedule(&identity_query_work, K_MSEC(25));
}
static void peer_disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);
    if (conn == remote_switch.conn) remote_switch_complete(-ENOTCONN);
    if (conn != identity_conn) return;
    switch_pending = false;
    k_work_cancel_delayable(&identity_query_work);
    bt_gatt_cancel(conn, &identity_read);
    identity_pending = identity_attempted = false;
    atomic_clear(&connection_verified);
    bt_conn_unref(identity_conn);
    identity_conn = NULL;
}
static struct bt_conn_cb peer_callbacks = {
    .connected = peer_connected, .security_changed = peer_security_changed,
    .disconnected = peer_disconnected,
};
static void peer_pairing_complete(struct bt_conn *conn, bool bonded) {
    if (bonded && conn == identity_conn && !atomic_get(&connection_verified) && !identity_pending) {
        identity_attempted = false;
        k_work_reschedule(&identity_query_work, K_MSEC(25));
    }
}
static struct bt_conn_auth_info_cb peer_auth_callbacks = {.pairing_complete = peer_pairing_complete};
static int peer_selection_init(void) {
    bt_conn_cb_register(&peer_callbacks);
    bt_conn_auth_info_cb_register(&peer_auth_callbacks);
    return 0;
}
SYS_INIT(peer_selection_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);
