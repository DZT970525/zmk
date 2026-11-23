/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zmk/keymap.h>
#include <zmk/keypoint_quota.h>
#include <zmk/ble.h>
#include <zmk/hog.h>
#include <zmk/split/central.h>
#include <zmk/endpoints.h>
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
#include <zmk/keypoint_relay.h>
#else
static inline bool zmk_keypoint_relay_routed(void) { return false; }
#endif
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include "codex_quota.h"
#include "area_pictures.h"
#include "util.h"

/* Config: WPM by default; the reserved physical layer-1 C shortcut is opt-in.
 * Telemetry stays in RAM. No tokens or account identifiers reach the keyboard. */
K_MUTEX_DEFINE(quota_lock);
static uint8_t config[3] = {0, 0, 15};
static uint8_t percentages[2] = {255, 255};
static uint32_t remaining[2], revision;
static uint8_t pc_percentages[2]={255,255};
static int64_t pc_received_at;
static bool pc_received;
static int64_t received_at;
static bool received, shortcut_held;

static int save_config(void) {
    uint8_t value[3] = {0, 0, 15};
    k_mutex_lock(&quota_lock, K_FOREVER);
    memcpy(value, config, sizeof(config));
    k_mutex_unlock(&quota_lock);
    int ret = settings_save_one("keypoint_quota/config", value, sizeof(value));
    return ret;
}
static void save_work_cb(struct k_work *work) { ARG_UNUSED(work); save_config(); }
K_WORK_DEFINE(quota_save_work, save_work_cb);

bool zmk_keypoint_quota_cycle(void) {
    k_mutex_lock(&quota_lock,K_FOREVER);
    bool enabled=config[1];
    if(enabled){do{config[0]=(config[0]+1)%4;}while(!(config[2]&(1U<<config[0])));revision++;}
    k_mutex_unlock(&quota_lock);
    if(enabled)k_work_submit(&quota_save_work);
    return enabled;
}

bool zmk_keypoint_quota_key(uint32_t position, bool pressed) {
    /* Physical C is index 30 in KEYPOINT's factory layout, independent of
     * Studio remapping. Consume its release even if the layer/setting changes. */
    if (position != (IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE_LEFT) ? 43 : IS_ENABLED(CONFIG_BOARD_ZITAOTECH_SOFLE_LEFT) ? 43 : (IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) ? 29 : 30))) return false;
    if (!pressed) {
        bool handled = shortcut_held;
        shortcut_held = false;
        return handled;
    }
    if (shortcut_held) return true;
    k_mutex_lock(&quota_lock, K_FOREVER);
    bool handled = config[1] && zmk_keymap_highest_layer_active() == 1;
    if (handled) { do { config[0] = (config[0]+1)%4; } while (!(config[2] & (1U << config[0]))); revision++; shortcut_held = true; }
    k_mutex_unlock(&quota_lock);
    if (handled) k_work_submit(&quota_save_work);
    return handled;
}

