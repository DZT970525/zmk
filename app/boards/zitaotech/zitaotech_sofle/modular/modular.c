/* SPDX-License-Identifier: MIT */
#include "modular.h"
#include <errno.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zmk/event_manager.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/central.h>
#if IS_ENABLED(CONFIG_ZMK_STUDIO) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/studio/custom.h>
#include <pb_encode.h>
#endif
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk,CONFIG_ZMK_LOG_LEVEL);
#include "trackpad_led.h"

/* Probe only known sensor addresses; never scan the display/EEPROM address space. */
static uint8_t local_kind, remote_kind;
uint8_t sofle_modular_local_kind(void) { return local_kind; }
uint32_t sofle_modular_scan_column(uint32_t row, uint32_t column) {
    /* The two module PCBs exchange encoder-push and pointer-push contacts.
     * Keep logical encoder/pointer positions stable for every keymap layer.
     * Row 5 (the two small buttons) is identical on both modules.
     */
#if IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || IS_ENABLED(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT)
    bool swapped = local_kind == SOFLE_MODULE_TRACKPOINT;
#else
    bool swapped = local_kind == SOFLE_MODULE_TOUCHPAD;
#endif
    unsigned module_row = (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) ||
                           IS_ENABLED(CONFIG_BOARD_ZITAOTECH_KEYPOINT_RIGHT)) ? 5 : 6;
    if (swapped && row == module_row && (column == 6 || column == 7)) return column ^ 1;
    return column;
}
int sofle_modular_step_dpi(bool right,int delta) {
    extern int zitaotech_trackpad_step_dpi(int);
    extern int zitaotech_trackpoint_step_dpi(int);
    extern int sofle_right_trackpad_step_dpi(int);
    extern int sofle_left_trackpoint_step_dpi(int);
    uint8_t kind=right?remote_kind:local_kind;
    if (kind==SOFLE_MODULE_TOUCHPAD)
        return right?sofle_right_trackpad_step_dpi(delta):zitaotech_trackpad_step_dpi(delta);
    if (kind==SOFLE_MODULE_TRACKPOINT)
        return right?zitaotech_trackpoint_step_dpi(delta):sofle_left_trackpoint_step_dpi(delta);
    return -ENODEV;
}
int sofle_modular_toggle_trackpad_mode(void) {
    extern int zitaotech_trackpad_toggle_mode(void);
    extern int sofle_right_trackpad_toggle_mode(void);
    int result=-ENOTSUP;
    if (local_kind==SOFLE_MODULE_TOUCHPAD) result=zitaotech_trackpad_toggle_mode();
    if (remote_kind==SOFLE_MODULE_TOUCHPAD) {
        int ret=sofle_right_trackpad_toggle_mode();
        if (!ret || result==-ENOTSUP) result=ret;
    }
    return result;
}
static int probe(void) {
    const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
    if (!device_is_ready(bus)) return -ENODEV;
    uint8_t reg = 0x82;
    for (int attempt=0; attempt<3; attempt++) {
        k_msleep(10);
        if (!i2c_write(bus,&reg,1,0x37) || !i2c_write(bus,&reg,1,0x3b)) {
            local_kind=SOFLE_MODULE_TOUCHPAD; return 0;
        }
        uint8_t packet[7];
        if (!i2c_read(bus,packet,sizeof(packet),0x15)) {
            local_kind=SOFLE_MODULE_TRACKPOINT; return 0;
        }
    }
    return 0; /* No sensor: leave both drivers/LED workers inactive. */
}
SYS_INIT(probe,POST_KERNEL,65);

struct sofle_modular_status { uint8_t kind, command, source; };
ZMK_EVENT_DECLARE(sofle_modular_status);
ZMK_EVENT_IMPL(sofle_modular_status);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(sofle_modular_status, smd, source);
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(sofle_modular_status, smd, source);
ZMK_RELAY_EVENT_HANDLE(sofle_modular_status, smd, source);
static void announce(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(announce_work,announce);
static void announce(struct k_work *work) {
    ARG_UNUSED(work);
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    raise_sofle_modular_status((struct sofle_modular_status){local_kind,0,ZMK_RELAY_EVENT_SOURCE_SELF});
    k_work_reschedule(&announce_work,K_SECONDS(2));
#endif
}
void sofle_modular_right_arrow(void) {
    raise_sofle_modular_status((struct sofle_modular_status){0,2,ZMK_RELAY_EVENT_SOURCE_SELF});
}
static int status_listener(const zmk_event_t *eh) {
    struct sofle_modular_status *ev=as_sofle_modular_status(eh);
    if (ev && ev->source!=ZMK_RELAY_EVENT_SOURCE_SELF) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        if (!ev->command && ev->kind<=SOFLE_MODULE_TRACKPOINT) {
            bool changed=remote_kind!=ev->kind;
            remote_kind=ev->kind;
            if (changed && remote_kind==SOFLE_MODULE_TOUCHPAD) {
                extern void sofle_modular_sync_right_pad(void);
                sofle_modular_sync_right_pad();
            }
        }
#else
        if (ev->command==2 && local_kind==SOFLE_MODULE_TOUCHPAD) indicator_tp_arrow_fired();
#endif
    }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    const struct zmk_split_peripheral_status_changed *split=as_zmk_split_peripheral_status_changed(eh);
    if (split && !split->connected) remote_kind=0;
#endif
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(sofle_modular_devices,status_listener);
ZMK_SUBSCRIPTION(sofle_modular_devices,sofle_modular_status);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_SUBSCRIPTION(sofle_modular_devices,zmk_split_peripheral_status_changed);
#endif
static int start(void) { k_work_reschedule(&announce_work,K_MSEC(500)); return 0; }
SYS_INIT(start,APPLICATION,95);
#if IS_ENABLED(CONFIG_ZMK_STUDIO) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static uint8_t reply[3];
static bool encode(pb_ostream_t *stream,const pb_field_t *field,void *const *arg) {
    ARG_UNUSED(arg);
    return pb_encode_tag_for_field(stream,field) && pb_encode_string(stream,reply,sizeof(reply));
}
static bool rpc(const zmk_custom_CallRequest *request,pb_callback_t *output) {
    if (request->payload.size!=1 || request->payload.bytes[0]!=1) return false;
    reply[0]=1;reply[1]=local_kind;reply[2]=remote_kind;
    output->funcs.encode=encode;output->arg=NULL;return true;
}
static struct zmk_rpc_custom_subsystem_meta meta={.security=ZMK_STUDIO_RPC_HANDLER_UNSECURED};
ZMK_RPC_CUSTOM_SUBSYSTEM(sofle__pointing_modules,&meta,rpc);
#endif
