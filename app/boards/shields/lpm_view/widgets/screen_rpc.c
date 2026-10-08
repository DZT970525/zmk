/*
 * Copyright (c) 2026 ZitaoTech
 * SPDX-License-Identifier: MIT
 *
 * ZitaoTech Studio's subsystem for the right half's picture carousel
 * (widgets/screen_config.c owns the values and the split mirroring).
 *
 * This lives in its own translation unit compiled by the app target: the
 * generated Studio protos are only on that target's include path, and only the
 * central half has a Studio RPC of its own.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC) &&                                                           \
    (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#include <pb_decode.h>
#include <pb_encode.h>
#include <string.h>
#include <zephyr/sys/crc.h>

#include <zmk/studio/custom.h>

#include <zitaotech/screen/screen.pb.h>

#include "screen_config.h"
#include "screen_image_store.h"
#include "screen_transfer.h"
#include <zmk/screen_usb.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta zitaotech_screen_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(zitaotech__screen, &zitaotech_screen_meta,
                         zitaotech_screen_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(zitaotech__screen, zitaotech_screen_Response);

static void fill_error(zitaotech_screen_Response *resp, const char *message);
static void fill_ok(zitaotech_screen_Response *resp);

static void fill_state(zitaotech_screen_State *out) {
    out->image_transfer_supported = true;
    out->image_management_supported = true;
    out->usb_image_upload_supported = true;
    out->playback_supported = true;
    out->interval_ms = zitaotech_screen_interval_ms();
    out->sequential = zitaotech_screen_sequential();
    out->playback_order.size=5; zitaotech_screen_playback_order(out->playback_order.bytes);
    out->right_interval_ms=zitaotech_screen_right_interval_ms();
    out->right_sequential=zitaotech_screen_right_sequential();
    out->right_playback_order.size=5; zitaotech_screen_right_playback_order(out->right_playback_order.bytes);
    out->interval_s = zitaotech_screen_interval_s();
    out->interval_min = ZITAOTECH_SCREEN_INTERVAL_MIN_S;
    out->interval_max = ZITAOTECH_SCREEN_INTERVAL_MAX_S;
    out->enabled_mask = zitaotech_screen_enabled_mask();
    out->picture_count = ZITAOTECH_SCREEN_PICTURES;
    out->right_connected = zitaotech_screen_right_connected();
    out->right_interval_s = zitaotech_screen_right_interval_s();
    out->right_applied_mask = zitaotech_screen_right_applied_mask();
}

/** The user pictures this half holds (the web UI caches the bitmaps locally). */
static void fill_image_list(zitaotech_screen_ImageListResponse *out) {
    out->images_count = 0;

    for (uint32_t slot = 0; slot < ZITAOTECH_SCREEN_IMAGE_SLOTS; slot++) {
        struct zitaotech_screen_image_info info;
        if (!zitaotech_screen_image_info(slot, &info)) {
            continue;
        }
        zitaotech_screen_ImageInfo *entry = &out->images[out->images_count++];
        *entry = (zitaotech_screen_ImageInfo)zitaotech_screen_ImageInfo_init_zero;
        entry->slot = info.slot;
        entry->crc32 = info.crc32;
        entry->size = info.size;
        entry->format = info.format;
    }
}

/*
 * A picture arrives in small chunks, so it is assembled here first and only
 * handed to the store when the last chunk lands.  One transfer at a time, which
 * is all the web UI ever does.
 */
static uint8_t image_staging[ZITAOTECH_SCREEN_IMAGE_MAX_BYTES];
static struct {
    uint32_t slot;
    uint32_t format;
    uint32_t total_size;
    size_t received;
    bool active;
} image_transfer;
bool zitaotech_screen_rpc_upload_active(void) { return image_transfer.active; }