static int settings_set(const char *name, size_t len, settings_read_cb read, void *arg) {
    if (strcmp(name, "config")) return -ENOENT;
    uint8_t value[3] = {0, 0, 15};
    if (len != 2 && len != sizeof(value)) return -EINVAL;
    int ret = read(arg, value, len);
    if (ret < 0) return ret;
    if (ret != len || value[0] > 3 || value[1] > 1 || !value[2] || value[2]>15) return -EINVAL;
    if (!(value[2] & (1U << value[0]))) for (value[0]=0; !(value[2] & (1U << value[0])); value[0]++) {}
    memcpy(config, value, sizeof(value));
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(keypoint_quota, "keypoint_quota", NULL, settings_set, NULL, NULL);

/* CQ1 telemetry: [version=1, 5h remaining%, 7d remaining%, reserved=0,
 *                5h seconds until reset LE32, 7d seconds until reset LE32].
 * 255 means unknown; percentages are remaining, never used. */
static int accept_data(const uint8_t *bytes, size_t len) {
    if (len != 12 || (bytes[0] != 1 && bytes[0] != 2) || bytes[3] ||
        (bytes[1] > 100 && bytes[1] != 255) ||
        (bytes[2] > 100 && bytes[2] != 255) ||
        sys_get_le32(bytes+4) > 604800 || sys_get_le32(bytes+8) > 604800) return -EINVAL;
    k_mutex_lock(&quota_lock, K_FOREVER);
    if (bytes[0]==2) {
        if (sys_get_le32(bytes+4) || sys_get_le32(bytes+8)) { k_mutex_unlock(&quota_lock); return -EINVAL; }
        pc_percentages[0]=bytes[1]; pc_percentages[1]=bytes[2];
        pc_received_at=k_uptime_get(); pc_received=true; revision++;
        k_mutex_unlock(&quota_lock); return 0;
    }
    percentages[0] = bytes[1]; percentages[1] = bytes[2];
    remaining[0] = sys_get_le32(bytes+4); remaining[1] = sys_get_le32(bytes+8);
    received_at = k_uptime_get(); received = true; revision++;
    k_mutex_unlock(&quota_lock);
    return 0;
}
static ssize_t quota_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(attr);
    /* An encrypted GATT connection may outlive the selected HID profile. */
    k_mutex_lock(&quota_lock,K_FOREVER);
    struct bt_conn *active=zmk_ble_active_profile_conn();
    bool allowed=!zmk_keypoint_relay_routed() && active && active==conn &&
        zmk_endpoint_get_selected().transport==ZMK_TRANSPORT_BLE;
    if(active)bt_conn_unref(active);
    ssize_t result;
    if(!allowed)result=BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    else if (offset || (flags & BT_GATT_WRITE_FLAG_PREPARE)) result=BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    else result=accept_data(buf,len) ? BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED) : len;
    k_mutex_unlock(&quota_lock);
    return result;
}
void zmk_keypoint_quota_clear(void) {
    k_mutex_lock(&quota_lock,K_FOREVER);
    received=false;pc_received=false;revision++;
    k_mutex_unlock(&quota_lock);
}
int zmk_keypoint_quota_relay_receive(const uint8_t *bytes,size_t length) {
    if(!zmk_keypoint_relay_routed())return -EACCES;
    return accept_data(bytes,length);
}
static int telemetry_host_changed(const zmk_event_t *event) {
    if(zmk_keypoint_relay_routed() && as_zmk_ble_active_profile_changed(event))
        return ZMK_EV_EVENT_BUBBLE;
    zmk_keypoint_quota_clear();
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(keypoint_telemetry_host,telemetry_host_changed);
ZMK_SUBSCRIPTION(keypoint_telemetry_host,zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(keypoint_telemetry_host,zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
ZMK_SUBSCRIPTION(keypoint_telemetry_host,zmk_keypoint_relay_changed);
#endif
static ssize_t quota_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset) {
    struct bt_conn *active=zmk_ble_active_profile_conn();
    bool allowed=!zmk_keypoint_relay_routed() && active && active==conn &&
        zmk_endpoint_get_selected().transport==ZMK_TRANSPORT_BLE;
    if(active)bt_conn_unref(active);
    if(!allowed)return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    struct bt_conn_info info;
    if(bt_conn_get_info(conn,&info))return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    uint8_t data[32]={4,zmk_ble_active_profile_index(),bt_conn_get_security(conn),info.state};
    sys_put_le16(info.le.interval,data+4);
    sys_put_le16(info.le.latency,data+6);
    sys_put_le16(info.le.timeout,data+8);
    data[10]=zmk_ble_last_host_disconnect_reason();
    data[11]=zmk_ble_host_param_request_count();
    zmk_hog_diagnostics(data+12);
    zmk_split_central_link_diagnostics(data+20);
    return bt_gatt_attr_read(conn,attr,buf,len,offset,data,sizeof(data));
}
BT_GATT_SERVICE_DEFINE(keypoint_quota_service,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0xf5d41000,0x6d2f,0x4f4b,0x9b2a,0x2f4a8e8c0001))),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0xf5d41001,0x6d2f,0x4f4b,0x9b2a,0x2f4a8e8c0001)),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT, quota_read, quota_write, NULL));

uint32_t keypoint_quota_revision(void) {
    k_mutex_lock(&quota_lock, K_FOREVER);
    /* Countdown redraw once a minute; also expire stale telemetry by 6 min. */
    uint32_t result = revision * 100003U;
    uint8_t mode=config[0];
    k_mutex_unlock(&quota_lock);
    if (mode==3) result += keypoint_area_picture_revision();
    else if (mode) result += k_uptime_get() / 60000;
    return result;
}

/* 3x5 glyphs, four-pixel pitch: 4h32min fits the narrow memory LCD.
 * Drawing uses the same LVGL canvas and rotation as WPM. */
