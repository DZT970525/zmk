/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The left assigns revisions. A dongle edit is a proposal against its last
 * acknowledged revision, never a competing revision counter. */
struct keypoint_config_revision {
    uint64_t generation;
    uint32_t crc;
};

struct keypoint_config_transaction {
    uint8_t *staging;
    size_t capacity;
    size_t length;
    size_t received;
    struct keypoint_config_revision committed;
    struct keypoint_config_revision incoming;
    uint32_t schema;
    bool active;
};

/* The callback must validate the entire snapshot and durably commit it
 * atomically. It must leave the previous configuration intact on failure. */
typedef int (*keypoint_config_commit_cb)(const uint8_t *, size_t,
                                        struct keypoint_config_revision, void *);

uint32_t keypoint_config_crc32(const uint8_t *data, size_t length);
int keypoint_config_begin(struct keypoint_config_transaction *tx, uint32_t schema,
                         struct keypoint_config_revision base,
                         struct keypoint_config_revision incoming, size_t length);
int keypoint_config_chunk(struct keypoint_config_transaction *tx, size_t offset,
                         const uint8_t *data, size_t length);
int keypoint_config_commit(struct keypoint_config_transaction *tx,
                          keypoint_config_commit_cb callback, void *context);
void keypoint_config_abort(struct keypoint_config_transaction *tx);
