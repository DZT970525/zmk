/* SPDX-License-Identifier: MIT */
#include "config_sync_journal.h"
#include <errno.h>
#include <stdio.h>
#include <zephyr/settings/settings.h>

struct record_read {
    uint8_t *data;
    size_t capacity;
    size_t length;
    int status;
};
static int read_callback(const char *name, size_t length, settings_read_cb read_cb,
                          void *argument, void *context) {
    struct record_read *read = context;
    if (name && name[0]) { return 0; }
    if (length > read->capacity) { read->status = -EMSGSIZE; return 0; }
    int ret = read_cb(argument, read->data, length);
    read->status = ret < 0 ? ret : ((size_t)ret == length ? 0 : -EBADMSG);
    read->length = length;
    return 0;
}
static int read_record(void *context, unsigned int slot, unsigned int record,
                        uint8_t *data, size_t capacity, size_t *length) {
    (void)context;
    char key[40];
    snprintf(key, sizeof(key), "keypoint_config/%u/%u", slot, record);
    struct record_read read = {.data=data, .capacity=capacity, .status=-ENOENT};
    int ret = settings_load_subtree_direct(key, read_callback, &read);
    if (ret) { return ret; }
    if (read.status) { return read.status; }
    *length = read.length;
    return 0;
}
static int write_record(void *context, unsigned int slot, unsigned int record,
                         const uint8_t *data, size_t length) {
    (void)context;
    char key[40];
    snprintf(key, sizeof(key), "keypoint_config/%u/%u", slot, record);
    return settings_save_one(key, data, length);
}
void keypoint_config_settings_init(struct keypoint_config_store *store, uint32_t schema) {
    *store = (struct keypoint_config_store){.read=read_record, .write=write_record,
                                           .schema=schema, .active_slot=-1};
}
