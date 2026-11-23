/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT zitaotech_behavior_runtime_tap_dance
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#if IS_ENABLED(CONFIG_ZMK_RUNTIME_TAP_DANCE_STUDIO_RPC)
#include <zmk/studio/custom.h>
#include <pb_encode.h>
#endif

/* Wire/storage V1: enabled,count,flavor,reserved,term:u16,name[24],
 * four pairs of tap/hold bindings (id:u16,param1:u32,param2:u32).
 * 0xffff means no hold action; it falls back to the matching tap action.
 * Slots are saved explicitly, never from key or timer callbacks. */
#define SLOT_BYTES 110
#define MAX_TAPS 4
#define MAX_ACTIVE 8
#define MAX_CAPTURED 32
#define UNASSIGNED UINT16_MAX
enum flavor { HOLD_PREFERRED, BALANCED, TAP_PREFERRED };
static uint8_t slots[CONFIG_ZMK_RUNTIME_TAP_DANCE_COUNT][SLOT_BYTES];
K_MUTEX_DEFINE(config_lock);
static void defaults(uint8_t *slot) {
    memset(slot, 0, SLOT_BYTES);
    slot[1] = 2;
    slot[2] = TAP_PREFERRED;
    sys_put_le16(200, slot + 4);
    for (int i = 0; i < 8; i++) sys_put_le16(UNASSIGNED, slot + 30 + i * 10);
}
static struct zmk_behavior_binding decode_binding(const uint8_t *data) {
    return (struct zmk_behavior_binding){
        .behavior_dev = zmk_behavior_find_behavior_name_from_local_id(sys_get_le16(data)),
        .param1 = sys_get_le32(data + 2), .param2 = sys_get_le32(data + 6)};
}
static int validate(const uint8_t *slot) {
    if (slot[0] > 1 || slot[1] < 1 || slot[1] > MAX_TAPS || slot[2] > TAP_PREFERRED ||
        slot[3] || sys_get_le16(slot + 4) < 50 || sys_get_le16(slot + 4) > 2000 ||
        !memchr(slot + 6, 0, 24)) return -EINVAL;
    for (int i = 0; i < 8; i++) {
        const uint8_t *data = slot + 30 + i * 10;
        if (sys_get_le16(data) == UNASSIGNED) {
            if (slot[0] && i == 0) return -EINVAL;
            continue;
        }
        struct zmk_behavior_binding binding = decode_binding(data);
        if (!binding.behavior_dev || !strcmp(binding.behavior_dev, DEVICE_DT_NAME(DT_NODELABEL(rtd))))
            return -EINVAL;
        int ret = zmk_behavior_validate_binding(&binding);
        if (ret < 0) return ret;
    }
    return 0;
}

