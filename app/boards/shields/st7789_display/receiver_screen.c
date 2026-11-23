#include <zmk/keypoint_relay.h>
/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/atomic.h>
#include <zmk/studio/custom.h>
#include <pb_encode.h>
#include "receiver_screen.h"
#include <zmk/screen_usb.h>
#include <zmk/ble.h>
#include "jpeg/tjpgd.h"
#include "widgets/helpers/display.h"

/* Two independent 240x240 baseline JPEG pictures, decoded to full-color RGB565.
 * A/B banks and a last-written manifest preserve the previous image if
 * power fails during upload. No flash writes occur in the input path. */
#define IMAGE_BYTES 24576
#define FRAGMENT 1024
static uint8_t staged[IMAGE_BYTES];

static uint8_t headers[2][9]; /* bank + CRC32 + JPEG length */
static bool valid[2];
static uint32_t timeout_s = 600;
static uint8_t brightness = 10;
static atomic_t last_input, idle;
static bool started;
static atomic_t boot_complete;
static uint16_t received;
static uint8_t staged_slot = 255;
static uint32_t staged_crc;
static uint32_t staged_size;
static int decode_jpeg(unsigned slot, const uint8_t *memory, unsigned size, bool draw);
static int64_t deadline;
K_MUTEX_DEFINE(picture_lock);
static struct k_work_delayable idle_work;
static void reevaluate(void);
bool receiver_screen_is_idle(void) { return atomic_get(&idle); }
static void apply_idle(bool value) {
    /* Redraw only on a real idle transition; retain gradual backlight changes. */
    bool changed = atomic_set(&idle, value) != value;
    if (!changed) return;
    receiver_screen_set_idle(value);
    receiver_backlight_set_idle(value, brightness);
}
static void expiry(struct k_work *work) {
    if (!atomic_get(&boot_complete)) {
        k_work_reschedule(&idle_work, K_MSEC(100));
        return;
    }
    if(zmk_mac_pair_pending()){apply_idle(false);k_work_reschedule(&idle_work,K_SECONDS(1));return;}
    uint32_t elapsed = k_uptime_get_32() - (uint32_t)atomic_get(&last_input);
    if (!timeout_s) { apply_idle(false); return; }
    uint32_t delay = timeout_s * 1000;
    if (elapsed >= delay) apply_idle(true);
    else k_work_reschedule(&idle_work, K_MSEC(delay - elapsed));
}
void receiver_screen_boot_complete(void) {
    atomic_set(&boot_complete, 1);
    atomic_set(&last_input, k_uptime_get_32());
    reevaluate();
}
void receiver_screen_note_input(void) {
    atomic_set(&last_input, k_uptime_get_32());
    if (started && atomic_get(&idle)) {
        apply_idle(false);
        if (timeout_s) k_work_reschedule(&idle_work, K_SECONDS(timeout_s));
    }
}
static void reevaluate(void) {
    if (started) k_work_reschedule(&idle_work, K_NO_WAIT);
}
static int setting(const char *name, size_t len, settings_read_cb read, void *arg) {
    if (!strcmp(name, "config")) {
        uint8_t data[6];
        if (len != 6 || read(arg, data, 6) != 6 || data[0] != 1 ||
            sys_get_le32(data + 1) > 86400 || data[5] > 100) return -EINVAL;
        timeout_s = sys_get_le32(data + 1); brightness = data[5]; return 0;
    }
    unsigned slot, bank, index; char tail;
    if (sscanf(name, "%u/meta%c", &slot, &tail) == 1 && slot < 2 && strstr(name, "/meta")) {
        if (len != 9 || read(arg, headers[slot], 9) != 9 || headers[slot][0] > 1) return -EINVAL;
        return 0;
    }
    if (sscanf(name, "%u/%u/f%u%c", &slot, &bank, &index, &tail) == 3 && slot < 2 && bank < 2 && index < 24) {

        return 0; /* Loaded in commit after all manifests have been read. */
    }
    return -ENOENT;
}

