/* SPDX-License-Identifier: MIT */
#include "config_sync_journal.h"
#include <errno.h>
#include <string.h>

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static void write32(uint8_t *p, uint32_t value) {
    for (unsigned int i = 0; i < 4; i++) { p[i] = value >> (8 * i); }
}
struct snapshot_header {
    uint32_t schema;
    uint32_t length;
    struct keypoint_config_revision revision;
};
static int read_header(struct keypoint_config_store *store, unsigned int slot,
                       struct snapshot_header *header) {
    uint8_t encoded[KEYPOINT_CONFIG_HEADER_SIZE];
    size_t length;
    int ret = store->read(store->context, slot, 0, encoded, sizeof(encoded), &length);
    if (ret) { return ret; }
    if (length != sizeof(encoded) || read32(encoded) != 0x3143504b) { return -EBADMSG; }
    header->schema = read32(encoded + 4);
    header->length = read32(encoded + 8);
    header->revision.generation = read32(encoded + 12) |
                                  (uint64_t)read32(encoded + 16) << 32;
    header->revision.crc = read32(encoded + 20);
    if (header->schema != store->schema) { return -EPROTONOSUPPORT; }
    if (!header->length || header->length > KEYPOINT_CONFIG_MAX_SIZE ||
        !header->revision.generation) { return -EBADMSG; }
    return 0;
}
static int read_snapshot(struct keypoint_config_store *store, unsigned int slot,
                         const struct snapshot_header *header, uint8_t *data) {
    size_t offset = 0;
    unsigned int record = 1;
    while (offset < header->length) {
        size_t expected = header->length - offset;
        if (expected > KEYPOINT_CONFIG_CHUNK_SIZE) { expected = KEYPOINT_CONFIG_CHUNK_SIZE; }
        size_t actual;
        int ret = store->read(store->context, slot, record++, data + offset, expected, &actual);
        if (ret) { return ret; }
        if (actual != expected) { return -EBADMSG; }
        offset += actual;
    }
    return keypoint_config_crc32(data, header->length) == header->revision.crc ? 0 : -EBADMSG;
}

int keypoint_config_store_load(struct keypoint_config_store *store, uint8_t *data,
                               size_t capacity, size_t *length) {
    if (!store || !store->read || !data || !length) { return -EINVAL; }
    struct snapshot_header headers[2];
    int errors[2] = {read_header(store, 0, &headers[0]), read_header(store, 1, &headers[1])};
    unsigned int first = (!errors[1] && (errors[0] ||
        headers[1].revision.generation > headers[0].revision.generation)) ? 1 : 0;
    int error = -ENOENT;
    for (unsigned int i = 0; i < 2; i++) {
        unsigned int slot = first ^ i;
        if (errors[slot]) {
            if (errors[slot] != -ENOENT) { error = errors[slot]; }
            continue;
        }
        /* Never silently load an older revision just because the caller's
         * buffer is too small for the newest snapshot. */
        if (headers[slot].length > capacity) { return -EMSGSIZE; }
        error = read_snapshot(store, slot, &headers[slot], data);
        if (error) { continue; }
        store->active_slot = slot;
        store->committed = headers[slot].revision;
        *length = headers[slot].length;
        return 0;
    }
    return error;
}

int keypoint_config_store_commit(struct keypoint_config_store *store, const uint8_t *data,
                                 size_t length, struct keypoint_config_revision revision) {
    if (!store || !store->write || !data || store->active_slot < -1 ||
        store->active_slot > 1) { return -EINVAL; }
    if (!length || length > KEYPOINT_CONFIG_MAX_SIZE) { return -EMSGSIZE; }
    if (revision.generation <= store->committed.generation) { return -ESTALE; }
    if (keypoint_config_crc32(data, length) != revision.crc) { return -EBADMSG; }
    unsigned int slot = store->active_slot == 0 ? 1 : 0;
    uint8_t header[KEYPOINT_CONFIG_HEADER_SIZE] = {0};
    /* Invalidate the inactive slot before replacing any of its chunks. The
     * currently active slot stays intact throughout a failed write. */
    int ret = store->write(store->context, slot, 0, header, sizeof(header));
    if (ret) { return ret; }
    size_t offset = 0;
    unsigned int record = 1;
    while (offset < length) {
        size_t chunk = length - offset;
        if (chunk > KEYPOINT_CONFIG_CHUNK_SIZE) { chunk = KEYPOINT_CONFIG_CHUNK_SIZE; }
        ret = store->write(store->context, slot, record++, data + offset, chunk);
        if (ret) { return ret; }
        offset += chunk;
    }
    write32(header, 0x3143504b);
    write32(header + 4, store->schema);
    write32(header + 8, length);
    write32(header + 12, revision.generation);
    write32(header + 16, revision.generation >> 32);
    write32(header + 20, revision.crc);
    ret = store->write(store->context, slot, 0, header, sizeof(header));
    if (ret) { return ret; }
    store->active_slot = slot;
    store->committed = revision;
    return 0;
}
