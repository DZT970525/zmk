/*
 * QMK style bootmagic: hold a key while the keyboard powers up and it boots
 * into the UF2 bootloader instead of starting ZMK.
 *
 * Copyright (c) 2026 ZitaoTech
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zmk/bootloader_feedback.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/matrix_transform.h>
#include <zmk/events/split_central_status_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

#include <zitaotech/zmk/bootmagic.h>

#if defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_RIGHT)
#define SETTINGS_PREFIX "zitaotech_bootmagic"
#else
#define SETTINGS_PREFIX "unified_bootmagic"
#endif

/* This half's own matrix: both halves name their kscan "kscan0", and the
 * transform carries the column offset, so a local (row, column) maps to the
 * same keymap position the central would use. */
#define BM_KSCAN DT_NODELABEL(kscan0)
/* The transform table is only *defined* in matrix_transform.c; the extern is
 * what makes the symbol visible here.  (Its column offset is what turns this
 * half's local row/column into the global keymap position.) */
ZMK_MATRIX_TRANSFORM_EXTERN(DT_NODELABEL(default_transform));
#define BM_TRANSFORM ZMK_MATRIX_TRANSFORM_T_FOR_NODE(DT_NODELABEL(default_transform))

#define BM_ROW_SPEC(i, _) GPIO_DT_SPEC_GET_BY_IDX(BM_KSCAN, row_gpios, i),
static const struct gpio_dt_spec bm_rows[] = {
    LISTIFY(DT_PROP_LEN(BM_KSCAN, row_gpios), BM_ROW_SPEC, ())};

#define BM_COL_SPEC(i, _) GPIO_DT_SPEC_GET_BY_IDX(BM_KSCAN, col_gpios, i),
static const struct gpio_dt_spec bm_cols[] = {
    LISTIFY(DT_PROP_LEN(BM_KSCAN, col_gpios), BM_COL_SPEC, ())};

static atomic_t key_position = ATOMIC_INIT(CONFIG_ZITAOTECH_BOOTMAGIC_DEFAULT_KEY);
#if (defined(CONFIG_BOARD_KEYPOINT_DONGLE) || defined(CONFIG_BOARD_SOFLE_DONGLE))
static atomic_t left_remote_key = ATOMIC_INIT(ZITAOTECH_BOOTMAGIC_KEY_UNKNOWN);
#endif
static atomic_t remote_key = ATOMIC_INIT(ZITAOTECH_BOOTMAGIC_KEY_UNKNOWN);

ZMK_EVENT_IMPL(zitaotech_bootmagic_key_changed);
ZMK_EVENT_IMPL(zitaotech_bootmagic_mirror);
ZMK_EVENT_IMPL(zitaotech_bootmagic_query);

ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zitaotech_bootmagic_mirror, bm, source);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zitaotech_bootmagic_mirror, bm, source);
ZMK_RELAY_EVENT_HANDLE(zitaotech_bootmagic_mirror, bm, source);

/* The central asks, the peripheral answers; see request_remote_key() below. */
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zitaotech_bootmagic_query, bmq, );
ZMK_RELAY_EVENT_HANDLE(zitaotech_bootmagic_query, bmq, );

int32_t zitaotech_bootmagic_key_position(void) { return (int32_t)atomic_get(&key_position); }

int32_t zitaotech_bootmagic_remote_key(void) { return (int32_t)atomic_get(&remote_key); }

int32_t zitaotech_bootmagic_studio_left_key(void) {
#if (defined(CONFIG_BOARD_KEYPOINT_DONGLE) || defined(CONFIG_BOARD_SOFLE_DONGLE))
    return atomic_get(&left_remote_key);
#else
    return zitaotech_bootmagic_key_position();
#endif
}

bool zitaotech_bootmagic_peripheral_connected(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* The central already tracks the split link, so just ask it. */
    return zmk_split_central_peripherals_connected();
#else
    return true;
#endif
}

