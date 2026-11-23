/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "config_sync_journal.h"

/* Run on the configuration coordinator's serialized work queue, never an
 * interrupt handler. The coordinator must exclude Studio/behavior edits for
 * the duration of capture. Partial output on failure must not be committed. */
int keypoint_config_snapshot_capture(uint8_t *buffer, size_t capacity, size_t *length);
/* Validate against this firmware's layout and behavior registry before any
 * live configuration or durable snapshot is changed. */
int keypoint_config_snapshot_validate(const uint8_t *buffer, size_t length);
/* Transfer transaction commit callback: validate first, then durable journal
 * commit. Live apply is a separate step after durable acceptance. */
int keypoint_config_snapshot_commit(const uint8_t *buffer, size_t length,
                                    struct keypoint_config_revision revision, void *store);
/* Left-authority checkpoint. Caller has loaded the journal and excludes edits.
 * A failed capture/validation/write leaves the committed generation intact. */
int keypoint_config_snapshot_checkpoint(struct keypoint_config_store *store, uint8_t *buffer,
                                        size_t capacity, size_t *length);
/* Replay a durably committed snapshot through the firmware APIs. Caller must
 * stop input/playback and exclude edits until success. On storage failure,
 * keep input gated and retry the same committed snapshot (including at boot).
 * This operation is idempotent but its individual legacy settings writes are
 * not atomic: the journal is the recovery authority. */
int keypoint_config_snapshot_restore(struct keypoint_config_store *store,
                                     const uint8_t *buffer, size_t length);
/* Transaction callback with replay. A retry of the same durable revision
 * resumes replay without another journal commit after a legacy write failure.
 * Return success (and thus ACK) only once replay completes. */
int keypoint_config_snapshot_receive(const uint8_t *buffer, size_t length,
                                     struct keypoint_config_revision revision, void *store);
