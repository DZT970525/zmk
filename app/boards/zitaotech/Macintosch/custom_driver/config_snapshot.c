/* SPDX-License-Identifier: MIT */
#include "config_snapshot.h"
#include "config_sync_journal.h"
#include <errno.h>
#include <string.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/behaviors/runtime_sensor_rotate.h>
#include <cormoran/zmk/runtime_macro.h>

#define DICTIONARY_LIMIT 128
struct capture {
    uint8_t *buffer;
    size_t capacity;
    size_t used;
    int error;
    const char *dictionary[DICTIONARY_LIMIT];
    uint8_t names;
    uint8_t macros;
};
static void emit(struct capture *state, uint32_t value, unsigned int bytes) {
    if (state->error) { return; }
    if (bytes > state->capacity - state->used) { state->error=-ENOSPC; return; }
    for (unsigned int i=0; i<bytes; i++) { state->buffer[state->used++]=value>>(8*i); }
}
static void string(struct capture *state, const char *text) {
    size_t length=text ? strlen(text) : 0;
    if (length > UINT8_MAX) { state->error=-ENAMETOOLONG; return; }
    emit(state,length,1);
    if (state->error) { return; }
    if (length > state->capacity-state->used) { state->error=-ENOSPC; return; }
    if (length) { memcpy(state->buffer+state->used,text,length); }
    state->used+=length;
}
static int name_index(struct capture *state, const char *name, bool add) {
    /* Missing/disabled bindings have an explicit sentinel. */
    if (!name || !name[0]) { return UINT8_MAX; }
    for (unsigned int i=0;i<state->names;i++) {
        if (!strcmp(state->dictionary[i],name)) { return i; }
    }
    if (!add) { return -ESTALE; }
    if (state->names==DICTIONARY_LIMIT) { return -ENOSPC; }
    state->dictionary[state->names]=name;
    return state->names++;
}
static const char *local_name(struct capture *state, uint16_t id) {
    const char *name=zmk_behavior_find_behavior_name_from_local_id(id);
    if (!name && id && id!=UINT16_MAX) { state->error=-ENODEV; }
    return name;
}
static const char *binding_name(struct capture *state, const struct zmk_behavior_binding *binding) {
    if (binding->behavior_dev && binding->behavior_dev[0]) { return binding->behavior_dev; }
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    if (binding->local_id) {
        return local_name(state,binding->local_id);
    }
#endif
    return binding->behavior_dev;
}
static int collect_names(struct capture *state) {
    for (unsigned int layer=0;layer<ZMK_KEYMAP_LAYERS_LEN;layer++) {
        for (unsigned int key=0;key<ZMK_KEYMAP_LEN;key++) {
            const struct zmk_behavior_binding *binding=zmk_keymap_get_layer_binding_at_idx(layer,key);
            if (!binding) { return -EINVAL; }
            int ret=name_index(state,binding_name(state,binding),true);
            if (state->error) { return state->error; }
            if (ret<0) { return ret; }
        }
        for (unsigned int sensor=0;sensor<ZMK_KEYMAP_SENSORS_LEN;sensor++) {
            struct runtime_sensor_rotate_layer_bindings pair;
            int ret=zmk_runtime_sensor_rotate_get_layer_bindings(sensor,layer,&pair);
            if (ret) { return ret; }
            ret=name_index(state,local_name(state,pair.cw_binding.behavior_local_id),true);
            if (state->error) { return state->error; }
            if (ret<0) { return ret; }
            ret=name_index(state,local_name(state,pair.ccw_binding.behavior_local_id),true);
            if (state->error) { return state->error; }
            if (ret<0) { return ret; }
        }
    }
    return 0;
}
static void binding(struct capture *state, const char *name, uint32_t p1, uint32_t p2) {
    int index=name_index(state,name,false);
    if (index<0) { state->error=index; return; }
    emit(state,index,1); emit(state,p1,4); emit(state,p2,4);
}
static int macro(uint32_t slot, const char *name, size_t encoded_size, bool unsaved, void *context) {
    (void)unsaved;
    struct capture *state=context;
    if (slot>=CONFIG_ZMK_RUNTIME_MACRO_COUNT || state->macros>=CONFIG_ZMK_RUNTIME_MACRO_COUNT ||
        encoded_size>CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES) { return -EINVAL; }
    emit(state,slot,1); string(state,name); emit(state,encoded_size,2);
    if (state->error) { return state->error; }
    if (encoded_size>state->capacity-state->used) { return -ENOSPC; }
    size_t actual;
    int ret=zmk_runtime_macro_read(name,state->buffer+state->used,encoded_size,&actual);
    if (ret) { return ret; }
    if (actual!=encoded_size) { return -ESTALE; }
    state->used+=actual; state->macros++;
    return 0;
}
int keypoint_config_snapshot_capture(uint8_t *buffer, size_t capacity, size_t *length) {
    if (!buffer || !length) { return -EINVAL; }
    if (capacity>KEYPOINT_CONFIG_MAX_SIZE) { capacity=KEYPOINT_CONFIG_MAX_SIZE; }
    struct capture state={.buffer=buffer,.capacity=capacity};
    int ret=collect_names(&state);
    if (ret) { return ret; }
    /* KPS1, schema, layer count, key count, sensors, dictionary count,
     * macro count (filled last), default stable layer ID. All integers LE. */
    emit(&state,0x3153504b,4); emit(&state,1,1);
    emit(&state,ZMK_KEYMAP_LAYERS_LEN,1); emit(&state,ZMK_KEYMAP_LEN,2);
    emit(&state,ZMK_KEYMAP_SENSORS_LEN,1); emit(&state,state.names,1);
    emit(&state,0,1); emit(&state,zmk_keymap_layer_default(),1);
    for (unsigned int i=0;i<state.names;i++) { string(&state,state.dictionary[i]); }
    /* Preserve layer order and inactive IDs independently of binding order. */
    for (unsigned int i=0;i<ZMK_KEYMAP_LAYERS_LEN;i++) {
        emit(&state,zmk_keymap_layer_index_to_id(i),1);
    }
    for (unsigned int layer=0;layer<ZMK_KEYMAP_LAYERS_LEN;layer++) {
        string(&state,zmk_keymap_layer_name(layer));
        for (unsigned int key=0;key<ZMK_KEYMAP_LEN;key++) {
            const struct zmk_behavior_binding *item=zmk_keymap_get_layer_binding_at_idx(layer,key);
            if (!item) { return -ESTALE; }
            binding(&state,binding_name(&state,item),item->param1,item->param2);
        }
        for (unsigned int sensor=0;sensor<ZMK_KEYMAP_SENSORS_LEN;sensor++) {
            struct runtime_sensor_rotate_layer_bindings pair;
            ret=zmk_runtime_sensor_rotate_get_layer_bindings(sensor,layer,&pair);
            if (ret) { return ret; }
            binding(&state,local_name(&state,pair.cw_binding.behavior_local_id),pair.cw_binding.param1,pair.cw_binding.param2);
            emit(&state,pair.cw_binding.tap_ms,4);
            binding(&state,local_name(&state,pair.ccw_binding.behavior_local_id),pair.ccw_binding.param1,pair.ccw_binding.param2);
            emit(&state,pair.ccw_binding.tap_ms,4);
        }
    }
    if (state.error) { return state.error; }
    ret=zmk_runtime_macro_for_each(macro,&state);
    if (ret) { return ret; }
    if (state.error) { return state.error; }
    buffer[10]=state.macros;
    *length=state.used;
    return 0;
}

