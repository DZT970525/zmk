/* SPDX-License-Identifier: MIT */
#pragma once
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/conn.h>

enum zmk_keypoint_peer_kind {
    ZMK_KEYPOINT_PEER_DONGLE = 1,
    ZMK_KEYPOINT_PEER_LEFT = 2,
    ZMK_KEYPOINT_PEER_RIGHT = 3,
};

enum zmk_keypoint_peer_kind zmk_keypoint_local_peer_kind(void);
enum zmk_keypoint_peer_kind zmk_keypoint_peer_target(void);
int zmk_keypoint_peer_target_address(bt_addr_le_t *address);
int zmk_keypoint_peer_set_target(enum zmk_keypoint_peer_kind target);
bool zmk_keypoint_peer_connection_verified(void);
bool zmk_keypoint_peer_is_verified_connection(struct bt_conn *conn);
typedef void (*zmk_keypoint_peer_switch_callback_t)(int status, void *user_data);
int zmk_keypoint_peer_switch_remote(struct bt_conn *conn, enum zmk_keypoint_peer_kind target,
                                    uint16_t nonce, zmk_keypoint_peer_switch_callback_t callback,
                                    void *user_data);
/* Central only. The caller owns the returned connection reference. */
struct bt_conn *zmk_keypoint_input_peer_connection(uint8_t input_reg);
/* Stage keeps the old link alive after durable ACK; release disconnects only
 * an identical staged transaction. Both calls support nonce-identical retries. */
int zmk_keypoint_peer_stage_remote(struct bt_conn *conn, enum zmk_keypoint_peer_kind target,
    uint16_t nonce,zmk_keypoint_peer_switch_callback_t callback,void *context);
int zmk_keypoint_peer_release_remote(struct bt_conn *conn, enum zmk_keypoint_peer_kind target,
    uint16_t nonce,zmk_keypoint_peer_switch_callback_t callback,void *context);
