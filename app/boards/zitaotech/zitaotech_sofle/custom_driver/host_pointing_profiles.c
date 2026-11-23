/* SPDX-License-Identifier: MIT
 * Stable storage IDs: BLE 1..4 = 0..3. USB shares the selected BLE slot.
 * Flash is written only by the debounced worker, never by endpoint listeners.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <zmk/pointing_profiles.h>
#include <zmk/event_manager.h>
#include <zmk/events/split_central_status_changed.h>
#include <zitaotech/zmk/trackpoint.h>
#include <zitaotech/zmk/trackpad.h>
#include <zitaotech/zmk/pointer_curve.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static atomic_t epoch;
uint32_t zmk_pointing_profile_epoch(void) { return (uint32_t)atomic_get(&epoch); }
struct sofle_pointing_profile_reset { uint8_t source; };
ZMK_EVENT_DECLARE(sofle_pointing_profile_reset);
ZMK_EVENT_IMPL(sofle_pointing_profile_reset);
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(sofle_pointing_profile_reset, spr, source);
ZMK_RELAY_EVENT_HANDLE(sofle_pointing_profile_reset, spr, source);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static void reset_motion(void) {
    atomic_inc(&epoch);
    raise_sofle_pointing_profile_reset((struct sofle_pointing_profile_reset){
        .source = ZMK_RELAY_EVENT_SOURCE_SELF});
}
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/endpoints.h>
#include <zmk/ble.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/ble_active_profile_changed.h>

/* Explicit uint16_t wire layout, no compiler padding or persisted bools. */
struct profile { uint16_t version, tp_dpi, tp_on, tp_mult, pad_dpi, pad_on, pad_mult;
    uint16_t curve_on, vmax, exponent, minimum, maximum;
    uint16_t rise_rate, release_decay; };