__weak void zitaotech_bootmagic_state_changed(void) {}

static atomic_t notify_suppressed = ATOMIC_INIT(0);

void zitaotech_bootmagic_suppress_notifications(bool suppress) {
    atomic_set(&notify_suppressed, suppress ? 1 : 0);
}

static void mark_changed(void) {
    if (!atomic_get(&notify_suppressed)) {
        zitaotech_bootmagic_state_changed();
    }
}

static void raise_mirror(int32_t position) {
    raise_zitaotech_bootmagic_mirror((struct zitaotech_bootmagic_mirror){
        .position = position,
        .source = ZMK_RELAY_EVENT_SOURCE_SELF,
#if (defined(CONFIG_BOARD_KEYPOINT_DONGLE_LEFT) || defined(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT))
        .side = 0,
#elif (defined(CONFIG_BOARD_KEYPOINT_DONGLE_RIGHT) || defined(CONFIG_BOARD_SOFLE_DONGLE_RIGHT) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_RIGHT))
        .side = 1,
#elif (defined(CONFIG_BOARD_KEYPOINT_DONGLE) || defined(CONFIG_BOARD_SOFLE_DONGLE))
        .side = 2,
#endif
    });
}

/*
 * Reads the configured key straight off the matrix pins.
 *
 * This board is wired col2row: the current enters through the column pin, runs
 * through the diode into the row pin, so the column is the one that gets
 * driven and the row is the one that is read (matching row-gpios carrying the
 * pull downs).  The scan driver re-drives its own side on its next pass, so the
 * momentary interference is harmless.
 */
static bool bootmagic_key_held(int32_t position) {
    for (int r = 0; r < ARRAY_SIZE(bm_rows); r++) {
        for (int c = 0; c < ARRAY_SIZE(bm_cols); c++) {
            if (zmk_matrix_transform_row_column_to_position(BM_TRANSFORM, r, c) != position) {
                continue;
            }

            /*
             * The matrix driver is interrupt driven: while it waits for a key
             * it arms the row interrupts and drives every column active, so a
             * single key cannot be read out of that state - and reconfiguring
             * the row pin would clear its interrupt, leaving that whole row
             * dead until some other key started the next scan (that is exactly
             * what made the thumb row unusable right after power-on).
             *
             * Stop the driver instead, drive just this key's column, and start
             * it again: its own start-up scan re-arms the interrupts and puts
             * the pins back the way they were.
             */
            const struct device *kscan = DEVICE_DT_GET(BM_KSCAN);
            const bool kscan_ready = device_is_ready(kscan);

            if (kscan_ready) {
                kscan_disable_callback(kscan);
            } else {
                /* Nothing has set the matrix up yet, so make the row readable
                 * here instead of trusting a floating pin. */
                gpio_pin_configure_dt(&bm_rows[r], GPIO_INPUT | GPIO_PULL_DOWN);
            }
            gpio_pin_configure_dt(&bm_cols[c], GPIO_OUTPUT_ACTIVE);
            k_busy_wait(100);
            bool pressed = gpio_pin_get_dt(&bm_rows[r]) > 0;
            gpio_pin_configure_dt(&bm_cols[c], GPIO_OUTPUT_INACTIVE);
            if (kscan_ready) {
                kscan_enable_callback(kscan);
            }

            LOG_DBG("bootmagic: key %d is %s", (int)position, pressed ? "held" : "released");
            return pressed;
        }
    }

    LOG_WRN("bootmagic: key %d is not on this half", (int)position);
    return false;
}

/* System-off wake runs startup again. Capture and consume the reset flags
 * before application startup, retaining the decision for the early check and
 * the later settings commit. Clearing avoids suppressing a future cold boot
 * because Nordic reset flags accumulate until explicitly cleared. */