struct dance {
    bool used, pressed, decided;
    uint8_t count, slot;
    struct zmk_behavior_binding_event event;
    struct zmk_behavior_binding action;
    uint8_t config[SLOT_BYTES]; /* Immutable until this press is released. */
    int64_t deadline;
    struct k_work_delayable timer;
};
static struct dance active[MAX_ACTIVE];
static struct dance *pending;
struct captured {
    bool keycode;
    union {
        struct zmk_position_state_changed_event position;
        struct zmk_keycode_state_changed_event code;
    } event;
};
static struct captured captured[MAX_CAPTURED];
static uint8_t captured_count;
static bool replaying;
static void flush(void);
static void finish(struct dance *dance, bool hold, int64_t timestamp) {
    if (!dance || dance->decided) return;
    k_work_cancel_delayable(&dance->timer);
    dance->decided = true;
    if (pending == dance) pending = NULL;
    const uint8_t *data = dance->config + 30 + ((dance->count - 1) * 2 + hold) * 10;
    if (hold && sys_get_le16(data) == UNASSIGNED) data -= 10;
    dance->action = decode_binding(data);
    struct zmk_behavior_binding_event event = dance->event;
    event.timestamp = timestamp;
    if (dance->action.behavior_dev) zmk_behavior_invoke_binding(&dance->action, event, true);
    if (!dance->pressed) {
        if (dance->action.behavior_dev) zmk_behavior_invoke_binding(&dance->action, event, false);
        dance->used = false;
    }
    flush();
}
static void flush(void) {
    if (replaying) return;
    replaying = true;
    while (captured_count && !pending) {
        struct captured next = captured[0];
        memmove(captured, captured + 1, --captured_count * sizeof(captured[0]));
        if (next.keycode) ZMK_EVENT_RELEASE(next.event.code);
        else ZMK_EVENT_RELEASE(next.event.position);
    }
    replaying = false;
}
static struct dance *find(uint32_t position) {
    for (int i = 0; i < MAX_ACTIVE; i++)
        if (active[i].used && active[i].event.position == position) return &active[i];
    return NULL;
}
static void timeout(struct k_work *work) {
    struct dance *dance = CONTAINER_OF(k_work_delayable_from_work(work), struct dance, timer);
    if (!dance->used || dance->decided) return;
    /* A cancelled timer may already be executing: respect the new deadline. */
    int64_t left = dance->deadline - k_uptime_get();
    if (left > 0) { k_work_reschedule(&dance->timer, K_MSEC(left)); return; }
    finish(dance, dance->pressed, dance->deadline);
}
static void schedule(struct dance *dance, int64_t timestamp) {
    dance->deadline = timestamp + sys_get_le16(dance->config + 4);
    k_work_reschedule(&dance->timer, K_MSEC(MAX(1, dance->deadline - k_uptime_get())));
}
static int pressed(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event) {
    if (binding->param1 >= CONFIG_ZMK_RUNTIME_TAP_DANCE_COUNT) return ZMK_BEHAVIOR_OPAQUE;
    struct dance *dance = find(event.position);
    if (!dance) {
        for (int i = 0; i < MAX_ACTIVE; i++) if (!active[i].used) { dance = &active[i]; break; }
        if (!dance) return ZMK_BEHAVIOR_OPAQUE;
        k_mutex_lock(&config_lock, K_FOREVER);
        memcpy(dance->config, slots[binding->param1], SLOT_BYTES);
        k_mutex_unlock(&config_lock);
        if (!dance->config[0] || validate(dance->config)) return ZMK_BEHAVIOR_OPAQUE;
        dance->used = true; dance->decided = false; dance->count = 0;
        dance->event = event; dance->slot = binding->param1;
        pending = dance;
    }
    dance->pressed = true;
    dance->count = MIN(dance->count + 1, dance->config[1]);
    schedule(dance, event.timestamp);
    return ZMK_BEHAVIOR_OPAQUE;
}
static int released(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    struct dance *dance = find(event.position);
    if (!dance) return ZMK_BEHAVIOR_OPAQUE;
    dance->pressed = false;
    if (dance->decided) {
        if (dance->action.behavior_dev) zmk_behavior_invoke_binding(&dance->action, event, false);
        dance->used = false;
    } else if (captured_count || dance->count == dance->config[1]) {
        finish(dance, false, event.timestamp);
    } else if (dance->count == 1 && sys_get_le16(dance->config + 50) == UNASSIGNED &&
               sys_get_le16(dance->config + 60) == UNASSIGNED) {
        /* No double action is configured: a released single tap is unambiguous. */
        finish(dance, false, event.timestamp);
    } else {
        /* Release must not add another full tapping term to the press deadline. */
        k_work_reschedule(&dance->timer, K_MSEC(MAX(1, dance->deadline - k_uptime_get())));
    }
    return ZMK_BEHAVIOR_OPAQUE;
}
static int listener(const zmk_event_t *event) {
    if (!pending) return ZMK_EV_EVENT_BUBBLE;
    struct zmk_position_state_changed *position = as_zmk_position_state_changed(event);
    struct zmk_keycode_state_changed *code = as_zmk_keycode_state_changed(event);
    if (position && position->timestamp >= pending->deadline) {
        finish(pending, pending->pressed, pending->deadline);
        return listener(event);
    }
    if (position && position->position == pending->event.position) return ZMK_EV_EVENT_BUBBLE;
    if (position) {
        bool other_was_pressed = false;
        for (int i = 0; i < captured_count; i++) {
            if (!captured[i].keycode && captured[i].event.position.data.position == position->position &&
                captured[i].event.position.data.state) other_was_pressed = true;
        }
        if (!pending->pressed || (position->state && pending->config[2] == HOLD_PREFERRED) ||
            (!position->state && pending->config[2] == BALANCED && other_was_pressed)) {
            finish(pending, pending->pressed, position->timestamp);
            return listener(event);
        }
    }
    if (!position && !code) return ZMK_EV_EVENT_BUBBLE;
    if (captured_count == MAX_CAPTURED) {
        /* Decide instead of losing a press/release when the bounded queue fills. */
        finish(pending, pending->pressed, k_uptime_get());
        return listener(event);
    }
    struct captured *entry = &captured[captured_count++];
    entry->keycode = code != NULL;
    if (code) entry->event.code = copy_raised_zmk_keycode_state_changed(code);
    else entry->event.position = copy_raised_zmk_position_state_changed(position);
    return ZMK_EV_EVENT_CAPTURED;
}
ZMK_LISTENER(runtime_tap_dance, listener);
ZMK_SUBSCRIPTION(runtime_tap_dance, zmk_position_state_changed);
ZMK_SUBSCRIPTION(runtime_tap_dance, zmk_keycode_state_changed);

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata values[] = {{
    .display_name = "Tap Dance slot", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
    .range = {.min = 0, .max = CONFIG_ZMK_RUNTIME_TAP_DANCE_COUNT - 1}}};
static const struct behavior_parameter_metadata_set sets[] = {{
    .param1_values = values, .param1_values_len = ARRAY_SIZE(values)}};
static const struct behavior_parameter_metadata metadata = {.sets = sets, .sets_len = ARRAY_SIZE(sets)};
#endif
static int init(const struct device *dev) {
    ARG_UNUSED(dev);
    for (int i = 0; i < ARRAY_SIZE(slots); i++) defaults(slots[i]);
    for (int i = 0; i < MAX_ACTIVE; i++) k_work_init_delayable(&active[i].timer, timeout);
    return 0;
}
static const struct behavior_driver_api api = {
    .binding_pressed = pressed, .binding_released = released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};
