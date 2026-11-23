/* SPDX-License-Identifier: MIT */
#pragma once
#include "config_sync_transaction.h"

#define KEYPOINT_CONFIG_CHUNK_SIZE 256
#define KEYPOINT_CONFIG_MAX_SIZE 12288
#define KEYPOINT_CONFIG_HEADER_SIZE 24

/* Each write is one atomic settings record. Callers serialize access to the
 * journal; an unsuccessful commit must never be acknowledged to the sender. */
struct keypoint_config_store {
    int (*read)(void *context, unsigned int slot, unsigned int record,
                uint8_t *data, size_t capacity, size_t *length);
    int (*write)(void *context, unsigned int slot, unsigned int record,
                 const uint8_t *data, size_t length);
    void *context;
    uint32_t schema;
    struct keypoint_config_revision committed;
    int active_slot;
};

/* Initialize active_slot to -1. Load verifies both complete snapshots before
 * selecting the newest, falling back to an older intact snapshot if needed. */
int keypoint_config_store_load(struct keypoint_config_store *store, uint8_t *data,
                               size_t capacity, size_t *length);
int keypoint_config_store_commit(struct keypoint_config_store *store, const uint8_t *data,
                                 size_t length, struct keypoint_config_revision revision);
/* Firmware adapter. Call after settings initialization, then load before the
 * first commit so existing generations/slots cannot be overwritten blindly. */
void keypoint_config_settings_init(struct keypoint_config_store *store, uint32_t schema);
