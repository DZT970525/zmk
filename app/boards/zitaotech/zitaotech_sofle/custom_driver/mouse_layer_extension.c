/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zmk/pointing/temp_layer.h>
#include <zmk/keypoint_mouse_layer.h>
/* Persisted as [enabled, extra_ms LE16]. Fresh devices default off, 200ms. */
static atomic_t extension_config = ATOMIC_INIT(200);
static bool valid_config(const uint8_t *bytes) {
    uint16_t ms=sys_get_le16(bytes+1);
    return bytes[0]<=1 && ms>=50 && ms<=2000;
}
static void apply_config(const uint8_t *bytes) {
    atomic_set(&extension_config,sys_get_le16(bytes+1)|((uint32_t)bytes[0]<<16));
}
static int extension_settings_set(const char *key,size_t len,settings_read_cb read,void *arg) {
    if(strcmp(key,"config"))return -ENOENT;
    if(len!=3)return -EINVAL;
    uint8_t bytes[3];
    int ret=read(arg,bytes,sizeof(bytes));
    if(ret<0)return ret;
    if(ret!=sizeof(bytes)||!valid_config(bytes))return -EINVAL;
    apply_config(bytes);return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(keypoint_mouse_extension,"keypoint_mouse_extension",NULL,
                              extension_settings_set,NULL,NULL);
void zmk_keypoint_mouse_layer_button_pressed(void) {
    uint32_t config=atomic_get(&extension_config);
    if(config & (1U<<16))
        zmk_input_processor_temp_layer_extend(CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER,config & 0xffff);
}
#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
#include <zmk/studio/custom.h>
#include <pb_encode.h>
static uint8_t reply[8];
static bool encode_reply(pb_ostream_t *stream,const pb_field_t *field,void *const *arg) {
    ARG_UNUSED(arg);
    return pb_encode_tag_for_field(stream,field) && pb_encode_string(stream,reply,sizeof(reply));
}
static bool extension_rpc(const zmk_custom_CallRequest *request,pb_callback_t *output) {
    const uint8_t *bytes=request->payload.bytes;size_t len=request->payload.size;
    int ret=-EINVAL;
    if(len==2 && bytes[0]==1 && bytes[1]==0)ret=0;
    else if(len==5 && bytes[0]==1 && bytes[1]==1 && valid_config(bytes+2)) {
        ret=settings_save_one("keypoint_mouse_extension/config",bytes+2,3);
        if(!ret)apply_config(bytes+2);
    }
    uint32_t config=atomic_get(&extension_config);
    reply[0]=1;sys_put_le32(ret,reply+1);reply[5]=(config>>16)&1;
    sys_put_le16(config&0xffff,reply+6);
    output->funcs.encode=encode_reply;output->arg=NULL;return true;
}
static struct zmk_rpc_custom_subsystem_meta extension_meta={.security=ZMK_STUDIO_RPC_HANDLER_SECURED};
ZMK_RPC_CUSTOM_SUBSYSTEM(zitaotech_mouse_extension,&extension_meta,extension_rpc);
#endif
/* Separate opt-in policy: absent settings default OFF without clearing keymaps. */
static atomic_t exit_on_key = ATOMIC_INIT(0);
bool zmk_keypoint_mouse_layer_exit_on_key_enabled(void) { return atomic_get(&exit_on_key) != 0; }
static int exit_settings_set(const char *key,size_t len,settings_read_cb read,void *arg) {
    if(strcmp(key,"enabled"))return -ENOENT;
    if(len!=1)return -EINVAL;
    uint8_t value;int ret=read(arg,&value,1);
    if(ret<0)return ret;
    if(ret!=1 || value>1)return -EINVAL;
    atomic_set(&exit_on_key,value);return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(mouse_exit,"zitaotech_mouse_exit",NULL,exit_settings_set,NULL,NULL);
#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
static bool exit_rpc(const zmk_custom_CallRequest *request,pb_callback_t *output) {
    const uint8_t *bytes=request->payload.bytes;size_t len=request->payload.size;
    int ret=-EINVAL;
    if(len==2 && bytes[0]==1 && bytes[1]==0)ret=0;
    else if(len==3 && bytes[0]==1 && bytes[1]==1 && bytes[2]<=1) {
        ret=settings_save_one("zitaotech_mouse_exit/enabled",bytes+2,1);
        if(!ret)atomic_set(&exit_on_key,bytes[2]);
    }
    reply[0]=1;sys_put_le32(ret,reply+1);reply[5]=atomic_get(&exit_on_key);reply[6]=0;reply[7]=0;
    output->funcs.encode=encode_reply;output->arg=NULL;return true;
}
static struct zmk_rpc_custom_subsystem_meta exit_meta={.security=ZMK_STUDIO_RPC_HANDLER_SECURED};
ZMK_RPC_CUSTOM_SUBSYSTEM(zitaotech_mouse_exit,&exit_meta,exit_rpc);
#endif
