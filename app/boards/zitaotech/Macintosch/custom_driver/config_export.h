/* SPDX-License-Identifier: MIT */
#pragma once
#include "config_sync_transaction.h"

enum keypoint_config_export_state {
    KEYPOINT_EXPORT_IDLE,
    KEYPOINT_EXPORT_QUEUED,
    KEYPOINT_EXPORT_AVAILABLE,
    KEYPOINT_EXPORT_FAILED,
    KEYPOINT_PROPOSAL_RECEIVING,
    KEYPOINT_PROPOSAL_SAVE_QUEUED,
    KEYPOINT_PROPOSAL_SAVED,
    KEYPOINT_PROPOSAL_METADATA,
};
struct keypoint_config_export_status {
    enum keypoint_config_export_state state;
    uint32_t nonce;
    int result;
    size_t length;
    struct keypoint_config_revision revision;
};
/* Nonblocking start. The configuration worker freezes the durable snapshot.
 * Only the requesting bonded peer may consume or release this session. */
int zmk_keypoint_config_export_start(uint32_t nonce);
int zmk_keypoint_config_export_status(struct keypoint_config_export_status *status);
int zmk_keypoint_config_export_read(uint32_t nonce, size_t offset, void *buffer, size_t length);
int zmk_keypoint_config_export_release(uint32_t nonce);
/* Proposals carry the last left-acknowledged revision. Only the left assigns
 * the next generation. SAVED means durable acceptance, not live replay: while
 * peripheral, the left loads the accepted snapshot on its next role reboot.
 * Local Studio edits remain blocked until that boot replay completes. */
int zmk_keypoint_config_proposal_start(uint32_t nonce,struct keypoint_config_revision base);
int zmk_keypoint_config_proposal_metadata(uint32_t nonce,uint32_t crc,size_t length);
int zmk_keypoint_config_proposal_chunk(uint32_t nonce,size_t offset,const uint8_t *data,size_t length);
int zmk_keypoint_config_proposal_save(uint32_t nonce);