static bool bootmagic_sleep_wake;
static int bootmagic_capture_reset(void) {
    uint32_t cause = 0;
    if (hwinfo_get_reset_cause(&cause) == 0) {
        bootmagic_sleep_wake = (cause & RESET_LOW_POWER_WAKE) != 0;
        hwinfo_clear_reset_cause();
    }
    return 0;
}
SYS_INIT(bootmagic_capture_reset, PRE_KERNEL_1, 99);

static void bootmagic_check(void) {
    if (bootmagic_sleep_wake) return;
    int32_t position = zitaotech_bootmagic_key_position();
    if (position < 0) {
        return;
    }

    if (!bootmagic_key_held(position)) {
        return;
    }

    LOG_INF("bootmagic: key %d held at power-up, entering the bootloader", (int)position);

    if (bootmode_set(BOOT_MODE_TYPE_BOOTLOADER) < 0) {
        LOG_ERR("bootmagic: could not set the boot mode");
        return;
    }

    zmk_bootloader_feedback_reboot(SYS_REBOOT_COLD);
}

int zitaotech_bootmagic_set_key(bool peripheral, int32_t position) {
    if (position < -1) {
        return -EINVAL;
    }

#if (defined(CONFIG_BOARD_KEYPOINT_DONGLE) || defined(CONFIG_BOARD_SOFLE_DONGLE))
    if (!zitaotech_bootmagic_peripheral_connected()) return -ENOTCONN;
    raise_zitaotech_bootmagic_mirror((struct zitaotech_bootmagic_mirror){
        .position = position, .source = ZMK_RELAY_EVENT_SOURCE_SELF, .side = peripheral ? 1 : 0});
    atomic_set(peripheral ? &remote_key : &left_remote_key, position);
    mark_changed();
    return 0;
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (peripheral) {
        /* The setting lives on the other half, so refuse rather than store
         * something that would never reach it. */
        if (!zitaotech_bootmagic_peripheral_connected()) {
            return -ENOTCONN;
        }
        /* Store it on the other half: it is that half which reads the setting
         * before its own keyboard starts. */
        /* A command sent by the left master targets the right half. Its side
         * is not the sender's own side used by raise_mirror() for reports. */
        raise_zitaotech_bootmagic_mirror((struct zitaotech_bootmagic_mirror){
            .position=position, .source=ZMK_RELAY_EVENT_SOURCE_SELF,
#if defined(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) || defined(CONFIG_BOARD_KEYPOINT_DONGLE_LEFT)
            .side=1,
#endif
        });
        atomic_set(&remote_key, (atomic_val_t)position);
        mark_changed();
        return 0;
    }
#else
    ARG_UNUSED(peripheral);
#endif

    atomic_set(&key_position, (atomic_val_t)position);
    settings_save_one(SETTINGS_PREFIX "/key", &position, sizeof(position));
    raise_mirror(position);
    mark_changed();
    return 0;
}

static int bootmagic_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "key", &next) && !next) {
        if (len != sizeof(int32_t)) {
            return -EINVAL;
        }
        int32_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        atomic_set(&key_position, (atomic_val_t)value);
        return 0;
    }

    return -ENOENT;
}

/*
 * The fallback check, run from the settings commit.
 *
 * main() calls settings_load() once everything else is up, which on this board
 * means after the bluetooth stack - so waiting for it made people hold the key
 * for the whole boot.  The early check below normally beats it to the punch and
 * this one stays as a second chance in case that read could not happen.
 */
