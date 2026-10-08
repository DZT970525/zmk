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
static uint8_t images[ZITAOTECH_SCREEN_IMAGE_SLOTS][ZITAOTECH_SCREEN_IMAGE_MAX_BYTES];
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

    for (size_t fragment = 0; fragment < FRAGMENT_COUNT; fragment++) {
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
    memcpy(images[slot], data, size);
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

const uint8_t *zitaotech_screen_image_data(uint32_t slot) {
    return slot < ZITAOTECH_SCREEN_IMAGE_SLOTS && headers[slot].magic == IMAGE_MAGIC
               ? images[slot] : NULL;
}

/* Pick the stored headers back up at boot. */
static int image_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, "last") == 0) {
        int32_t value;
        if (len != sizeof(value) || read_cb(cb_arg, &value, sizeof(value)) != sizeof(value) || value < 0 || value >= 2) return -EINVAL;
        last_slot = value;
        return 0;
    }
    const char *next;
    unsigned slot;

    /* name is "<slot>/meta" inside the zitaotech_img subtree. */
    if (!settings_name_steq(name, "0", &next) && !settings_name_steq(name, "1", &next)) {
        return -ENOENT;
    }
    slot = (unsigned)(name[0] - '0');
    /* settings_name_steq() hands back the remainder without the separator. */
    if (slot >= ZITAOTECH_SCREEN_IMAGE_SLOTS || next == NULL) {
        return -ENOENT;
    }

    if (next[0] == 'f' && next[1] >= '0' && next[1] < '0' + FRAGMENT_COUNT && next[2] == '\0') {
        unsigned fragment = next[1] - '0';
        if (len > FRAGMENT_BYTES || len == 0 || read_cb(cb_arg,
            images[slot] + fragment * FRAGMENT_BYTES, len) != len) {
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
        for (unsigned f = 0; f < FRAGMENT_COUNT && f * FRAGMENT_BYTES < size; f++) {
            if (fragment_lengths[slot][f] != MIN(FRAGMENT_BYTES, size - f * FRAGMENT_BYTES))
                complete = false;
        }
        if (!complete || crc32_ieee(images[slot], size) != headers[slot].crc32)
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

