/* Opt-in momentary-layer priority; independent of the HID and split protocols. */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zmk/keymap.h>
#include <zmk/mo_priority.h>
#include <zmk/events/mo_priority_changed.h>

ZMK_EVENT_IMPL(zmk_mo_priority_changed);
void zmk_mo_priority_notify_display(void) {
    raise_zmk_mo_priority_changed((struct zmk_mo_priority_changed){0});
}

#define MO_PRIORITY_SLOTS 64
struct held_mo { uint32_t position; uint32_t sequence; uint32_t higher_at_press; uint8_t source; uint8_t layer; bool used; };
struct mo_layer { unsigned count; bool originally_active; bool managed; };
static struct held_mo held[MO_PRIORITY_SLOTS];
static struct mo_layer layers[ZMK_KEYMAP_LAYERS_LEN];
static struct k_spinlock tracker_lock;
static uint32_t next_sequence;
static atomic_t enabled;

/* Reordering changes layer priority without changing stable IDs. */
static uint32_t active_higher_layers(uint8_t target) {
    bool above_target = false;
    uint32_t result = 0;
    for (unsigned i = 0; i < ZMK_KEYMAP_LAYERS_LEN; ++i) {
        uint8_t layer = zmk_keymap_layer_index_to_id(i);
        if (layer == target) { above_target = true; continue; }
        if (above_target && layer < 32 && zmk_keymap_layer_active(layer)) result |= BIT(layer);
    }
    return result;
}

int zmk_mo_priority_press(uint8_t source, uint32_t position, uint8_t layer) {
    if (layer >= ZMK_KEYMAP_LAYERS_LEN) return -EINVAL;
    k_spinlock_key_t key = k_spin_lock(&tracker_lock);
    int free_slot = -1;
    for (int i = 0; i < MO_PRIORITY_SLOTS; ++i) {
        if (held[i].used && held[i].source == source && held[i].position == position &&
            held[i].layer == layer) { k_spin_unlock(&tracker_lock, key); return 0; }
        if (!held[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) { k_spin_unlock(&tracker_lock, key); return -ENOMEM; }
    struct mo_layer *state = &layers[layer];
    if (!state->count) {
        state->originally_active = zmk_keymap_layer_active(layer);
        state->managed = false;
    }
    state->count++;
    if (atomic_get(&enabled)) state->managed = true;
    held[free_slot] = (struct held_mo){.position=position,.sequence=++next_sequence,
                                        .higher_at_press=active_higher_layers(layer),
                                        .source=source,.layer=layer,.used=true};
    k_spin_unlock(&tracker_lock, key);
    return 1;
}

int zmk_mo_priority_release(uint8_t source, uint32_t position, uint8_t layer) {
    k_spinlock_key_t key = k_spin_lock(&tracker_lock);
    for (int i = 0; i < MO_PRIORITY_SLOTS; ++i) {
        if (!held[i].used || held[i].source != source || held[i].position != position ||
            held[i].layer != layer) continue;
        uint8_t actual = held[i].layer;
        held[i].used = false;
        struct mo_layer *state = &layers[actual];
        if (atomic_get(&enabled)) state->managed = true;
        if (state->count) state->count--;
        bool deactivate = !state->managed || (!state->count && !state->originally_active);
        if (!state->count) state->managed = false;
        k_spin_unlock(&tracker_lock, key);
        return deactivate ? 1 : 0;
    }
    /* A repeated or failed press must not release somebody else's MO hold.
     * With the option disabled and no managed hold, retain ZMK's old behavior. */
    bool skip = atomic_get(&enabled) || (layer < ZMK_KEYMAP_LAYERS_LEN && layers[layer].managed);
    k_spin_unlock(&tracker_lock, key);
    return skip ? 0 : 1;
}

int zmk_mo_priority_current(void) {
    if (!atomic_get(&enabled)) return -1;
    k_spinlock_key_t key = k_spin_lock(&tracker_lock);
    /* Calculate higher-active masks once, rather than scanning layers once per
     * held key while interrupts are locked. Invalid/reordered IDs are skipped. */
    uint32_t higher[ZMK_KEYMAP_LAYERS_LEN] = {0};
    uint32_t mask = 0;
    for (int i = ZMK_KEYMAP_LAYERS_LEN - 1; i >= 0; --i) {
        uint8_t layer = zmk_keymap_layer_index_to_id(i);
        if (layer >= ZMK_KEYMAP_LAYERS_LEN || layer >= 32) continue;
        higher[layer] = mask;
        if (zmk_keymap_layer_active(layer)) mask |= BIT(layer);
    }
    int best = -1;
    uint32_t seq = 0;
    for (int i = 0; i < MO_PRIORITY_SLOTS; ++i) {
        if (held[i].used && held[i].higher_at_press && zmk_keymap_layer_active(held[i].layer) &&
            !(higher[held[i].layer] & ~held[i].higher_at_press) &&
            (best < 0 || (int32_t)(held[i].sequence - seq) > 0)) {
            best = held[i].layer;
            seq = held[i].sequence;
        }
    }
    k_spin_unlock(&tracker_lock, key);
    return best;
}

static int mo_settings_set(const char *key, size_t len, settings_read_cb read, void *arg) {
    if (strcmp(key, "enabled")) return -ENOENT;
    if (len != 1) return -EINVAL;
    uint8_t value; int ret = read(arg, &value, 1);
    if (ret < 0) return ret;
    if (ret != 1 || value > 1) return -EINVAL;
    atomic_set(&enabled, value); return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(mo_priority, "zitaotech_mo_priority", NULL, mo_settings_set, NULL, NULL);

#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
#include <zmk/studio/custom.h>
#include <pb_encode.h>
static uint8_t reply[6];
static bool encode_reply(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    ARG_UNUSED(arg);
    return pb_encode_tag_for_field(stream, field) && pb_encode_string(stream, reply, sizeof(reply));
}
static bool mo_rpc(const zmk_custom_CallRequest *request, pb_callback_t *output) {
    const uint8_t *bytes = request->payload.bytes; size_t len = request->payload.size;
    int ret = -EINVAL;
    if (len == 2 && bytes[0] == 1 && bytes[1] == 0) ret = 0;
    else if (len == 3 && bytes[0] == 1 && bytes[1] == 1 && bytes[2] <= 1) {
        ret = settings_save_one("zitaotech_mo_priority/enabled", bytes+2, 1);
        if (!ret) {
            bool changed = atomic_get(&enabled) != bytes[2];
            atomic_set(&enabled, bytes[2]);
            if (changed) zmk_mo_priority_notify_display();
        }
    }
    reply[0] = 1; sys_put_le32(ret, reply+1); reply[5] = atomic_get(&enabled) != 0;
    output->funcs.encode = encode_reply; output->arg = NULL; return true;
}
static struct zmk_rpc_custom_subsystem_meta meta = {.security = ZMK_STUDIO_RPC_HANDLER_SECURED};
ZMK_RPC_CUSTOM_SUBSYSTEM(zitaotech_mo_priority, &meta, mo_rpc);
#endif