static const char glyph_chars[] = "0123456789codexhminHD%?-COEXPURAM";
static const uint16_t glyphs[] = {
    0x7b6f,0x2c97,0x73e7,0x73cf,0x5bc9,0x79cf,0x79ef,0x7249,0x7bef,0x7bcf,0xf27,0xf6f,0x13ef,0xfe7,0xa95,0x49ad,0xfed,0x2092,0xd6d,0x5bed,0x6b6e,0x52a5,0x6282,0x1c0,0x7927,0x7b6f,0x79e7,0x5aad,0x6ba4,0x5b6f,0x6bad,0x2bed,0x5fed
};
static void rect(lv_obj_t *canvas, int x, int y, int w, int h, lv_color_t color) {
    /* Write existing RGB565 canvas directly: pixel glyphs must not allocate
     * hundreds of LVGL draw tasks on the memory-constrained left half. */
    lv_draw_buf_t *buf=lv_canvas_get_draw_buf(canvas);
    uint16_t *pixels=(uint16_t *)buf->data;
    unsigned stride=buf->header.stride/sizeof(uint16_t);
    uint16_t value=lv_color_to_u16(color);
    for (int row=y; row<y+h; row++)
        for (int col=x; col<x+w; col++)
            if (col >= 0 && col < CANVAS_SIZE && row >= 0 && row < STATUS_CANVAS_SIZE)
                pixels[row*stride+col]=value;
}
static void text(lv_obj_t *canvas, int x, int y, const char *s, lv_color_t color) {
    for (; *s; s++,x+=7) {
        const char *entry = strchr(glyph_chars,*s);
        if (!entry) continue;
        uint16_t bits= glyphs[entry-glyph_chars];
        for (int row=0; row<5; row++) for (int col=0; col<3; col++)
            if (bits & (1U << (14-row*3-col))) rect(canvas,x+col*2,y+row*2,2,2,color);
    }
}
/* Compact header: 6px-wide capitals, 5px high; body remains 6x10. */
static void title_text(lv_obj_t *canvas, int x, int y, const char *s, lv_color_t color) {
    for (; *s; s++,x+=7) {
        const char *entry=strchr(glyph_chars,*s);
        if (!entry) continue;
        uint16_t bits=glyphs[entry-glyph_chars];
        for (int row=0;row<5;row++) for(int col=0;col<3;col++)
            if(bits & (1U << (14-row*3-col))) rect(canvas,x+col*2,y+row,2,1,color);
    }
}
static uint32_t quota_days(uint32_t seconds) {
    /* Nearest day, with half days rounded up: 6.5 days displays 7d. */
    return (seconds + 43200U) / 86400U;
}
bool keypoint_quota_draw(lv_obj_t *canvas) {
    uint8_t pct[2]; uint32_t secs[2];
    k_mutex_lock(&quota_lock,K_FOREVER);
    if (!config[0]) { k_mutex_unlock(&quota_lock); return false; }
    if(config[0]==3) { k_mutex_unlock(&quota_lock); keypoint_area_picture_draw(canvas); return true; }
    bool pc=config[0]==2;
    int64_t pc_age=(k_uptime_get()-pc_received_at)/1000;
    int64_t age = received ? (k_uptime_get()-received_at)/1000 : 0;
    bool valid = received && age < 360;
    for (int i=0;i<2;i++) { pct[i]=valid?percentages[i]:255; secs[i]=remaining[i]>age?remaining[i]-age:0; }
    if(pc) for(int i=0;i<2;i++) pct[i]=pc_received && pc_age<15 ? pc_percentages[i] : 255;
    k_mutex_unlock(&quota_lock);
    lv_color_t white=lv_color_hex(0xffffff), black=lv_color_hex(0), cyan=lv_color_hex(0x00ffff);
    /* Keep the final canvas row/column clear before the -90 degree bake. */
    /* 7D bars end at y67; keep two empty rows before the bottom border. */
    rect(canvas,2,18,69,53,white); rect(canvas,3,19,67,51,black);
    rect(canvas,3,19,67,8,cyan); title_text(canvas,pc?30:20,20,pc?"PC":"CODEX",black);
    for (int i=0;i<2;i++) {
        int y=30+i*22; char time[16];
        text(canvas,5,y,pc?(i?"RAM":"CPU"):(i?"7D":"5H"),white);
        if (pct[i]==255) strcpy(time,"--");
        else if(pc) snprintf(time,sizeof(time),"%u%%",pct[i]);
        else if (i) snprintf(time,sizeof(time),"%ud",(unsigned)quota_days(secs[i]));
        else snprintf(time,sizeof(time),"%uh%um",(unsigned)(secs[i]/3600),(unsigned)((secs[i]%3600)/60));
        text(canvas,70-7*strlen(time),y,time,white);
        lv_color_t color=pct[i]==255?white:lv_color_hex(pc?(pct[i]>70?0xff0000:pct[i]>30?0xffff00:0x00ff00):(pct[i]<=30?0xff0000:pct[i]<=70?0xffff00:0x00ff00));
        for (int j=0;j<10;j++) {
            rect(canvas,7+j*6,y+11,5,5,color);
            if (pct[i]==255 || j >= (pct[i]+9)/10) rect(canvas,8+j*6,y+12,3,3,black);
        }

    }
    lv_obj_invalidate(canvas);
    return true;
}

