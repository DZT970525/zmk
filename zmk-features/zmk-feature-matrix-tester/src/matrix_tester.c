/* SPDX-License-Identifier: MIT */
#include <string.h>
#include <zephyr/kernel.h>
#include <zmk/matrix.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);
#include <zmk/studio/custom.h>
#include <pb_decode.h>
#include <zmk/matrix_tester/matrix_tester.pb.h>

#define MATRIX_BYTES ((ZMK_KEYMAP_LEN + 7) / 8)
BUILD_ASSERT(ZMK_KEYMAP_LEN <= 256, "Matrix tester supports up to 256 positions");
static struct k_spinlock lock;
static uint8_t pressed[MATRIX_BYTES], tested[MATRIX_BYTES];
static uint32_t deadline, revision;
static bool active;
BUILD_ASSERT(ZMK_KEYMAP_SENSORS_LEN <= 16, "Matrix tester supports 16 encoders");
static uint16_t encoder_ticks[MAX(1, ZMK_KEYMAP_SENSORS_LEN * 2)];
static uint8_t encoder_tested[4];
static bool session_live(uint32_t now) {
    if (active && (int32_t)(now - deadline) >= 0) { active = false; }
    return active;
}
static void observe(const zmk_event_t *event) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(event);
    if (!ev || ev->position >= ZMK_KEYMAP_LEN) { return; }
    k_spinlock_key_t key = k_spin_lock(&lock);
    uint8_t mask = BIT(ev->position % 8);
    if (ev->state) {
        pressed[ev->position / 8] |= mask;
        if (session_live(k_uptime_get_32())) { tested[ev->position / 8] |= mask; }
    } else { pressed[ev->position / 8] &= ~mask; }
    k_spin_unlock(&lock, key);
}
extern int __real_zmk_event_manager_raise(zmk_event_t *event);
int __wrap_zmk_event_manager_raise(zmk_event_t *event) {
    observe(event);
    const struct zmk_sensor_event *sensor = as_zmk_sensor_event(event);
    if (sensor && sensor->sensor_index < ZMK_KEYMAP_SENSORS_LEN && sensor->channel_data_size > 0 &&
        sensor->channel_data[0].channel == SENSOR_CHAN_ROTATION) {
        struct sensor_value value = sensor->channel_data[0].value;
        if (value.val1 || value.val2) {
            unsigned direction = (value.val1 ? value.val1 : value.val2) > 0 ? 0 : 1;
            unsigned index = sensor->sensor_index * 2 + direction;
            k_spinlock_key_t key = k_spin_lock(&lock);
            if (session_live(k_uptime_get_32())) {
                encoder_ticks[index]++;
                encoder_tested[index / 8] |= BIT(index % 8);
            }
            k_spin_unlock(&lock, key);
        }
    }
    return __real_zmk_event_manager_raise(event);
}

static struct zmk_rpc_custom_subsystem_meta meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};
ZMK_RPC_CUSTOM_SUBSYSTEM(zmk__matrix_tester, &meta, handle_request);
ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(zmk__matrix_tester, zmk_matrix_tester_Response);
static bool handle_request(const zmk_custom_CallRequest *raw, pb_callback_t *encode_response) {
    zmk_matrix_tester_Request req = zmk_matrix_tester_Request_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(raw->payload.bytes, raw->payload.size);
    if (!pb_decode(&stream, zmk_matrix_tester_Request_fields, &req) ||
        req.command < 1 || req.command > 4) { return false; }
    zmk_matrix_tester_Response *out =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(zmk__matrix_tester, encode_response);
    uint32_t now = k_uptime_get_32();
    k_spinlock_key_t key = k_spin_lock(&lock);
    session_live(now);
    if (req.command == 2 || req.command == 3) {
        memcpy(tested, pressed, MATRIX_BYTES);
        memset(encoder_tested, 0, sizeof(encoder_tested));
        if (req.command == 2) { memset(encoder_ticks, 0, sizeof(encoder_ticks)); }
        active = true;
        revision++;
    } else if (req.command == 4) { active = false; }
    if (active) { deadline = now + 5000; }
    out->key_count = ZMK_KEYMAP_LEN;
    out->pressed.size = out->tested.size = MATRIX_BYTES;
    memcpy(out->pressed.bytes, pressed, MATRIX_BYTES);
    memcpy(out->tested.bytes, tested, MATRIX_BYTES);
    out->active = active;
    out->revision = revision;
    out->encoder_count = ZMK_KEYMAP_SENSORS_LEN;
    out->encoder_ticks.size = ZMK_KEYMAP_SENSORS_LEN * 4;
    out->encoder_tested.size = (ZMK_KEYMAP_SENSORS_LEN * 2 + 7) / 8;
    memcpy(out->encoder_tested.bytes, encoder_tested, out->encoder_tested.size);
    for (unsigned i = 0; i < ZMK_KEYMAP_SENSORS_LEN * 2; i++) {
        out->encoder_ticks.bytes[i * 2] = encoder_ticks[i] & 0xff;
        out->encoder_ticks.bytes[i * 2 + 1] = encoder_ticks[i] >> 8;
    }
    k_spin_unlock(&lock, key);
    return true;
}