static int bootmagic_settings_commit(void) {
    bootmagic_check();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(zitaotech_bootmagic, SETTINGS_PREFIX, NULL, bootmagic_settings_set,
                               bootmagic_settings_commit, NULL);

/*
 * As early as the pins can be trusted.
 *
 * The matrix pins are not configured by the kscan driver's init: with
 * CONFIG_PM_DEVICE the driver comes up suspended and only calls
 * kscan_matrix_setup_pins() when something resumes it, which is what
 * zmk_physical_layouts_init() does at APPLICATION/CONFIG_APPLICATION_INIT_PRIORITY.
 * Checking any earlier reads floating row pins, so this runs just after that -
 * still before main()'s full settings_load(), so the stored key is read on its
 * own and the key only has to be held while the keyboard starts.
 */
static int bootmagic_early_check(void) {
    /* Reset firmware uses factory keys and checks before erasing flash. */
    if (!IS_ENABLED(CONFIG_ZMK_SETTINGS_RESET_ON_START) && settings_subsys_init() == 0) {
        /* Just this subtree; the rest is loaded by main() as usual. */
        uint32_t settings_started = k_uptime_get_32();
        settings_load_subtree(SETTINGS_PREFIX);
        LOG_INF("boot timing: Bootmagic settings %u ms, finished at %u ms",
                k_uptime_get_32() - settings_started, k_uptime_get_32());
    }

    bootmagic_check();
    return 0;
}

/* Just after CONFIG_APPLICATION_INIT_PRIORITY (90), where the layouts init
 * resumed the matrix.  The priority has to be a plain literal: SYS_INIT bakes
 * it into the init section name, so an arithmetic expression would not
 * stringify to a number. */
SYS_INIT(bootmagic_early_check, APPLICATION, 95);

/*
 * Mirror handling.  The central sends the peripheral's key over, the peripheral
 * reports its own back so Studio can show both halves.
 */
static int bootmagic_mirror_listener(const zmk_event_t *eh) {
    struct zitaotech_bootmagic_mirror *ev = as_zitaotech_bootmagic_mirror(eh);
    if (ev == NULL || ev->source == ZMK_RELAY_EVENT_SOURCE_SELF) {
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* Came from the other half: remember it for the UI. */
#if (defined(CONFIG_BOARD_KEYPOINT_DONGLE) || defined(CONFIG_BOARD_SOFLE_DONGLE))
    if (ev->side > 1) return ZMK_EV_EVENT_BUBBLE;
    atomic_set(ev->side == 0 ? &left_remote_key : &remote_key, ev->position);
#else
    atomic_set(&remote_key, (atomic_val_t)ev->position);
#endif
#else
#if (defined(CONFIG_BOARD_KEYPOINT_DONGLE_LEFT) || defined(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT))
    if (ev->side != 0) return ZMK_EV_EVENT_BUBBLE;
#elif (defined(CONFIG_BOARD_KEYPOINT_DONGLE_RIGHT) || defined(CONFIG_BOARD_SOFLE_DONGLE_RIGHT) || defined(CONFIG_BOARD_ZITAOTECH_KEYPOINT_RIGHT))
    if (ev->side != 1) return ZMK_EV_EVENT_BUBBLE;
#endif
    /* Came from the central: this half owns the setting, so store it. */
    atomic_set(&key_position, (atomic_val_t)ev->position);
    settings_save_one(SETTINGS_PREFIX "/key", &ev->position, sizeof(ev->position));
#endif

    mark_changed();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zitaotech_bootmagic_mirror_apply, bootmagic_mirror_listener);
ZMK_SUBSCRIPTION(zitaotech_bootmagic_mirror_apply, zitaotech_bootmagic_mirror);

/*
 * Asking the other half what it is set to.
 *
 * A peripheral reporting on its own does not survive: at its power-up - and
 * even when the link comes up - the central has usually not subscribed to the
 * relay notification characteristic yet, so the report is dropped.  The central
 * therefore asks once the link is up, and asks again a few times in case the
 * GATT handles were not discovered on the first try.
 */
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

#define BM_QUERY_FIRST_MS 800
#define BM_QUERY_RETRY_MS 700
#define BM_QUERY_ATTEMPTS 5
#define BM_QUERY_COOLDOWN_MS 8000

static atomic_t bootmagic_query_attempts = ATOMIC_INIT(0);
static int64_t bootmagic_query_last_ms;

static void bootmagic_query_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(bootmagic_query_work, bootmagic_query_work_handler);

static void bootmagic_query_work_handler(struct k_work *work) {
    if (!zitaotech_bootmagic_peripheral_connected() ||
        ((zitaotech_bootmagic_remote_key() != ZITAOTECH_BOOTMAGIC_KEY_UNKNOWN)
#if (defined(CONFIG_BOARD_KEYPOINT_DONGLE) || defined(CONFIG_BOARD_SOFLE_DONGLE))
         && atomic_get(&left_remote_key) != ZITAOTECH_BOOTMAGIC_KEY_UNKNOWN
#endif
        )) {
        atomic_set(&bootmagic_query_attempts, 0);
        return;
    }

    raise_zitaotech_bootmagic_query((struct zitaotech_bootmagic_query){.reserved = 0});

    if (atomic_inc(&bootmagic_query_attempts) < BM_QUERY_ATTEMPTS) {
        k_work_reschedule(&bootmagic_query_work, K_MSEC(BM_QUERY_RETRY_MS));
    } else {
        atomic_set(&bootmagic_query_attempts, 0);
        bootmagic_query_last_ms = k_uptime_get();
    }
}

void zitaotech_bootmagic_request_remote_key(void) {
    if (atomic_get(&bootmagic_query_attempts) != 0) {
        /* Already asking; a second sequence would only duplicate traffic. */
        return;
    }

    int64_t now = k_uptime_get();
    if (now - bootmagic_query_last_ms < BM_QUERY_COOLDOWN_MS) {
        return;
    }

    k_work_reschedule(&bootmagic_query_work, K_MSEC(BM_QUERY_FIRST_MS));
}

#else

void zitaotech_bootmagic_request_remote_key(void) {}

#endif

/* The peripheral answers the question with its current setting. */
static int bootmagic_query_listener(const zmk_event_t *eh) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    raise_mirror(zitaotech_bootmagic_key_position());
#endif
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zitaotech_bootmagic_query_apply, bootmagic_query_listener);
ZMK_SUBSCRIPTION(zitaotech_bootmagic_query_apply, zitaotech_bootmagic_query);

/*
 * "Is the other half there?" is not a heartbeat: the split transport tells us
 * when the link comes up or drops.  Both halves turn that into a state push, so
 * Studio follows the hardware instead of only refreshing when the page loads.
 */
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static void bootmagic_link_work_handler(struct k_work *work) { mark_changed(); }

K_WORK_DELAYABLE_DEFINE(bootmagic_link_work, bootmagic_link_work_handler);

static int bootmagic_link_listener(const zmk_event_t *eh) {
    /* The other half's setting is what we are usually missing right after a
     * reconnect, so ask for it and let both changes land in one push. */
    zitaotech_bootmagic_request_remote_key();
    k_work_reschedule(&bootmagic_link_work, K_MSEC(150));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zitaotech_bootmagic_link, bootmagic_link_listener);
ZMK_SUBSCRIPTION(zitaotech_bootmagic_link, zmk_split_central_status_changed);

#elif IS_ENABLED(CONFIG_ZMK_SPLIT)

static int bootmagic_link_listener(const zmk_event_t *eh) {
    struct zmk_split_peripheral_status_changed *ev = as_zmk_split_peripheral_status_changed(eh);
    if (ev != NULL && ev->connected) {
        /* The report we sent at power-up may have gone nowhere if the central
         * was not listening yet, so send the current key again now. */
        raise_mirror(zitaotech_bootmagic_key_position());
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zitaotech_bootmagic_link, bootmagic_link_listener);
ZMK_SUBSCRIPTION(zitaotech_bootmagic_link, zmk_split_peripheral_status_changed);

#endif

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Tell the central what this half is configured to do. */
static int bootmagic_report_init(void) {
    raise_mirror(zitaotech_bootmagic_key_position());
    return 0;
}

SYS_INIT(bootmagic_report_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif
