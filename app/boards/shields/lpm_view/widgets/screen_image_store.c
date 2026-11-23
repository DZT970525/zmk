/*
 * Copyright (c) 2026 ZitaoTech
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include "screen_image_store.h"
#include <stdlib.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Small enough to live comfortably in one settings entry. */
#define FRAGMENT_BYTES 1024
#define FRAGMENT_COUNT                                                                             \
    ((ZITAOTECH_SCREEN_IMAGE_MAX_BYTES + FRAGMENT_BYTES - 1) / FRAGMENT_BYTES)

#define IMAGE_MAGIC 0x5a494d47 /* "ZIMG" */

struct image_header {
    uint32_t magic;
    uint32_t crc32;
    uint32_t size;
    uint32_t format;
};

/*
 * The headers are held in RAM: they are tiny, they only change when the web UI
 * stores a picture, and having them here keeps listing (the hot path) free of
 * settings loads.
 */
static int32_t last_slot = -1;
int zitaotech_screen_image_last_slot(void) { return last_slot; }
static struct image_header headers[ZITAOTECH_SCREEN_IMAGE_SLOTS];
/* Fixed working set: central keeps only the current outbound picture;
 * peripheral reads one committed Flash picture on demand. */
static uint8_t image_cache[ZITAOTECH_SCREEN_IMAGE_MAX_BYTES];
static int cached_slot = -1;
K_MUTEX_DEFINE(image_cache_lock);
static uint8_t upload_owner;
uint8_t *zitaotech_screen_image_upload_buffer(uint8_t owner) {
    k_mutex_lock(&image_cache_lock, K_FOREVER);
    if (upload_owner && upload_owner != owner) { k_mutex_unlock(&image_cache_lock); return NULL; }
    upload_owner = owner;
    cached_slot = -1;
    k_mutex_unlock(&image_cache_lock);
    return image_cache;
}
void zitaotech_screen_image_upload_end(uint8_t owner) {
    k_mutex_lock(&image_cache_lock, K_FOREVER);
    if (upload_owner == owner) upload_owner = 0;
    k_mutex_unlock(&image_cache_lock);
}
static uint16_t fragment_lengths[ZITAOTECH_SCREEN_IMAGE_SLOTS][FRAGMENT_COUNT];

static void settings_name(uint32_t slot, const char *leaf, char *out, size_t out_size) {
    snprintf(out, out_size, "zitaotech_img/%u/%s", slot, leaf);
}

int zitaotech_screen_image_write(uint32_t slot, uint32_t format, const uint8_t *data, size_t size) {
    if (slot >= ZITAOTECH_SCREEN_IMAGE_SLOTS || data == NULL || size == 0 ||
        size > ZITAOTECH_SCREEN_IMAGE_MAX_BYTES || size != (format == 1 ? 1080 : 4320)) {
        return -EINVAL;
    }
    if (format != ZITAOTECH_SCREEN_IMAGE_FORMAT_RGB111 &&
        format != ZITAOTECH_SCREEN_IMAGE_FORMAT_MONO) {
        return -EINVAL;
    }

    char name[40];
    size_t offset = 0;

    if (!IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) for (size_t fragment = 0; fragment < FRAGMENT_COUNT; fragment++) {
        char leaf[8];
        size_t chunk = MIN((size_t)FRAGMENT_BYTES, size - offset);

        snprintf(leaf, sizeof(leaf), "f%u", (unsigned)fragment);
        settings_name(slot, leaf, name, sizeof(name));

        if (chunk == 0) {
            /* A smaller picture leaves no stale tail behind. */
            settings_delete(name);
        } else {
            int ret = settings_save_one(name, data + offset, chunk);
            if (ret < 0) {
                return ret;
            }
        }
        offset += chunk;
    }

    /* The header goes last: a picture only counts as stored once it is here,
     * so a transfer that is interrupted cannot be mistaken for a good one. */
    struct image_header header = {
        .magic = IMAGE_MAGIC,
        .crc32 = crc32_ieee(data, size),
        .size = (uint32_t)size,
        .format = format,
    };

    settings_name(slot, "meta", name, sizeof(name));
    int ret = settings_save_one(name, &header, sizeof(header));
    if (ret < 0) {
        return ret;
    }

    int32_t saved_slot = slot;
    ret = settings_save_one("zitaotech_img/last", &saved_slot, sizeof(saved_slot));
    if (ret < 0) return ret;
    last_slot = saved_slot;
    k_mutex_lock(&image_cache_lock, K_FOREVER);
    if (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) {
        memmove(image_cache, data, size);
        cached_slot = slot;
    } else if (cached_slot == slot) cached_slot = -1;
    k_mutex_unlock(&image_cache_lock);
    headers[slot] = header;
    LOG_INF("Screen picture slot %u: %u bytes, crc %08x, format %u", (unsigned)slot,
            (unsigned)size, (unsigned)header.crc32, (unsigned)format);
    return 0;
}

