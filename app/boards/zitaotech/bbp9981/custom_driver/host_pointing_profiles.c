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
#include <zitaotech/zmk/trackpad.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);
#include <zmk/endpoints.h>
#include <zmk/ble.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/ble_active_profile_changed.h>

/* Explicit uint16_t wire layout, no compiler padding or persisted bools. */
struct profile { uint16_t version, pad_dpi, pad_on, pad_mult; };
static struct profile profiles[4];
static uint8_t loaded, dirty;
static uint32_t generation[4];
static int active;
static bool ready, applying;
K_MUTEX_DEFINE(profile_lock);
static void persist(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(profile_save_work, persist);
static void notify(struct k_work *work) {
    ARG_UNUSED(work);
    zitaotech_trackpad_state_changed();
}
K_WORK_DEFINE(profile_notify_work, notify);

static struct profile capture(void) {
    return (struct profile){1, zitaotech_trackpad_dpi_index(),
        zitaotech_trackpad_burst_enabled(), zitaotech_trackpad_burst_multiplier()};
}
static bool valid(const struct profile *p) {
    return p->version == 1 && p->pad_dpi < ZITAOTECH_TRACKPAD_DPI_LEVELS &&
        p->pad_on <= 1 && p->pad_mult >= 1 &&
        p->pad_mult <= CONFIG_ZITAOTECH_TRACKPAD_BURST_MAX;
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
    zitaotech_trackpad_suppress_notifications(true);
    zitaotech_trackpad_set_dpi_index(p->pad_dpi);
    zitaotech_trackpad_set_burst(p->pad_on, p->pad_mult);
    zitaotech_trackpad_suppress_notifications(false);
    applying = false;
    k_work_submit(&profile_notify_work);
}
void zmk_pointing_profiles_start(void) {
    k_mutex_lock(&profile_lock, K_FOREVER);
    struct profile legacy = capture();
    for (int i = 0; i < 4; i++) {
        if (!(loaded & BIT(i))) { profiles[i] = legacy; dirty |= BIT(i); }
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
        char key[] = "bbp_pointing/0";
        key[sizeof(key)-2] += i;
        int rc = settings_save_one(key, &p, sizeof(p));
        k_mutex_lock(&profile_lock, K_FOREVER);
        if (!rc && gen == generation[i]) dirty &= ~BIT(i);
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
    struct profile p;
    if (len != sizeof(p)) return -EINVAL;
    int rc = read_cb(arg, &p, sizeof(p));
    if (rc != sizeof(p) || !valid(&p)) return -EINVAL;
    int i = name[0] - '0'; profiles[i] = p; loaded |= BIT(i);
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(bbp_pointing, "bbp_pointing", NULL, settings_set, NULL, NULL);

static int profile_listener(const zmk_event_t *eh) {
    if (as_zmk_endpoint_changed(eh) || as_zmk_ble_active_profile_changed(eh)) {
        k_mutex_lock(&profile_lock, K_FOREVER);
        int next = selected_profile();
        if (ready && next != active) { remember(); apply(next); }
        k_mutex_unlock(&profile_lock);
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(bbp_pointing_profiles, profile_listener);
ZMK_SUBSCRIPTION(bbp_pointing_profiles, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(bbp_pointing_profiles, zmk_ble_active_profile_changed);
