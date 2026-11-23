/* SPDX-License-Identifier: MIT */
#include "config_snapshot.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/settings/settings.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/behaviors/runtime_sensor_rotate.h>
#include <cormoran/zmk/runtime_macro.h>

/* Only used after full snapshot validation. Bounds and registry checks still
 * run before any mutation; names resolve to this build's stable device strings. */
struct restore_reader { const uint8_t *buffer; size_t offset; };
static uint32_t take(struct restore_reader *reader, unsigned int bytes) {
    uint32_t value=0;
    for (unsigned int i=0;i<bytes;i++) { value|=(uint32_t)reader->buffer[reader->offset++]<<(8*i); }
    return value;
}
static size_t take_string(struct restore_reader *reader, char *text) {
    size_t size=take(reader,1);
    memcpy(text,reader->buffer+reader->offset,size); text[size]=0;
    reader->offset+=size;
    return size;
}
struct resolved_binding { const char *name; uint16_t id; };
static struct zmk_behavior_binding take_binding(struct restore_reader *reader,
                                                const struct resolved_binding *dictionary) {
    unsigned int index=take(reader,1);
    struct zmk_behavior_binding binding={.param1=take(reader,4),.param2=take(reader,4)};
    if (index!=UINT8_MAX) {
        binding.behavior_dev=dictionary[index].name;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
        binding.local_id=dictionary[index].id;
#endif
    }
    return binding;
}
static int delete_macro(uint32_t slot, const char *name, size_t size, bool unsaved, void *context) {
    (void)slot; (void)size; (void)unsaved; (void)context;
    return zmk_runtime_macro_delete(name);
}
static int restore_macros(struct restore_reader reader, unsigned int count) {
    int ret=zmk_runtime_macro_for_each(delete_macro,NULL);
    if (ret) { return ret; }
    for (unsigned int i=0;i<count;i++) {
        unsigned int slot=take(&reader,1);
        char name[CONFIG_ZMK_RUNTIME_MACRO_NAME_MAX_LEN+1];
        take_string(&reader,name);
        size_t size=take(&reader,2);
        uint8_t blob[sizeof(ZMK_RUNTIME_MACRO_KEY_PREFIX)+CONFIG_ZMK_RUNTIME_MACRO_NAME_MAX_LEN+
                     CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];
        int prefix=snprintf((char *)blob,sizeof(blob),"%s%s",ZMK_RUNTIME_MACRO_KEY_PREFIX,name);
        if (prefix<0 || (size_t)prefix+1+size>sizeof(blob)) { return -EMSGSIZE; }
        memcpy(blob+prefix+1,reader.buffer+reader.offset,size); reader.offset+=size;
        char key[96];
        snprintf(key,sizeof(key),"custom_settings/%s/%s#%u",ZMK_RUNTIME_MACRO_SUBSYSTEM_ID,
                  ZMK_RUNTIME_MACRO_KEY_PREFIX,slot);
        /* The module documents ordinal storage identities and binds them in
         * its settings handler. Persist before invoking that same handler. */
        ret=settings_save_one(key,blob,prefix+1+size);
        if (ret) { return ret; }
        ret=settings_runtime_set(key,blob,prefix+1+size);
        if (ret) { return ret; }
        uint32_t actual;
        ret=zmk_runtime_macro_slot_for_name(name,&actual);
        if (ret) { return ret; }
        if (actual!=slot) { return -EBADMSG; }
    }
    return 0;
}