static struct profile profiles[4];
static uint16_t stored_rates[4][2];
static uint8_t rates_loaded;
static uint8_t loaded, dirty;
static uint32_t generation[4];
static int active;
static bool ready, applying;
K_MUTEX_DEFINE(profile_lock);
static void persist(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(profile_save_work, persist);
static void notify(struct k_work *work) {
    ARG_UNUSED(work);
    zitaotech_trackpoint_state_changed();
    zitaotech_trackpad_state_changed();
}
K_WORK_DEFINE(profile_notify_work, notify);

static struct profile capture(void) {
    struct zitaotech_pointer_curve c;
    zitaotech_pointer_curve_get(&c);
    return (struct profile){2, zitaotech_trackpoint_dpi_index(),
        zitaotech_trackpoint_burst_enabled(), zitaotech_trackpoint_burst_multiplier(),
        zitaotech_trackpad_dpi_index(), zitaotech_trackpad_burst_enabled(),
        zitaotech_trackpad_burst_multiplier(), c.enabled, c.vmax, c.exponent,
        c.minimum, c.maximum, zitaotech_pointer_rise_rate_get(),
        zitaotech_pointer_release_decay_get()};
}
static bool valid(const struct profile *p) {
    struct zitaotech_pointer_curve c = {p->vmax,p->exponent,p->minimum,p->maximum,p->curve_on};
    return p->version == 2 && p->rise_rate >= 1 && p->rise_rate <= 100 &&
        p->release_decay >= 1 && p->release_decay <= 100 &&
        p->tp_dpi < ZITAOTECH_TRACKPOINT_DPI_LEVELS &&
        p->pad_dpi < ZITAOTECH_TRACKPAD_DPI_LEVELS && p->tp_on <= 1 && p->pad_on <= 1 &&
        p->curve_on <= 1 && p->tp_mult >= 1 && p->tp_mult <= CONFIG_ZITAOTECH_TRACKPOINT_BURST_MAX &&
        p->pad_mult >= 1 && p->pad_mult <= CONFIG_ZITAOTECH_TRACKPAD_BURST_MAX &&
        zitaotech_pointer_curve_valid(&c);
}
static void remember(void) {
    struct profile p = capture();
    if (memcmp(&profiles[active], &p, sizeof(p))) {
        profiles[active] = p;
        generation[active]++;
        dirty |= BIT(active);
        k_work_reschedule(&profile_save_work, K_MSEC(1000));
    }
}
void zmk_pointing_profile_begin(void) { k_mutex_lock(&profile_lock, K_FOREVER); }
void zmk_pointing_profile_end(void) {
    if (ready && !applying) remember();
    k_mutex_unlock(&profile_lock);
}
static int selected_profile(void) {
    /* Studio uses USB to edit the selected slot, even when HID also uses USB. */
    int index = zmk_ble_active_profile_index();
    return CLAMP(index, 0, 3);
}
static void apply(int index) {
    active = index;
    applying = true;
    const struct profile *p = &profiles[index];
    struct zitaotech_pointer_curve c = {p->vmax,p->exponent,p->minimum,p->maximum,p->curve_on};
    zitaotech_trackpoint_suppress_notifications(true);
    zitaotech_trackpad_suppress_notifications(true);
    zitaotech_trackpoint_set_dpi_index(p->tp_dpi);
    zitaotech_trackpoint_set_burst(p->tp_on, p->tp_mult);
    zitaotech_trackpad_set_dpi_index(p->pad_dpi);
    zitaotech_trackpad_set_burst(p->pad_on, p->pad_mult);
    zitaotech_pointer_curve_set(&c);
    zitaotech_pointer_rise_rate_set(p->rise_rate);
    zitaotech_pointer_release_decay_set(p->release_decay);
    zitaotech_trackpoint_suppress_notifications(false);
    zitaotech_trackpad_suppress_notifications(false);
    applying = false;
    reset_motion();
    k_work_submit(&profile_notify_work);
}
void zmk_pointing_profiles_start(void) {
    k_mutex_lock(&profile_lock, K_FOREVER);
    struct profile legacy = capture();
    for (int i = 0; i < 4; i++) {
        if (!(loaded & BIT(i))) { profiles[i] = legacy; dirty |= BIT(i); }
        if (rates_loaded & BIT(i)) {
            profiles[i].rise_rate = stored_rates[i][0];
            profiles[i].release_decay = stored_rates[i][1];
        }
    }
    ready = true;
    apply(selected_profile());
    if (dirty) k_work_reschedule(&profile_save_work, K_MSEC(1000));
    k_mutex_unlock(&profile_lock);
}
static void persist(struct k_work *work) {
    ARG_UNUSED(work);
    for (int i = 0; i < 4; i++) {
        k_mutex_lock(&profile_lock, K_FOREVER);
        if (!(dirty & BIT(i))) { k_mutex_unlock(&profile_lock); continue; }
        struct profile p = profiles[i];
        uint32_t gen = generation[i];
        k_mutex_unlock(&profile_lock);
        char key[] = "sofle_pointing/0";
        key[sizeof(key)-2] += i;
        /* Preserve the version-1 record byte-for-byte for firmware rollback. */
        p.version = 1;
        int rc = settings_save_one(key, &p, sizeof(p) - 2 * sizeof(uint16_t));
        char rates_key[] = "sofle_rates/0";
        rates_key[sizeof(rates_key)-2] += i;
        const uint16_t rates[] = {p.rise_rate, p.release_decay};
        int rates_rc = settings_save_one(rates_key, rates, sizeof(rates));
        k_mutex_lock(&profile_lock, K_FOREVER);
        if (!rc && !rates_rc && gen == generation[i]) dirty &= ~BIT(i);
        k_mutex_unlock(&profile_lock);
    }
    k_mutex_lock(&profile_lock, K_FOREVER);
    if (dirty) k_work_reschedule(&profile_save_work, K_SECONDS(2));
    k_mutex_unlock(&profile_lock);
}
static int settings_set(const char *name, size_t len, settings_read_cb read_cb, void *arg) {
    /* Ignore the separate USB record from the first test build. */
    if (!strcmp(name, "4")) return 0;
    if (name[0] < '0' || name[0] > '3' || name[1]) return -ENOENT;
    struct profile p = {0};
    const size_t old_size = sizeof(p) - 2 * sizeof(uint16_t);
    if (len != sizeof(p) && len != old_size) return -EINVAL;
    int rc = read_cb(arg, &p, len);
    if (rc != len) return -EINVAL;
    if (len == old_size && p.version == 1) {
        p.version = 2;
        p.rise_rate = ZITAOTECH_POINTER_RISE_RATE_DEFAULT;
        p.release_decay = ZITAOTECH_POINTER_RELEASE_DECAY_DEFAULT;
    }
    if (!valid(&p)) return -EINVAL;
    int i = name[0] - '0'; profiles[i] = p; loaded |= BIT(i);
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(sofle_pointing, "sofle_pointing", NULL, settings_set, NULL, NULL);
static int rates_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *arg) {
    if (name[0] < '0' || name[0] > '3' || name[1]) return -ENOENT;
    uint16_t rates[2];
    if (len != sizeof(rates)) return -EINVAL;
    int rc = read_cb(arg, rates, sizeof(rates));
    if (rc != sizeof(rates) || rates[0] < 1 || rates[0] > 100 ||
        rates[1] < 1 || rates[1] > 100) return -EINVAL;
    int i = name[0] - '0';
    stored_rates[i][0] = rates[0];
    stored_rates[i][1] = rates[1];
    rates_loaded |= BIT(i);
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(sofle_rates, "sofle_rates", NULL, rates_settings_set, NULL, NULL);
#endif

static int profile_listener(const zmk_event_t *eh) {
    struct sofle_pointing_profile_reset *reset = as_sofle_pointing_profile_reset(eh);
    if (reset && reset->source != ZMK_RELAY_EVENT_SOURCE_SELF) atomic_inc(&epoch);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (as_zmk_endpoint_changed(eh) || as_zmk_ble_active_profile_changed(eh)) {
        k_mutex_lock(&profile_lock, K_FOREVER);
        int next = selected_profile();
        if (ready && next != active) { remember(); apply(next); }
        k_mutex_unlock(&profile_lock);
    }
    struct zmk_split_central_status_changed *split = as_zmk_split_central_status_changed(eh);
    if (split && split->connected) reset_motion();
#endif
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(sofle_pointing_profiles, profile_listener);
ZMK_SUBSCRIPTION(sofle_pointing_profiles, sofle_pointing_profile_reset);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_SUBSCRIPTION(sofle_pointing_profiles, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(sofle_pointing_profiles, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(sofle_pointing_profiles, zmk_split_central_status_changed);
#endif