static int handle_image_write(const zitaotech_screen_ImageWriteRequest *req,
                              zitaotech_screen_Response *resp) {
    if (zitaotech_screen_transfer_active() || zitaotech_usb_image_active()) { fill_error(resp, "Image transfer busy"); return 0; }
    if (req->total_size == 0 || req->total_size > ZITAOTECH_SCREEN_IMAGE_MAX_BYTES ||
        req->slot >= ZITAOTECH_SCREEN_IMAGE_SLOTS ||
        (req->format != ZITAOTECH_SCREEN_IMAGE_FORMAT_RGB111 &&
         req->format != ZITAOTECH_SCREEN_IMAGE_FORMAT_MONO)) {
        fill_error(resp, "Bad image header");
        return 0;
    }

    if (req->offset == 0) {
        image_transfer.slot = req->slot;
        image_transfer.format = req->format;
        image_transfer.total_size = req->total_size;
        image_transfer.received = 0;
        image_transfer.active = true;
    }

    if (!image_transfer.active || req->slot != image_transfer.slot ||
        req->offset != image_transfer.received ||
        req->offset + req->data.size > image_transfer.total_size) {
        fill_error(resp, "Chunk out of order");
        return 0;
    }

    memcpy(image_staging + req->offset, req->data.bytes, req->data.size);
    image_transfer.received += req->data.size;

    resp->which_response_type = zitaotech_screen_Response_image_write_tag;
    zitaotech_screen_ImageWriteResponse *out = &resp->response_type.image_write;
    out->slot = req->slot;

    if (image_transfer.received < image_transfer.total_size) {
        /* More to come: report progress only. */
        out->stored = false;
        out->crc32 = (uint32_t)image_transfer.received;
        return 0;
    }

    int ret = zitaotech_screen_image_write(req->slot, req->format, image_staging,
                                           image_transfer.total_size);
    image_transfer.active = false;
    if (ret < 0) {
        fill_error(resp, "Failed to store picture");
        return 0;
    }

    out = &resp->response_type.image_write;
    out->stored = true;
    out->crc32 = crc32_ieee(image_staging, image_transfer.total_size);
    return 0;
}

static void fill_error(zitaotech_screen_Response *resp, const char *message) {
    resp->which_response_type = zitaotech_screen_Response_error_tag;
    strncpy(resp->response_type.error.message, message,
            sizeof(resp->response_type.error.message) - 1);
}

static void fill_ok(zitaotech_screen_Response *resp) {
    resp->which_response_type = zitaotech_screen_Response_ok_tag;
}

static int screen_subsystem_index(void) {
    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);

    for (size_t i = 0; i < subsystem_count; i++) {
        struct zmk_rpc_custom_subsystem *subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &subsys);
        if (strcmp(subsys->identifier, "zitaotech__screen") == 0) {
            return (int)i;
        }
    }
    return -ENOENT;
}

static bool encode_notification_payload(pb_ostream_t *stream, const pb_field_t *field,
                                        void *const *arg) {
    const zitaotech_screen_Notification *notification =
        (const zitaotech_screen_Notification *)*arg;

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }
    return pb_encode_submessage(stream, zitaotech_screen_Notification_fields, notification);
}

static void notify_state_changed(void) {
    int subsystem_index = screen_subsystem_index();
    if (subsystem_index < 0) {
        return;
    }

    static zitaotech_screen_Notification notification;
    notification = (zitaotech_screen_Notification)zitaotech_screen_Notification_init_zero;
    fill_state(&notification.state);

    raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = subsystem_index,
        .encode_payload =
            {
                .funcs = {.encode = encode_notification_payload},
                .arg = &notification,
            },
    });
}

/* The module calls this after every change; without this file it is a no-op. */
void zitaotech_screen_state_changed(void) { notify_state_changed(); }