int zitaotech_screen_image_erase(uint32_t slot) {
    if (slot >= ZITAOTECH_SCREEN_IMAGE_SLOTS) {
        return -EINVAL;
    }

    char name[40];
    settings_name(slot, "meta", name, sizeof(name));
    int ret = settings_delete(name);
    if (ret) return ret;
    /* Invalidate first: interrupted cleanup must not resurrect a picture. */
    headers[slot] = (struct image_header){0};
    if (cached_slot == slot) cached_slot = -1;
    for (size_t fragment = 0; fragment < FRAGMENT_COUNT; fragment++) {
        char leaf[8];
        snprintf(leaf, sizeof(leaf), "f%u", (unsigned)fragment);
        settings_name(slot, leaf, name, sizeof(name));
        settings_delete(name);
    }

    return 0;
}

bool zitaotech_screen_image_info(uint32_t slot, struct zitaotech_screen_image_info *out) {
    if (slot >= ZITAOTECH_SCREEN_IMAGE_SLOTS || out == NULL) {
        return false;
    }

    const struct image_header *header = &headers[slot];
    if (header->magic != IMAGE_MAGIC || header->size == 0) {
        return false;
    }

    *out = (struct zitaotech_screen_image_info){
        .slot = slot,
        .crc32 = header->crc32,
        .size = header->size,
        .format = header->format,
    };
    return true;
}

int zitaotech_screen_image_read_commit(uint32_t slot) {
    if (slot >= ZITAOTECH_SCREEN_IMAGE_SLOTS || headers[slot].magic != IMAGE_MAGIC) return -EINVAL;
    k_mutex_lock(&image_cache_lock, K_FOREVER);
    int err = crc32_ieee(image_cache,headers[slot].size) == headers[slot].crc32 ? 0 : -EBADMSG;
    if (!err) cached_slot = slot;
    k_mutex_unlock(&image_cache_lock);
    return err;
}
struct cache_load { size_t lengths[FRAGMENT_COUNT]; bool failed; };
static int cache_read(const char *name, size_t len, settings_read_cb cb, void *arg, void *param) {
    struct cache_load *load = param;
    if (name[0] != 'f' || name[1] < '0' || name[1] >= '0' + FRAGMENT_COUNT || name[2]) return 0;
    unsigned f = name[1] - '0';
    if (!len || len > FRAGMENT_BYTES || cb(arg, image_cache + f * FRAGMENT_BYTES, len) != len) {
        load->failed = true; return -EINVAL;
    }
    load->lengths[f] = len;
    return 0;
}
const uint8_t *zitaotech_screen_image_data(uint32_t slot) {
    if (slot >= ZITAOTECH_SCREEN_IMAGE_SLOTS || headers[slot].magic != IMAGE_MAGIC) return NULL;
    k_mutex_lock(&image_cache_lock, K_FOREVER);
    if (upload_owner) { k_mutex_unlock(&image_cache_lock); return NULL; }
    if (cached_slot != slot) {
        if (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) {
            k_mutex_unlock(&image_cache_lock); return NULL;
        }
        char subtree[40]; settings_name(slot, "", subtree, sizeof(subtree));
        subtree[strlen(subtree)-1] = 0;
        struct cache_load load = {0};
        cached_slot = -1;
        int err = settings_load_subtree_direct(subtree, cache_read, &load);
        size_t size = headers[slot].size;
        for (unsigned f=0; f<FRAGMENT_COUNT; f++) {
            size_t expected = f*FRAGMENT_BYTES < size ? MIN(FRAGMENT_BYTES,size-f*FRAGMENT_BYTES) : 0;
            if (load.lengths[f] != expected) load.failed = true;
        }
        if (err || load.failed || crc32_ieee(image_cache,size) != headers[slot].crc32) {
            k_mutex_unlock(&image_cache_lock); return NULL;
        }
        cached_slot = slot;
    }
    k_mutex_unlock(&image_cache_lock);
    return image_cache;
}