int keypoint_config_snapshot_restore(struct keypoint_config_store *store,
                                     const uint8_t *buffer, size_t length) {
    if (!store || !buffer) { return -EINVAL; }
    if (length>KEYPOINT_CONFIG_MAX_SIZE) { return -EMSGSIZE; }
    if (!store->committed.generation || store->schema!=1 ||
        keypoint_config_crc32(buffer,length)!=store->committed.crc) { return -ESTALE; }
    int ret=keypoint_config_snapshot_validate(buffer,length);
    if (ret) { return ret; }
    struct restore_reader reader={.buffer=buffer,.offset=9};
    unsigned int names=take(&reader,1), macros=take(&reader,1), default_layer=take(&reader,1);
    /* ZMK's global default is immutable; per-endpoint default-layer settings
     * are a separate schema extension, not a silently ignored field. */
    if (default_layer!=zmk_keymap_layer_default()) { return -ENOTSUP; }
    struct resolved_binding dictionary[128];
    for (unsigned int i=0;i<names;i++) {
        char name[256]; take_string(&reader,name);
        const struct device *device=zmk_behavior_get_binding(name);
        if (!device) { return -ENODEV; }
        dictionary[i]=(struct resolved_binding){.name=device->name,.id=zmk_behavior_get_local_id(name)};
        if (dictionary[i].id==UINT16_MAX) { return -ENODEV; }
    }
    uint8_t order[ZMK_KEYMAP_LAYERS_LEN];
    for (unsigned int i=0;i<ZMK_KEYMAP_LAYERS_LEN;i++) { order[i]=take(&reader,1); }
#if !IS_ENABLED(CONFIG_ZMK_KEYMAP_LAYER_REORDERING)
    for (unsigned int i=0;i<ZMK_KEYMAP_LAYERS_LEN;i++) {
        if (order[i]!=zmk_keymap_layer_index_to_id(i)) { return -ENOTSUP; }
    }
#endif
    struct restore_reader layers=reader;
    for (unsigned int layer=0;layer<ZMK_KEYMAP_LAYERS_LEN;layer++) {
        size_t size=take(&reader,1);
        reader.offset+=size+ZMK_KEYMAP_LEN*9+ZMK_KEYMAP_SENSORS_LEN*26;
    }
    ret=restore_macros(reader,macros);
    if (ret) { return ret; }
#if IS_ENABLED(CONFIG_ZMK_KEYMAP_LAYER_REORDERING)
    for (int i=ZMK_KEYMAP_LAYERS_LEN-1;i>=0;i--) {
        if (zmk_keymap_layer_index_to_id(i)!=UINT8_MAX) {
            ret=zmk_keymap_remove_layer(i);
            if (ret) { return ret; }
        }
    }
    for (unsigned int i=0;i<ZMK_KEYMAP_LAYERS_LEN;i++) {
        if (order[i]!=UINT8_MAX) {
            ret=zmk_keymap_restore_layer(order[i],i);
            if (ret) { return ret; }
        }
    }
#endif
    for (unsigned int layer=0;layer<ZMK_KEYMAP_LAYERS_LEN;layer++) {
        char name[256]; size_t size=take_string(&layers,name);
        ret=zmk_keymap_set_layer_name(layer,name,size);
        if (ret) { return ret; }
        for (unsigned int key=0;key<ZMK_KEYMAP_LEN;key++) {
            struct zmk_behavior_binding binding=take_binding(&layers,dictionary);
            ret=zmk_keymap_set_layer_binding_at_idx(layer,key,binding);
            if (ret) { return ret; }
        }
        for (unsigned int sensor=0;sensor<ZMK_KEYMAP_SENSORS_LEN;sensor++) {
            struct runtime_sensor_rotate_layer_bindings pair={0};
            struct zmk_behavior_binding cw=take_binding(&layers,dictionary);
            pair.cw_binding.behavior_local_id=cw.behavior_dev ? zmk_behavior_get_local_id(cw.behavior_dev) : 0;
            pair.cw_binding.param1=cw.param1; pair.cw_binding.param2=cw.param2;
            pair.cw_binding.tap_ms=take(&layers,4);
            struct zmk_behavior_binding ccw=take_binding(&layers,dictionary);
            pair.ccw_binding.behavior_local_id=ccw.behavior_dev ? zmk_behavior_get_local_id(ccw.behavior_dev) : 0;
            pair.ccw_binding.param1=ccw.param1; pair.ccw_binding.param2=ccw.param2;
            pair.ccw_binding.tap_ms=take(&layers,4);
            ret=zmk_runtime_sensor_rotate_set_layer_bindings(sensor,layer,&pair);
            if (ret) { return ret; }
        }
    }
    return zmk_keymap_save_changes();
}

int keypoint_config_snapshot_receive(const uint8_t *buffer, size_t length,
                                     struct keypoint_config_revision revision, void *context) {
    struct keypoint_config_store *store=context;
    if (!store) { return -EINVAL; }
    if (store->committed.generation==revision.generation) {
        if (store->committed.crc!=revision.crc) { return -ESTALE; }
    } else {
        int ret=keypoint_config_snapshot_commit(buffer,length,revision,store);
        if (ret) { return ret; }
    }
    return keypoint_config_snapshot_restore(store,buffer,length);
}
