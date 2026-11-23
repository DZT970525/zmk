/* SPDX-License-Identifier: MIT */
#pragma once
#include "config_sync_transaction.h"
#include <zephyr/bluetooth/conn.h>
typedef void (*keypoint_config_download_callback)(int result,
    struct keypoint_config_revision revision,size_t length,void *context);
/* Caller owns buffer until completion. No full snapshot allocation is made.
 * A successful callback means the frozen snapshot's entire CRC was verified;
 * the caller must validate/apply/journal it before acknowledging a handoff. */
int zmk_keypoint_config_download(struct bt_conn *left,uint32_t nonce,
    uint8_t *buffer,size_t capacity,keypoint_config_download_callback callback,void *context);
/* Successful completion means the left has durably accepted the proposal and
 * assigned the returned revision. Live left replay occurs at the role reboot.
 * Caller keeps this buffer immutable until completion. */
int zmk_keypoint_config_propose(struct bt_conn *left,uint32_t nonce,
    struct keypoint_config_revision base,const uint8_t *buffer,size_t length,
    keypoint_config_download_callback callback,void *context);