struct reader {
    const uint8_t *buffer;
    size_t length;
    size_t offset;
    int error;
};
static uint32_t read_integer(struct reader *reader, unsigned int bytes) {
    if (reader->error) { return 0; }
    if (bytes>reader->length-reader->offset) { reader->error=-EBADMSG; return 0; }
    uint32_t value=0;
    for (unsigned int i=0;i<bytes;i++) { value|=(uint32_t)reader->buffer[reader->offset++]<<(8*i); }
    return value;
}
static const uint8_t *read_bytes(struct reader *reader, size_t length) {
    if (reader->error) { return NULL; }
    if (length>reader->length-reader->offset) { reader->error=-EBADMSG; return NULL; }
    const uint8_t *result=reader->buffer+reader->offset;
    reader->offset+=length;
    return result;
}
static const uint8_t *read_string(struct reader *reader, size_t *length) {
    *length=read_integer(reader,1);
    const uint8_t *text=read_bytes(reader,*length);
    /* Embedded NUL would make registry lookup disagree with the wire name. */
    if (text && memchr(text,0,*length)) { reader->error=-EBADMSG; return NULL; }
    return text;
}
static void validate_binding(struct reader *reader, unsigned int names) {
    unsigned int index=read_integer(reader,1);
    read_integer(reader,4); read_integer(reader,4);
    if (index!=UINT8_MAX && index>=names) { reader->error=-EBADMSG; }
}
int keypoint_config_snapshot_validate(const uint8_t *buffer, size_t length) {
    if (!buffer) { return -EINVAL; }
    if (length>KEYPOINT_CONFIG_MAX_SIZE) { return -EMSGSIZE; }
    struct reader reader={.buffer=buffer,.length=length};
    if (read_integer(&reader,4)!=0x3153504b) { return -EBADMSG; }
    if (read_integer(&reader,1)!=1) { return -EPROTONOSUPPORT; }
    unsigned int layers=read_integer(&reader,1), keys=read_integer(&reader,2);
    unsigned int sensors=read_integer(&reader,1), names=read_integer(&reader,1);
    unsigned int macros=read_integer(&reader,1), default_layer=read_integer(&reader,1);
    if (reader.error) { return reader.error; }
    if (layers!=ZMK_KEYMAP_LAYERS_LEN || keys!=ZMK_KEYMAP_LEN ||
        sensors!=ZMK_KEYMAP_SENSORS_LEN) { return -EXDEV; }
    if (layers>32 || default_layer>=layers || names>DICTIONARY_LIMIT ||
        macros>CONFIG_ZMK_RUNTIME_MACRO_COUNT) { return -EBADMSG; }
    if (default_layer!=zmk_keymap_layer_default()) { return -ENOTSUP; }
    const uint8_t *dictionary[DICTIONARY_LIMIT];
    uint8_t dictionary_lengths[DICTIONARY_LIMIT];
    for (unsigned int i=0;i<names;i++) {
        size_t size;
        dictionary[i]=read_string(&reader,&size); dictionary_lengths[i]=size;
        if (reader.error) { return reader.error; }
        if (!size) { return -EBADMSG; }
        for (unsigned int j=0;j<i;j++) {
            if (dictionary_lengths[j]==size && !memcmp(dictionary[j],dictionary[i],size)) { return -EBADMSG; }
        }
        char name[256]; memcpy(name,dictionary[i],size); name[size]=0;
        if (!zmk_behavior_get_binding(name)) { return -ENODEV; }
    }
    uint32_t active_layers=0;
    for (unsigned int i=0;i<layers;i++) {
        unsigned int id=read_integer(&reader,1);
        if (reader.error) { return reader.error; }
        if (id==UINT8_MAX) { continue; }
        if (id>=layers || (active_layers & (1u<<id))) { return -EBADMSG; }
        active_layers|=1u<<id;
    }
    if (!(active_layers & (1u<<default_layer))) { return -EBADMSG; }
    for (unsigned int layer=0;layer<layers;layer++) {
        size_t size; read_string(&reader,&size);
        if (size>=CONFIG_ZMK_KEYMAP_LAYER_NAME_MAX_LEN) { return -ENAMETOOLONG; }
        for (unsigned int key=0;key<keys;key++) { validate_binding(&reader,names); }
        for (unsigned int sensor=0;sensor<sensors;sensor++) {
            validate_binding(&reader,names); read_integer(&reader,4);
            validate_binding(&reader,names); read_integer(&reader,4);
        }
        if (reader.error) { return reader.error; }
    }
    uint32_t slots=0;
    size_t pool_bytes=0;
    const uint8_t *macro_names[32]; uint8_t macro_name_lengths[32];
    for (unsigned int i=0;i<macros;i++) {
        unsigned int slot=read_integer(&reader,1);
        size_t size; macro_names[i]=read_string(&reader,&size); macro_name_lengths[i]=size;
        if (reader.error) { return reader.error; }
        if (slot>=CONFIG_ZMK_RUNTIME_MACRO_COUNT || !size || (slots & (1u<<slot))) { return -EBADMSG; }
        if (size>CONFIG_ZMK_RUNTIME_MACRO_NAME_MAX_LEN) { return -ENAMETOOLONG; }
        slots|=1u<<slot;
        for (unsigned int j=0;j<i;j++) {
            if (macro_name_lengths[j]==size && !memcmp(macro_names[j],macro_names[i],size)) { return -EBADMSG; }
        }
        size_t encoded=read_integer(&reader,2);
        if (encoded>CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES) { return -EMSGSIZE; }
        pool_bytes+=size+sizeof(ZMK_RUNTIME_MACRO_KEY_PREFIX)+encoded;
        if (pool_bytes>CONFIG_ZMK_RUNTIME_MACRO_POOL_BYTES) { return -ENOSPC; }
        const uint8_t *body=read_bytes(&reader,encoded);
        if (reader.error) { return reader.error; }
        int ret=zmk_runtime_macro_validate_encoded(body,encoded);
        if (ret) { return ret; }
    }
    return reader.offset==length ? 0 : -EBADMSG;
}

int keypoint_config_snapshot_commit(const uint8_t *buffer, size_t length,
                                    struct keypoint_config_revision revision, void *context) {
    struct keypoint_config_store *store=context;
    if (!store || store->schema!=1) { return -EPROTONOSUPPORT; }
    int ret=keypoint_config_snapshot_validate(buffer,length);
    if (ret) { return ret; }
    return keypoint_config_store_commit(store,buffer,length,revision);
}

int keypoint_config_snapshot_checkpoint(struct keypoint_config_store *store, uint8_t *buffer,
                                        size_t capacity, size_t *length) {
    if (!store) { return -EINVAL; }
    if (store->committed.generation==UINT64_MAX) { return -EOVERFLOW; }
    int ret=keypoint_config_snapshot_capture(buffer,capacity,length);
    if (ret) { return ret; }
    struct keypoint_config_revision revision={
        .generation=store->committed.generation+1,
        .crc=keypoint_config_crc32(buffer,*length),
    };
    return keypoint_config_snapshot_commit(buffer,*length,revision,store);
}
