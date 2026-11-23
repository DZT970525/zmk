/*
 * Copyright (c) 2026 ZitaoTech
 * SPDX-License-Identifier: MIT
 *
 * The user picture slots behind the carousel: pictures that came from the web
 * UI instead of being compiled in.  Each one is stored as a small header (CRC,
 * size, format) plus fixed size fragments, so a picture never depends on one
 * big flash write and can be identified by its CRC alone.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZITAOTECH_SCREEN_IMAGE_SLOTS 20

/** Largest picture we accept: 120x72 at 4bpp (RGB111). */
#define ZITAOTECH_SCREEN_IMAGE_MAX_BYTES 4320

/** 0 = 4bpp RGB111, 1 = 1bpp black and white. */
#define ZITAOTECH_SCREEN_IMAGE_FORMAT_RGB111 0
#define ZITAOTECH_SCREEN_IMAGE_FORMAT_MONO 1

struct zitaotech_screen_image_info {
    uint32_t slot;
    uint32_t crc32;
    uint32_t size;
    uint32_t format;
};

/** Store (or replace) a picture.  Returns 0 or a negative errno. */
int zitaotech_screen_image_write(uint32_t slot, uint32_t format, const uint8_t *data, size_t size);

int zitaotech_screen_image_erase(uint32_t slot);

bool zitaotech_screen_image_info(uint32_t slot, struct zitaotech_screen_image_info *out);

/** One reusable cache. Central data is available only for the latest upload. */
const uint8_t *zitaotech_screen_image_data(uint32_t slot);

int zitaotech_screen_image_last_slot(void);

/** Shared RPC/USB upload staging; caller must own the upload/transfer lease. */
uint8_t *zitaotech_screen_image_upload_buffer(uint8_t owner);
void zitaotech_screen_image_upload_end(uint8_t owner);
int zitaotech_screen_image_read_commit(uint32_t slot);