BEHAVIOR_DT_INST_DEFINE(0, init, NULL, NULL, NULL, POST_KERNEL,
                       CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);

static int settings_set(const char *name, size_t length, settings_read_cb read_cb, void *arg) {
    char *end;
    unsigned long index = strtoul(name, &end, 10);
    if (!*name || *end || index >= ARRAY_SIZE(slots) || length != SLOT_BYTES + 1) return -EINVAL;
    uint8_t record[SLOT_BYTES + 1];
    int ret = read_cb(arg, record, sizeof(record));
    if (ret != sizeof(record) || record[0] != 1) return -EINVAL;
    /* Behavior local IDs are assigned later during boot. Validate bindings on edit/play. */
    uint8_t *slot = record + 1;
    if (slot[0] > 1 || slot[1] < 1 || slot[1] > MAX_TAPS || slot[2] > TAP_PREFERRED ||
        slot[3] || sys_get_le16(slot + 4) < 50 || sys_get_le16(slot + 4) > 2000 ||
        !memchr(slot + 6, 0, 24)) return -EINVAL;
    memcpy(slots[index], slot, SLOT_BYTES);
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(runtime_td, "runtime_td", NULL, settings_set, NULL, NULL);

#if IS_ENABLED(CONFIG_ZMK_RUNTIME_TAP_DANCE_STUDIO_RPC)
static uint8_t response[6 + SLOT_BYTES];
/* Native custom RPC requests carry at most 30 bytes. Stage edits in bounded
 * chunks and publish only a complete CRC-verified durable transaction. */
static uint8_t staged[SLOT_BYTES], staged_index = UINT8_MAX, received;
static uint32_t staged_crc;
static int64_t staged_deadline;
static bool encode(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    ARG_UNUSED(arg);
    return pb_encode_tag_for_field(stream, field) && pb_encode_string(stream, response, sizeof(response));
}
static bool rpc(const zmk_custom_CallRequest *request, pb_callback_t *output) {
    const uint8_t *data = request->payload.bytes;
    int ret = 0;
    uint8_t index = request->payload.size >= 3 ? data[2] : 0;
    memset(response, 0, sizeof(response));
    response[0] = 1; response[5] = ARRAY_SIZE(slots);
    if (request->payload.size < 3 || data[0] != 1 || data[1] > 4 || index >= ARRAY_SIZE(slots) ||
        (data[1] == 1 && request->payload.size != 7) ||
        (data[1] == 3 && (request->payload.size < 5 || request->payload.size > 30)) ||
        (data[1] != 1 && data[1] != 3 && request->payload.size != 3)) ret = -EINVAL;
    else {
        uint8_t candidate[SLOT_BYTES];
        if (data[1] == 1) {
            staged_index = index; received = 0;
            staged_crc = sys_get_le32(data + 3);
            staged_deadline = k_uptime_get() + 30000;
        } else if (data[1] == 3) {
            size_t length = request->payload.size - 4;
            if (staged_index != index || k_uptime_get() > staged_deadline || data[3] != received ||
                received + length > SLOT_BYTES) ret = -EINVAL;
            else {
                memcpy(staged + received, data + 4, length);
                received += length;
                staged_deadline = k_uptime_get() + 30000;
            }
        } else if (data[1] == 2 || data[1] == 4) {
            if (data[1] == 2) defaults(candidate);
            else if (staged_index != index || received != SLOT_BYTES || k_uptime_get() > staged_deadline ||
                     crc32_ieee(staged, SLOT_BYTES) != staged_crc) ret = -EINVAL;
            else memcpy(candidate, staged, SLOT_BYTES);
            if (data[1] == 4 || staged_index == index) staged_index = UINT8_MAX;
            if (!ret) ret = validate(candidate);
            if (!ret) {
                char key[24]; snprintf(key, sizeof(key), "runtime_td/%u", index);
                uint8_t record[SLOT_BYTES + 1] = {1};
                memcpy(record + 1, candidate, SLOT_BYTES);
                /* Publish RAM state only after durable storage succeeds. */
                ret = settings_save_one(key, record, sizeof(record));
                if (!ret) {
                    k_mutex_lock(&config_lock, K_FOREVER);
                    memcpy(slots[index], candidate, SLOT_BYTES);
                    k_mutex_unlock(&config_lock);
                }
            }
        }
        k_mutex_lock(&config_lock, K_FOREVER);
        memcpy(response + 6, slots[index], SLOT_BYTES);
        k_mutex_unlock(&config_lock);
    }
    sys_put_le32((uint32_t)ret, response + 1);
    output->funcs.encode = encode; output->arg = NULL;
    return true;
}
static struct zmk_rpc_custom_subsystem_meta rpc_meta = {.security = ZMK_STUDIO_RPC_HANDLER_SECURED};
ZMK_RPC_CUSTOM_SUBSYSTEM(zitaotech_runtime_tap_dance, &rpc_meta, rpc);
#endif
