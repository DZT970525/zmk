/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zmk/studio/custom.h>
#include <zmk/split/role.h>
#include <zmk/split/peer_selection.h>
#include <pb_encode.h>
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
#include <zmk/keypoint_relay.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_CONFIG_MIRROR)
#include "topology_control_client.h"
K_SEM_DEFINE(topology_rpc_done,0,1);
static int remote_result;
static struct keypoint_topology_status remote_status;
static void remote_done(int result,struct keypoint_topology_status status,void *context) {
    ARG_UNUSED(context);remote_result=result;remote_status=status;k_sem_give(&topology_rpc_done);
}
#endif
static uint8_t response[8];
static bool encode(pb_ostream_t *stream,const pb_field_t *field,void *const *arg) {
    ARG_UNUSED(arg);
    return pb_encode_tag_for_field(stream,field) && pb_encode_string(stream,response,sizeof(response));
}
static bool topology_rpc(const zmk_custom_CallRequest *request,pb_callback_t *output) {
    const uint8_t *command=request->payload.bytes;
    int ret=0;bool enabled=false,central=false;
    if (request->payload.size<2 || command[0]!=1 || command[1]>1 ||
        request->payload.size!=(command[1] ? 3 : 2) || (command[1] && command[2]>1)) ret=-EINVAL;
    else {
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
        if (command[1]) ret=zmk_keypoint_relay_set_manual_enabled(command[2]);
        enabled=zmk_keypoint_relay_manual_enabled();
        central=!zmk_keypoint_relay_routed();
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_RUNTIME_ROLE)
        if (command[1]) ret=zmk_split_role_set_manual_switch_enabled(command[2]);
        enabled=zmk_split_role_manual_switch_enabled();central=zmk_split_role_is_central();
#elif IS_ENABLED(CONFIG_ZMK_KEYPOINT_CONFIG_MIRROR)
        struct bt_conn *left=zmk_keypoint_input_peer_connection(1);
        if (!left || !zmk_keypoint_config_rpc_ready()) ret=-ENOTCONN;
        else {
            k_sem_reset(&topology_rpc_done);
            ret=zmk_keypoint_topology_remote(left,command[1],command[1] && command[2],remote_done,NULL);
            /* Client owns the eight-second deadline and completes before
             * reusing its buffer. Never abandon a borrowed callback context. */
            if (!ret) {k_sem_take(&topology_rpc_done,K_FOREVER);ret=remote_result;}
            if (!ret) {enabled=remote_status.enabled;central=remote_status.central;}
        }
        if (left) bt_conn_unref(left);
#else
        ret=-ENOTSUP;
#endif
    }
    response[0]=1;sys_put_le32((uint32_t)ret,response+1);
    response[5]=enabled;response[6]=central;response[7]=!ret;
    output->funcs.encode=encode;output->arg=NULL;return true;
}
static struct zmk_rpc_custom_subsystem_meta meta={.security=ZMK_STUDIO_RPC_HANDLER_SECURED};
ZMK_RPC_CUSTOM_SUBSYSTEM(zitaotech_keypoint_topology,&meta,topology_rpc);