#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
#include <zmk/studio/custom.h>
#include <pb_encode.h>
static uint8_t reply[9];
static bool encode_reply(pb_ostream_t *stream,const pb_field_t *field,void *const *arg) {
    ARG_UNUSED(arg); return pb_encode_tag_for_field(stream,field) && pb_encode_string(stream,reply,sizeof(reply));
}
static bool quota_rpc(const zmk_custom_CallRequest *request,pb_callback_t *output) {
    const uint8_t *bytes=request->payload.bytes; size_t len=request->payload.size;
    int ret=-EINVAL;
    if (len==2 && bytes[0]==1 && bytes[1]==0) ret=0;
    else if (len==4 && bytes[0]==1 && bytes[1]==1 && bytes[2]<=3 && bytes[3]<=1 && (config[2] & (1U << bytes[2]))) {
        uint8_t old[3]; k_mutex_lock(&quota_lock,K_FOREVER); memcpy(old,config,sizeof(config));
        config[0]=bytes[2]; config[1]=bytes[3];
        k_mutex_unlock(&quota_lock);
        ret=save_config();
        k_mutex_lock(&quota_lock,K_FOREVER);
        if (ret) memcpy(config,old,sizeof(config)); else revision++;
        k_mutex_unlock(&quota_lock);
    } else if (len==3 && bytes[0]==1 && bytes[1]==3 && bytes[2] && bytes[2]<=15) {
        uint8_t old[3]; k_mutex_lock(&quota_lock,K_FOREVER); memcpy(old,config,sizeof(config));
        config[2]=bytes[2];
        if (!(config[2] & (1U << config[0]))) for (config[0]=0; !(config[2] & (1U << config[0])); config[0]++) {}
        k_mutex_unlock(&quota_lock);
        ret=save_config();
        k_mutex_lock(&quota_lock,K_FOREVER);
        if (ret) memcpy(config,old,sizeof(config)); else revision++;
        k_mutex_unlock(&quota_lock);
    } else if (len==14 && bytes[0]==1 && bytes[1]==2)
        ret=!zmk_keypoint_relay_routed() && zmk_endpoint_get_selected().transport==ZMK_TRANSPORT_USB ? accept_data(bytes+2,12) : -EACCES;
    reply[0]=1; sys_put_le32(ret,reply+1);
    k_mutex_lock(&quota_lock,K_FOREVER); reply[5]=config[0]; reply[6]=config[1]; reply[8]=config[2]; k_mutex_unlock(&quota_lock);
    reply[7]=1; output->funcs.encode=encode_reply; output->arg=NULL; return true;
}
static struct zmk_rpc_custom_subsystem_meta quota_meta={.security=ZMK_STUDIO_RPC_HANDLER_SECURED};
ZMK_RPC_CUSTOM_SUBSYSTEM(zitaotech_keypoint_quota,&quota_meta,quota_rpc);
/* Write-only volatile telemetry is independent of the Studio keymap lock. */
static bool telemetry_rpc(const zmk_custom_CallRequest *request,pb_callback_t *output) {
    int ret=!zmk_keypoint_relay_routed() && zmk_endpoint_get_selected().transport==ZMK_TRANSPORT_USB ?
        accept_data(request->payload.bytes,request->payload.size) : -EACCES;
    memset(reply,0,sizeof(reply));reply[0]=1;sys_put_le32(ret,reply+1);
    output->funcs.encode=encode_reply;output->arg=NULL;return true;
}
static struct zmk_rpc_custom_subsystem_meta telemetry_meta={.security=ZMK_STUDIO_RPC_HANDLER_UNSECURED};
ZMK_RPC_CUSTOM_SUBSYSTEM(zitaotech_keypoint_telemetry,&telemetry_meta,telemetry_rpc);
#endif
