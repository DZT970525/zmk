/* SPDX-License-Identifier: MIT */
#include "config_sync_transaction.h"
#include <errno.h>
#include <string.h>

uint32_t keypoint_config_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
        }
    }
    return ~crc;
}

int keypoint_config_begin(struct keypoint_config_transaction *tx, uint32_t schema,
                         struct keypoint_config_revision base,
                         struct keypoint_config_revision incoming, size_t length) {
    if (tx->active) {
        return -EBUSY;
    }
    if (schema != tx->schema) {
        return -EPROTONOSUPPORT;
    }
    if (!tx->staging || !length || length > tx->capacity) {
        return -EMSGSIZE;
    }
    if (base.generation != tx->committed.generation || base.crc != tx->committed.crc ||
        incoming.generation <= base.generation) {
        return -ESTALE;
    }
    tx->incoming = incoming;
    tx->length = length;
    tx->received = 0;
    tx->active = true;
    return 0;
}

int keypoint_config_chunk(struct keypoint_config_transaction *tx, size_t offset,
                         const uint8_t *data, size_t length) {
    if (!tx->active) {
        return -ENOENT;
    }
    if (!data || !length || offset > tx->length || length > tx->length - offset) {
        return -EINVAL;
    }
    if (offset < tx->received) {
        /* A lost acknowledgement may cause an identical chunk retransmission. */
        if (length > tx->received - offset || memcmp(tx->staging + offset, data, length)) {
            return -EBADMSG;
        }
        return 0;
    }
    if (offset != tx->received) {
        return -EAGAIN;
    }
    memcpy(tx->staging + offset, data, length);
    tx->received += length;
    return 0;
}

int keypoint_config_commit(struct keypoint_config_transaction *tx,
                          keypoint_config_commit_cb callback, void *context) {
    if (!tx->active) {
        return -ENOENT;
    }
    if (tx->received != tx->length) {
        return -EAGAIN;
    }
    if (keypoint_config_crc32(tx->staging, tx->length) != tx->incoming.crc) {
        return -EBADMSG;
    }
    if (!callback) {
        return -EINVAL;
    }
    int ret = callback(tx->staging, tx->length, tx->incoming, context);
    if (ret != 0) {
        return ret;
    }
    tx->committed = tx->incoming;
    tx->active = false;
    return 0;
}

void keypoint_config_abort(struct keypoint_config_transaction *tx) {
    tx->active = false;
    tx->length = 0;
    tx->received = 0;
}