/* Pick the stored headers back up at boot. */
static int image_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, "last") == 0) {
        int32_t value;
        if (len != sizeof(value) || read_cb(cb_arg, &value, sizeof(value)) != sizeof(value) || value < 0 || value >= ZITAOTECH_SCREEN_IMAGE_SLOTS) return -EINVAL;
        last_slot = value;
        return 0;
    }
    const char *next;
    unsigned slot;

    char *end;
    slot = strtoul(name, &end, 10);
    if (end == name || *end != '/' || slot >= ZITAOTECH_SCREEN_IMAGE_SLOTS) return -ENOENT;
    next = end + 1;

    if (next[0] == 'f' && next[1] >= '0' && next[1] < '0' + FRAGMENT_COUNT && next[2] == '\0') {
        unsigned fragment = next[1] - '0';
        if (len > FRAGMENT_BYTES || len == 0) {
            return -EINVAL;
        }
        fragment_lengths[slot][fragment] = len;
        return 0;
    }
    if (strcmp(next, "meta") != 0 || len != sizeof(struct image_header)) return -ENOENT;

    struct image_header header;
    if (read_cb(cb_arg, &header, sizeof(header)) != sizeof(header) || header.magic != IMAGE_MAGIC ||
        (header.format != 0 && header.format != 1) ||
        header.size != (header.format == 1 ? 1080 : 4320)) {
        return -EINVAL;
    }

    headers[slot] = header;
    return 0;
}

static int image_settings_commit(void) {
    for (unsigned slot = 0; slot < ZITAOTECH_SCREEN_IMAGE_SLOTS; slot++) {
        size_t size = headers[slot].size;
        bool complete = headers[slot].magic == IMAGE_MAGIC;
        if (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) continue;
        for (unsigned f = 0; f < FRAGMENT_COUNT && f * FRAGMENT_BYTES < size; f++) {
            if (fragment_lengths[slot][f] != MIN(FRAGMENT_BYTES, size - f * FRAGMENT_BYTES))
                complete = false;
        }
        if (!complete)
            headers[slot] = (struct image_header){0};
    }
    return 0;
}

static struct settings_handler image_settings = {
    .name = "zitaotech_img",
    .h_set = image_settings_set,
    .h_commit = image_settings_commit,
};

static int screen_image_store_init(void) {
    settings_register(&image_settings);
    return 0;
}

/*
 * The settings subsystem registers itself at CONFIG_APPLICATION_INIT_PRIORITY,
 * so this has to run *after* it: the same priority would make the order depend
 * on link order, and registering a handler that early locks the keyboard up at
 * boot. 95 is the same value the bootmagic module uses for its early init.
 */
SYS_INIT(screen_image_store_init, APPLICATION, 95);