static bool zitaotech_screen_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                                pb_callback_t *encode_response) {
    zitaotech_screen_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(zitaotech__screen, encode_response);

    zitaotech_screen_Request req = zitaotech_screen_Request_init_zero;
    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, zitaotech_screen_Request_fields, &req)) {
        LOG_WRN("Failed to decode screen request: %s", PB_GET_ERROR(&req_stream));
        fill_error(resp, "Failed to decode request");
        return true;
    }

    switch (req.which_request_type) {
    case zitaotech_screen_Request_get_state_tag:
        /* Ask the right half for a fresh report; its answer arrives as a
         * notification a few milliseconds later, and the page's refresh picks
         * it up. */
        zitaotech_screen_refresh_right();
        break;
    case zitaotech_screen_Request_set_config_tag: {
        if (zitaotech_screen_transfer_active() || zitaotech_usb_image_active()) { fill_error(resp, "Image transfer busy"); return true; }
        const zitaotech_screen_SetConfigRequest *config = &req.request_type.set_config;

        if (config->has_interval_s && (config->interval_s < ZITAOTECH_SCREEN_INTERVAL_MIN_S ||
                                       config->interval_s > ZITAOTECH_SCREEN_INTERVAL_MAX_S)) {
            fill_error(resp, "Interval out of range");
            return true;
        }

        if (config->has_interval_ms || config->has_sequential || config->playback_order.size) {
            uint8_t order[5]; zitaotech_screen_playback_order(order);
            if(config->playback_order.size){if(config->playback_order.size!=5){fill_error(resp,"Invalid playback order");return true;} memcpy(order,config->playback_order.bytes,5);}
            if(zitaotech_screen_set_playback(config->has_interval_ms?config->interval_ms:zitaotech_screen_interval_ms(),config->has_sequential?config->sequential:zitaotech_screen_sequential(),order)){fill_error(resp,"Failed to save playback configuration");return true;}
        }
        if (config->has_interval_s) {
            if(zitaotech_screen_set_interval_s(config->interval_s)){fill_error(resp,"Failed to save interval");return true;}
        }
        if (config->has_enabled_mask) {
            if(zitaotech_screen_set_enabled_mask((uint16_t)config->enabled_mask)){fill_error(resp,"Failed to save enabled images");return true;}
        }
        fill_ok(resp);
        return true;
    }
    case zitaotech_screen_Request_reset_tag:
        if (zitaotech_screen_transfer_active() || zitaotech_usb_image_active()) { fill_error(resp, "Image transfer busy"); return true; }
        zitaotech_screen_reset();
        fill_ok(resp);
        return true;
    case zitaotech_screen_Request_image_sync_tag:
        if (image_transfer.active || zitaotech_usb_image_active() || zitaotech_screen_image_sync(req.request_type.image_sync.slot))
            fill_error(resp,"Image transfer unavailable or busy");
        else fill_ok(resp);
        return true;
    case zitaotech_screen_Request_get_usb_image_status_tag: {
        struct zitaotech_usb_image_status value;
        zitaotech_usb_image_status(&value);
        resp->which_response_type=zitaotech_screen_Response_usb_image_status_tag;
        zitaotech_screen_UsbImageStatus *out=&resp->response_type.usb_image_status;
        out->active=value.active; out->stored=value.stored; out->token=value.token;
        out->op=value.op; out->slot=value.slot; out->received=value.received;
        out->total=value.total; out->crc32=value.crc; out->error=value.error;
        return true;
    }
    case zitaotech_screen_Request_get_image_transfer_tag: {
        struct zitaotech_image_transfer_status value;
        zitaotech_screen_image_transfer_status(&value);
        resp->which_response_type=zitaotech_screen_Response_image_transfer_tag;
        zitaotech_screen_ImageTransferState *out=&resp->response_type.image_transfer;
        out->active=value.active; out->slot=value.slot; out->sent=value.sent;
        out->total=value.total; out->crc32=value.crc; out->error=value.error;
        return true;
    }
    case zitaotech_screen_Request_image_list_tag:
        resp->which_response_type = zitaotech_screen_Response_image_list_tag;
        fill_image_list(&resp->response_type.image_list);
        return true;
    case zitaotech_screen_Request_image_write_tag:
        return handle_image_write(&req.request_type.image_write, resp) == 0;
    case zitaotech_screen_Request_image_read_tag: {
        if (zitaotech_screen_transfer_active() || zitaotech_usb_image_active() || image_transfer.active) { fill_error(resp, "Image transfer busy"); return true; }
        const zitaotech_screen_ImageReadRequest *read = &req.request_type.image_read;
        struct zitaotech_screen_image_info info;
        if (!zitaotech_screen_image_info(read->slot, &info) || read->offset >= info.size) {
            fill_error(resp, "No such picture or offset"); return true;
        }
        resp->which_response_type = zitaotech_screen_Response_image_read_tag;
        zitaotech_screen_ImageReadResponse *out = &resp->response_type.image_read;
        out->slot = read->slot; out->offset = read->offset; out->crc32 = info.crc32;
        out->data.size = MIN(64, info.size - read->offset);
        memcpy(out->data.bytes, zitaotech_screen_image_data(read->slot) + read->offset, out->data.size);
        return true;
    }
    case zitaotech_screen_Request_image_erase_tag:
        if (req.request_type.image_erase.both_halves) {
            if (image_transfer.active || zitaotech_usb_image_active() || zitaotech_screen_image_delete(req.request_type.image_erase.slot))
                fill_error(resp, "Delete unavailable or busy; connect both halves");
            else fill_ok(resp);
            return true;
        }
        if (zitaotech_screen_transfer_active() || zitaotech_usb_image_active()) { fill_error(resp, "Image transfer busy"); return true; }
        if (zitaotech_screen_image_erase(req.request_type.image_erase.slot) < 0) {
            fill_error(resp, "No such picture slot");
            return true;
        }
        fill_ok(resp);
        return true;
    default:
        fill_error(resp, "Unknown request");
        return true;
    }

    resp->which_response_type = zitaotech_screen_Response_state_tag;
    fill_state(&resp->response_type.state);
    return true;
}

#endif /* CONFIG_ZMK_STUDIO_RPC && central */