struct read_context { unsigned slot, bank, offset, count; uint8_t *out; };
static int load_fragment(const char *name, size_t len, settings_read_cb read, void *arg, void *param) {
    struct read_context *ctx = param;
    unsigned slot, bank, index; char tail;
    if (sscanf(name, "%u/%u/f%u%c", &slot, &bank, &index, &tail) != 3 ||
        slot != ctx->slot || bank != ctx->bank || index >= 24) return 0;
    unsigned off = index * FRAGMENT, size = sys_get_le32(headers[slot] + 5);
    if (off >= size || size > IMAGE_BYTES) return 0;
    unsigned n = MIN(FRAGMENT, size - off);
    unsigned begin = MAX(off, ctx->offset), end = MIN(off + n, ctx->offset + ctx->count);
    if (len != n || begin >= end) return 0;
    uint8_t fragment[FRAGMENT];
    if (read(arg, fragment, n) != n) return -EIO;
    if (ctx->out) memcpy(ctx->out + begin - ctx->offset, fragment + begin - off, end - begin);

    return 0;
}
static int read_picture(unsigned slot, unsigned offset, uint8_t *out, unsigned count) {
    struct read_context ctx = {.slot = slot, .bank = headers[slot][0], .offset = offset, .count = count, .out = out};
    if (out) memset(out, 0, count);
    return settings_load_subtree_direct("receiver_screen", load_fragment, &ctx);
}
static int commit_settings(void) {
    /* Reuse the upload buffer at boot; no three full-screen permanent buffers. */
    for (int slot = 0; slot < 2; slot++) {
        unsigned size = sys_get_le32(headers[slot] + 5);
        int ret = size > 0 && size <= IMAGE_BYTES ? read_picture(slot, 0, staged, size) : -EINVAL;
        valid[slot] = !ret && sys_get_le32(headers[slot] + 1) == crc32_ieee(staged, size);
    }
    reevaluate(); return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(receiver_screen, "receiver_screen", NULL, setting, commit_settings, NULL);
static void clear_image_bank(unsigned slot, unsigned bank) {
    char key[48];
    for (unsigned index = 0; index < IMAGE_BYTES / FRAGMENT; index++) {
        snprintf(key, sizeof(key), "receiver_screen/%u/%u/f%u", slot, bank, index);
        settings_delete(key);
    }
}
static int commit_upload(unsigned slot) {
    int ret=0;
            if (staged_slot != slot || received != staged_size || k_uptime_get() > deadline || crc32_ieee(staged, staged_size) != staged_crc || decode_jpeg(slot, staged, staged_size, false)) ret = -EINVAL;
            else {
                char key[48]; unsigned bank = 1 - headers[slot][0];
                /* Discard only the unpublished bank; the current image remains intact. */
                clear_image_bank(slot, bank);
                for (unsigned off = 0; off < staged_size && !ret; off += FRAGMENT) {
                    snprintf(key, sizeof(key), "receiver_screen/%u/%u/f%u", slot, bank, off / FRAGMENT);
                    ret = settings_save_one(key, staged + off, MIN(FRAGMENT, staged_size - off));
                }
                uint8_t meta[9] = {bank}; sys_put_le32(staged_crc, meta + 1); sys_put_le32(staged_size, meta + 5);
                snprintf(key, sizeof(key), "receiver_screen/%u/meta", slot);
                if (!ret) ret = settings_save_one(key, meta, sizeof(meta));
                if (!ret) {
                    k_mutex_lock(&picture_lock, K_FOREVER);
                    memcpy(headers[slot], meta, 9); valid[slot] = true;
                    k_mutex_unlock(&picture_lock);
                    /* Once the manifest is durable, reclaim the previous image bank. */
                    clear_image_bank(slot, 1 - bank);
                    receiver_screen_set_idle(receiver_screen_is_idle());
                }
            }
            staged_slot = 255;
    return ret;
}

K_MUTEX_DEFINE(upload_lock);
static bool fast_upload;
uint8_t *receiver_screen_usb_begin(unsigned slot, unsigned total, uint32_t crc) {
    k_mutex_lock(&upload_lock, K_FOREVER);
    uint8_t *result=NULL;
    if (slot<2 && total>0 && total<=IMAGE_BYTES && !fast_upload &&
        (staged_slot==255 || k_uptime_get()>deadline)) {
        fast_upload=true; staged_slot=slot; staged_size=total;
        staged_crc=crc; received=0; deadline=k_uptime_get()+30000; result=staged;
    }
    k_mutex_unlock(&upload_lock); return result;
}
int receiver_screen_usb_commit(unsigned slot, unsigned count) {
    k_mutex_lock(&upload_lock, K_FOREVER);
    int ret=-EINVAL;
    if (fast_upload && staged_slot==slot) { received=count; deadline=k_uptime_get()+30000; ret=commit_upload(slot); }
    fast_upload=false; staged_slot=255;
    k_mutex_unlock(&upload_lock); return ret;
}
void receiver_screen_usb_abort(void) {
    k_mutex_lock(&upload_lock, K_FOREVER);
    if (fast_upload) { fast_upload=false; staged_slot=255; }
    k_mutex_unlock(&upload_lock);
}
static uint8_t response[88];
static size_t response_size;
static uint32_t config_save_ms,config_save_count;
static bool encode(pb_ostream_t *s, const pb_field_t *f, void *const *arg) {
    return pb_encode_tag_for_field(s, f) && pb_encode_string(s, response, response_size);
}
static bool rpc(const zmk_custom_CallRequest *request, pb_callback_t *output) {
    const uint8_t *p = request->payload.bytes;
    size_t n = request->payload.size;
    int ret = 0;
    response_size = 24; memset(response, 0, sizeof(response)); response[0] = 2;
    /* Status must stay readable while the worker validates JPEG and writes NVS. */
    if (n == 2 && p[0] == 2 && p[1] == 8) {
        struct zitaotech_usb_image_status usb; zitaotech_usb_image_status(&usb);
        response[5]=usb.token; response[6]=usb.slot; response[7]=usb.op;
        response[8]=usb.active; response[9]=usb.stored;
        sys_put_le16(usb.received,response+10); sys_put_le16(usb.total,response+12);
        sys_put_le32(usb.crc,response+14); sys_put_le32(usb.error,response+18);
        output->funcs.encode=encode; return true;
    }
    if (n==2 && p[0]==2 && p[1]==11) {
        zmk_receiver_relay_diagnostics(response+5);response[29]=receiver_screen_is_idle();
        sys_put_le32(config_save_ms,response+30);sys_put_le32(config_save_count,response+34);
        response_size=38;output->funcs.encode=encode;return true;
    }
    if(n==2 && p[0]==2 && (p[1]==12 || p[1]==13)) {
        ret=p[1]==12?zmk_receiver_request_source_diagnostics():zmk_receiver_source_diagnostics(response+5);
        sys_put_le32(ret,response+1);response_size=p[1]==13?28:5;output->funcs.encode=encode;return true;
    }
    k_mutex_lock(&upload_lock, K_FOREVER);
    if (n < 2 || p[0] != 2) ret = -EINVAL;
    else if (fast_upload && p[1]>=2 && p[1]<=6) ret=-EBUSY;
    else if (p[1] == 0 && n == 2) { /* read */ }
    else if (p[1] == 9 && n == 2) { /* read host BLE capability */ }
    else if (p[1] == 10 && n == 3 && p[2] <= 1) { ret = zmk_ble_host_set_enabled(p[2]); }
    else if (p[1] == 1 && n == 7) {
        uint32_t seconds = sys_get_le32(p + 2);
        if (seconds > 86400 || p[6] > 100) ret = -EINVAL;
        else {
            uint8_t config[6] = {1}; sys_put_le32(seconds, config + 1); config[5] = p[6];
            uint32_t save_start=k_uptime_get_32();
            ret = settings_save_one("receiver_screen/config", config, 6);
            config_save_ms=k_uptime_get_32()-save_start;config_save_count++;
            if (!ret) { timeout_s = seconds; brightness = p[6];
                receiver_backlight_set_idle(receiver_screen_is_idle(), brightness); reevaluate(); }
        }
    } else if (p[1] == 7 && n == 3) {
        /* Volatile preview: dragging must not repeatedly write flash. */
        if (p[2] > 100) ret = -EINVAL;
        else if (receiver_screen_is_idle()) receiver_backlight_preview(p[2]);
    } else if (n >= 3 && p[2] < 2) {
        unsigned slot = p[2];
        if (p[1] == 2 && n == 9) { /* begin */
            staged_size = sys_get_le16(p + 7);
            if (!staged_size || staged_size > IMAGE_BYTES) ret = -EINVAL;
            else { staged_slot = slot; received = 0; staged_crc = sys_get_le32(p + 3); }
            deadline = k_uptime_get() + 30000;
        } else if (p[1] == 3 && n > 5 && n <= 30) {
            unsigned offset = sys_get_le16(p + 3);
            if (staged_slot != slot || k_uptime_get() > deadline || offset != received || offset + n - 5 > staged_size) ret = -EINVAL;
            else { memcpy(staged + offset, p + 5, n - 5); received += n - 5; deadline = k_uptime_get() + 30000; }
        } else if (p[1] == 4 && n == 3) { /* durable commit */
            ret = commit_upload(slot);
        } else if (p[1] == 5 && n == 3) {
            char key[40]; snprintf(key, sizeof(key), "receiver_screen/%u/meta", slot);
            uint8_t meta[9] = {0}; ret = settings_save_one(key, meta, 9);
            if (!ret) { k_mutex_lock(&picture_lock, K_FOREVER); valid[slot] = false; memset(headers[slot], 0, 9); k_mutex_unlock(&picture_lock); clear_image_bank(slot, 0); clear_image_bank(slot, 1); receiver_screen_set_idle(receiver_screen_is_idle()); }
        } else if (p[1] == 6 && n == 5 && valid[slot]) {
            unsigned off = sys_get_le16(p + 3);
            if (off >= sys_get_le32(headers[slot] + 5)) ret = -EINVAL;
            else { unsigned count = MIN(64, sys_get_le32(headers[slot] + 5) - off); ret = read_picture(slot, off, response + 24, count); response_size += count; }
        } else ret = -EINVAL;
    } else ret = -EINVAL;
    sys_put_le32(ret, response + 1); sys_put_le32(timeout_s, response + 5);
    response[9] = brightness; response[10] = receiver_screen_is_idle(); response[11] = valid[0] | (valid[1] << 1);
    if (n >= 3 && p[1] == 6 && p[2] < 2) memcpy(response + 12, headers[p[2]] + 1, 4);
    else sys_put_le16(received, response + 12);
    sys_put_le32(sys_get_le32(headers[0] + 5), response + 16);
    sys_put_le32(sys_get_le32(headers[1] + 5), response + 20);
    if (n == 2 && p[1] == 0) response[14]=1; /* USB image fast channel supported. */
    response[15] = 0x80 | zmk_ble_host_is_enabled(); /* Host BLE toggle capability + state. */
    output->funcs.encode = encode; k_mutex_unlock(&upload_lock); return true;
}
static struct zmk_rpc_custom_subsystem_meta meta = {.security = ZMK_STUDIO_RPC_HANDLER_UNSECURED};
ZMK_RPC_CUSTOM_SUBSYSTEM(zitaotech_receiver_screen, &meta, rpc);
/* Coalesce MCU tiles into full-width bands instead of hundreds of SPI writes. */
struct jpeg_session {
    unsigned slot, offset, size, cached;
    const uint8_t *memory;
    bool draw;
    uint16_t *band;
    uint8_t cache[FRAGMENT];
    uint8_t work[4096] __aligned(4);
};
static size_t jpeg_input(JDEC *decoder, uint8_t *out, size_t count) {
    struct jpeg_session *session = decoder->device;
    count = MIN(count, session->size - session->offset);
    size_t total = count;
    if (!out) { session->offset += count; return count; }
    while (count) {
        unsigned base = session->offset / FRAGMENT * FRAGMENT;
        unsigned at = session->offset - base, chunk = MIN(count, FRAGMENT - at);
        if (session->memory) memcpy(out, session->memory + session->offset, chunk);
        else {
            if (session->cached != base) {
                if (read_picture(session->slot, base, session->cache, MIN(FRAGMENT, session->size - base))) return 0;
                session->cached = base;
            }
            memcpy(out, session->cache + at, chunk);
        }
        session->offset += chunk; out += chunk; count -= chunk;
    }
    return total;
}
static int jpeg_output(JDEC *decoder, void *pixels, JRECT *rect) {
    struct jpeg_session *session = decoder->device;
    if (!session->draw) return 1;
    unsigned width = rect->right - rect->left + 1, height = rect->bottom - rect->top + 1;
    if (rect->right >= 240 || rect->bottom >= 240) return 0;
    if (height > 16) return 0;
    uint16_t *rgb = pixels;
    for (unsigned row = 0; row < height; row++) {
        for (unsigned col = 0; col < width; col++) {
            uint16_t value = rgb[row * width + col];
            session->band[row * 240 + rect->left + col] = (value << 8) | (value >> 8);
        }
    }
    if (rect->right != 239) return 1;
    struct display_buffer_descriptor desc = {.buf_size = 240 * height * 2, .width = 240, .height = height, .pitch = 240};
    return display_write(DEVICE_DT_GET(DT_CHOSEN(zephyr_display)), 0, rect->top, &desc, session->band) == 0;
}
static int decode_jpeg(unsigned slot, const uint8_t *memory, unsigned size, bool draw) {
    struct jpeg_session *session = k_malloc(sizeof(*session));
    if (!session) return -ENOMEM;
    *session = (struct jpeg_session){.slot = slot, .size = size, .memory = memory, .draw = draw, .cached = UINT32_MAX};
    if (draw) {
        session->band = k_malloc(240 * 16 * sizeof(uint16_t));
        if (!session->band) { k_free(session); return -ENOMEM; }
    }
    JDEC decoder;
    JRESULT result = jd_prepare(&decoder, jpeg_input, session->work, sizeof(session->work), session);
    if (result == JDR_OK && (decoder.width != 240 || decoder.height != 240)) result = JDR_PAR;
    if (result == JDR_OK) result = jd_decomp(&decoder, jpeg_output, 0);
    k_free(session->band); k_free(session);
    return result == JDR_OK ? 0 : -EBADMSG;
}
bool receiver_screen_draw_picture(void) {
    unsigned slot = receiver_screen_is_idle() ? 1 : 0;
    k_mutex_lock(&picture_lock, K_FOREVER);
    if (!valid[slot]) { k_mutex_unlock(&picture_lock); return false; }
    unsigned size = sys_get_le32(headers[slot] + 5);
    /* Scan settings once, rather than once for each 1 KiB decoder cache refill. */
    uint8_t *jpeg = k_malloc(size);
    int ret = -ENOMEM;
    if (jpeg) {
        ret = read_picture(slot, 0, jpeg, size);
        if (!ret) ret = decode_jpeg(slot, jpeg, size, true);
        k_free(jpeg);
    }
    k_mutex_unlock(&picture_lock); return ret == 0;
}
static int init(void) {
    k_work_init_delayable(&idle_work, expiry); atomic_set(&last_input, k_uptime_get_32()); started = true;
    reevaluate(); return 0;
}
SYS_INIT(init, APPLICATION, 91);
