/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/bos.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>

#include "os_detection_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* CONFIG_USB_DEVICE_BOS (selected by this module, see Kconfig) bumps the
 * device descriptor's bcdUSB from 2.00 to 2.01 (usb_descriptor.c), which is
 * exactly what makes a host request GET_DESCRIPTOR(BOS) at all - the signal
 * this module's Windows/macOS/Linux split depends on. But Zephyr's BOS
 * header (bos.c) starts with wTotalLength=0/bNumDeviceCaps=0 and is only
 * ever corrected by usb_bos_register_cap(), which nothing in ZMK or this
 * module called before this fix - so the device answered BOS reads with a
 * spec-invalid empty descriptor (wTotalLength=0, below its own 5-byte
 * header size). Real-hardware testing found Windows aborts enumeration
 * after reading that broken BOS (3 retries, never reaches
 * SET_CONFIGURATION - see docs/windows-usb-enumeration-issue.md), while
 * Linux/macOS tolerate it. Registering the standard, always-truthful "no
 * LPM support" USB 2.0 Extension capability (bmAttributes=0, same as
 * Zephyr's own webusb sample) makes the BOS response spec-valid without
 * claiming any capability this stack doesn't actually have. Must run
 * before zmk_usb_init()'s usb_enable() (SYS_INIT APPLICATION/
 * CONFIG_ZMK_USB_INIT_PRIORITY in zmk/app/src/usb.c) - POST_KERNEL always
 * precedes APPLICATION regardless of priority number, so that's used here
 * rather than racing on priority numbers. */
USB_DEVICE_BOS_DESC_DEFINE_CAP struct usb_bos_capability_lpm bos_cap_lpm = {
    .bLength = sizeof(struct usb_bos_capability_lpm),
    .bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
    .bDevCapabilityType = USB_BOS_CAPABILITY_EXTENSION,
    .bmAttributes = 0,
};

static int os_detection_usb_bos_init(void) {
    usb_bos_register_cap((void *)&bos_cap_lpm);
    return 0;
}

SYS_INIT(os_detection_usb_bos_init, POST_KERNEL, 0);

/* All branches below are VERIFIED signatures (real captures on this
 * workspace's rig against an actual Mac, an actual Windows PC, an actual
 * Linux machine, and an actual iPhone - see docs/fingerprints.md).
 *
 * IMPORTANT HISTORY (2026-07-05, second pass): the *original* "real Windows
 * capture" this classifier was first built from was actually a capture of
 * Windows *failing* to enumerate the device at all - see
 * docs/windows-usb-enumeration-issue.md. Before the os_detection_usb_bos_init()
 * fix above, this module's BOS response was spec-invalid
 * (wTotalLength=0, below its own 5-byte header size), which made Windows
 * abort enumeration right after reading it (retried 3x, never reached
 * SET_CONFIGURATION, never read a single string descriptor - all consistent
 * with a failed enumeration, not a real "Windows doesn't read strings"
 * signal). Once the BOS response became spec-valid, a fresh real Windows
 * capture showed it reads strings (at wLength=255) exactly like Linux does -
 * so "no string descriptors" is retired as a Windows signal entirely. What
 * still discriminates a real, successfully-enumerating Windows from real
 * Linux is how *BOS itself* is read: Linux does the spec-correct two-step
 * dance (5-byte header probe, then a second read at the wTotalLength that
 * header just advertised - 12, once this module's one registered
 * capability is included), matching how it already treats every other
 * multi-byte descriptor; Windows instead reads BOS directly at wLength=255
 * in one blind shot, ignoring what the header said. This one BOS-request
 * pattern needed to move ahead of the Linux string-based check below. */
enum zmk_os zmk_os_classify_usb(const struct usb_fp_stats *stats) {
    if (stats->string_request_count == 0 && !stats->bos_requested) {
        return ZMK_OS_UNKNOWN;
    }

    bool short_probe_seen = stats->string_wlength_hist[USB_FP_WLENGTH_2] > 0;
    bool full_reread_seen = stats->string_wlength_other > 0;

    /* macOS/iOS (VERIFIED, 2026-07-05 and re-verified 2026-07-19 real
     * captures): every descriptor - device, each string, configuration - is
     * read as a short 2-byte header probe followed by a full-length re-read.
     * This is independent of the BOS fix above (Apple hosts are dispatched
     * purely on string shape, before either OS-specific BOS/Linux check
     * below is reached), so it's checked here first.
     *
     * Reported as macOS, NOT iOS: over USB, macOS and iOS are the same
     * Apple USB stack and enumerate identically - they cannot be reliably
     * distinguished here, the same way Android/ChromeOS can't be told apart
     * from desktop Linux over USB (see docs/fingerprints.md). An earlier
     * revision split iOS out on SET_FEATURE(DEVICE_REMOTE_WAKEUP) sent after
     * SET_CONFIGURATION, but a 2026-07-19 side-by-side capture of a real Mac
     * and a real iPhone showed BOTH send it - the original macOS capture had
     * simply stopped one packet early, at SET_CONFIGURATION, before macOS
     * sends it. That heuristic was therefore removed; a real Mac on USB was
     * being misdetected as iOS. iOS is still detected over BLE (ANCS/AMS -
     * see os_detection_ble.c), where the distinction is real. */
    if (short_probe_seen && full_reread_seen) {
        return ZMK_OS_MACOS;
    }

    /* Windows (RE-VERIFIED 2026-07-05, after the BOS fix above): reads BOS
     * directly at wLength=255 in one blind shot - never the 5-byte header
     * probe Linux (below) now does first. Checked before the Linux rule
     * because Windows' string-descriptor shape is otherwise
     * indistinguishable from Linux's post-fix (see the fingerprints.md
     * writeup for the exact byte sequence). Also observed but not yet
     * tracked as a signal: CONFIGURATION read directly at wLength=255
     * (Linux does a 9-then-34 two-step there too) and DEVICE_QUALIFIER
     * requested only once (Linux retries it 3x) - worth adding if the
     * BOS-length signal ever proves too coarse against a wider range of
     * real Windows versions. */
    if (stats->bos_requested && stats->bos_wlength == 255) {
        return ZMK_OS_WINDOWS;
    }

    /* Linux (RE-VERIFIED 2026-07-05, after the BOS fix above): fetches
     * every string descriptor (including index 0, the language-ID list)
     * directly at the full 255-byte buffer, with no short header probe
     * first - unchanged from before the fix. Also requests BOS as a
     * genuine two-step read (5-byte header, then 12 bytes - the
     * wTotalLength that header now correctly advertises), and - not yet
     * tracked as a signal here - retries DEVICE_QUALIFIER (descriptor type
     * 0x06) three times, which real Windows did not do post-fix. */
    if (stats->string_wlength_hist[USB_FP_WLENGTH_255] > 0 && !short_probe_seen) {
        return ZMK_OS_LINUX;
    }

    return ZMK_OS_UNKNOWN;
}

static struct usb_fp_stats stats;

static void usb_settle_work_handler(struct k_work *work) {
    enum zmk_os detected = zmk_os_classify_usb(&stats);
    LOG_DBG("os detection: USB fingerprint stats strings=%u other=%u bos=%d",
            stats.string_request_count, stats.string_wlength_other, stats.bos_requested);
    zmk_os_detection_report_usb(detected);
}

static K_WORK_DELAYABLE_DEFINE(usb_settle_work, usb_settle_work_handler);

static void reset_usb_fp_stats(void) { memset(&stats, 0, sizeof(stats)); }

/* Called (via the __wrap_usb_handle_bos hook below, or directly by the
 * CONFIG_ZMK_OS_DETECTION_TEST_INJECT shim) for every standard USB SETUP
 * packet. Observation only - never changes what's returned to the host. */
void zmk_os_detection_observe_setup(const struct usb_setup_packet *setup) {
    LOG_DBG("os detection: SETUP bmRequestType=0x%02x bRequest=0x%02x wValue=0x%04x "
            "wIndex=0x%04x wLength=%u",
            setup->bmRequestType, setup->bRequest, setup->wValue, setup->wIndex, setup->wLength);

    if (setup->bRequest == USB_SREQ_SET_ADDRESS) {
        /* Host (re)started enumeration; drop any previous cycle's stats. */
        reset_usb_fp_stats();
        return;
    }

    if (setup->bRequest != USB_SREQ_GET_DESCRIPTOR || !usb_reqtype_is_to_host(setup)) {
        return;
    }

    uint8_t descriptor_type = USB_GET_DESCRIPTOR_TYPE(setup->wValue);
    switch (descriptor_type) {
    case USB_DESC_STRING:
        stats.string_request_count++;
        if (setup->wLength == 255) {
            stats.string_wlength_hist[USB_FP_WLENGTH_255]++;
        } else if (setup->wLength == 2) {
            stats.string_wlength_hist[USB_FP_WLENGTH_2]++;
        } else {
            stats.string_wlength_other++;
        }
        break;
    case USB_DESC_BOS:
        stats.bos_requested = true;
        stats.bos_wlength = setup->wLength;
        break;
    default:
        return; /* not a signal we track; don't restart the debounce timer */
    }

    k_work_reschedule(&usb_settle_work, K_MSEC(CONFIG_ZMK_OS_DETECTION_USB_SETTLE_MS));
}

static int os_detection_usb_listener(const zmk_event_t *eh) {
    const struct zmk_usb_conn_state_changed *ev = as_zmk_usb_conn_state_changed(eh);
    if (ev && ev->conn_state == ZMK_USB_CONN_NONE) {
        k_work_cancel_delayable(&usb_settle_work);
        reset_usb_fp_stats();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(os_detection_usb, os_detection_usb_listener);
ZMK_SUBSCRIPTION(os_detection_usb, zmk_usb_conn_state_changed);

#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_TEST_INJECT)

/* Synthetic SETUP sequences approximating each OS's enumeration pattern, per
 * the (unverified - see docs/fingerprints.md) heuristics above. Used only to
 * unit-test the classifier on native_sim, which has no real USB DC. */
static void inject_setup(uint8_t bRequest, uint8_t descriptor_type, uint16_t wLength) {
    struct usb_setup_packet setup = {
        .bmRequestType = 0x80, /* device-to-host, standard, device recipient */
        .bRequest = bRequest,
        .wValue = (uint16_t)(descriptor_type << 8),
        .wLength = wLength,
    };
    zmk_os_detection_observe_setup(&setup);
}

/* Both a real Mac and a real iPhone send this right after SET_CONFIGURATION
 * (2026-07-19 side-by-side capture) - it is deliberately NOT a classification
 * signal anymore (see zmk_os_classify_usb()). Injected here only to prove the
 * classifier ignores it and still reports macOS for the Apple pattern. */
static void inject_set_feature_remote_wakeup(void) {
    struct usb_setup_packet setup = {
        .bmRequestType = 0x00, /* host-to-device, standard, device recipient */
        .bRequest = USB_SREQ_SET_FEATURE,
        .wValue = USB_SFS_REMOTE_WAKEUP,
    };
    zmk_os_detection_observe_setup(&setup);
}

/* Matches the *second*, re-verified real Windows capture in
 * docs/fingerprints.md (2026-07-05, after the os_detection_usb_bos_init()
 * fix above) - not the module's original Windows capture, which turned out
 * to be Windows failing to enumerate at all against this module's old
 * spec-invalid BOS response (see docs/windows-usb-enumeration-issue.md).
 * With a valid BOS, real Windows now enumerates successfully and *does*
 * request string descriptors (directly at wLength=255, indistinguishable
 * by itself from Linux) - the only signal that still discriminates it from
 * Linux is BOS itself being fetched directly at wLength=255 in one blind
 * shot, vs. Linux's genuine two-step header-then-advertised-length read
 * (see inject_linux_like()). Device descriptor probed at wLength=64 (not
 * macOS's 8), configuration fetched directly at wLength=255 (like BOS,
 * unlike Linux's two-step), and DEVICE_QUALIFIER requested only once
 * (Linux retries it 3x) are recorded here for fidelity/future use but
 * aren't tracked signals yet. */
static void inject_windows_like(void) {
    inject_setup(USB_SREQ_SET_ADDRESS, 0, 0);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_DEVICE, 64);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_DEVICE, 18);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_CONFIGURATION, 255);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_BOS, 255);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 255);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 255);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 255);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_DEVICE_QUALIFIER, 10);
}

/* Matches the real capture in docs/fingerprints.md: short header probe (2
 * bytes) then full re-read for each of 3 string descriptors, then BOS read
 * once at exactly its minimal header length. */
static void inject_macos_like(void) {
    inject_setup(USB_SREQ_SET_ADDRESS, 0, 0);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 2);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 24);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 2);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 24);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 2);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 34);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_BOS, 5);
}

/* Matches the 2026-07-19 real captures in docs/fingerprints.md: a real Mac
 * and a real iPhone captured side by side enumerated IDENTICALLY through this
 * point - device probed at wLength=8, short-probe-then-full re-read on every
 * string and on configuration, BOS read as a two-step 5-then-12 dance, then
 * SET_FEATURE(DEVICE_REMOTE_WAKEUP). Because macOS and iOS are indistinguishable
 * over USB, this scenario asserts the classifier reports macOS (ZMK_OS_MACOS,
 * value 2) - specifically guarding against the earlier regression where the
 * REMOTE_WAKEUP packet made a real Mac misdetect as iOS. */
static void inject_apple_with_remote_wakeup(void) {
    inject_setup(USB_SREQ_SET_ADDRESS, 0, 0);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_DEVICE, 8);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_DEVICE, 18);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 2);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 24);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 2);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 24);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 2);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 34);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_CONFIGURATION, 9);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_CONFIGURATION, 34);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_BOS, 5);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_BOS, 12);
    inject_set_feature_remote_wakeup();
}

/* Matches the *second*, re-verified real Linux capture in
 * docs/fingerprints.md (2026-07-05, after the os_detection_usb_bos_init()
 * fix above, captured from this workspace's own sandbox host - a real
 * Linux kernel): device descriptor probed at wLength=64 (same as Windows,
 * not macOS's 8); BOS now read as a genuine two-step header-then-full
 * dance (5-byte header probe, then 12 bytes - the wTotalLength that header
 * now correctly advertises once this module registers one BOS
 * capability), unlike the single pre-fix 5-byte read this scenario used to
 * inject - this final 12-byte read is what the classifier actually keys
 * off (Windows' single-shot 255-byte read never matches it); DEVICE_QUALIFIER
 * retried 3 times (not tracked as a signal, included for fidelity, and the
 * one place real Windows still differs - it doesn't retry at all);
 * configuration read as header-then-full like macOS; every string
 * descriptor (including index 0, the language-ID list) read directly at
 * the full 255-byte buffer with no short probe first - unchanged from
 * before the fix. */
static void inject_linux_like(void) {
    inject_setup(USB_SREQ_SET_ADDRESS, 0, 0);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_DEVICE, 64);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_DEVICE, 18);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_BOS, 5);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_BOS, 12);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_DEVICE_QUALIFIER, 10);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_DEVICE_QUALIFIER, 10);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_DEVICE_QUALIFIER, 10);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_CONFIGURATION, 9);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_CONFIGURATION, 34);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 255);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 255);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 255);
    inject_setup(USB_SREQ_GET_DESCRIPTOR, USB_DESC_STRING, 255);
}

/* Run every scenario sequentially in this one process/port-3240 owner,
 * pausing past the settle debounce between each so every scenario gets
 * classified (and logged) before the stats are reset by the next one's
 * SET_ADDRESS. See the ZMK_OS_DETECTION_TEST_INJECT Kconfig help for why
 * this isn't split into one native_sim process per scenario. */
static int os_detection_test_inject_init(void) {
    const uint32_t settle_margin_ms = CONFIG_ZMK_OS_DETECTION_USB_SETTLE_MS + 20;

    inject_windows_like();
    k_sleep(K_MSEC(settle_margin_ms));

    inject_macos_like();
    k_sleep(K_MSEC(settle_margin_ms));

    inject_linux_like();
    k_sleep(K_MSEC(settle_margin_ms));

    /* Last so its macOS result is distinct from the previous scenario's
     * (Linux) - zmk_os_detection_report_usb() only logs on a change, so two
     * adjacent macOS(2) scenarios would collapse to one snapshot line. */
    inject_apple_with_remote_wakeup();
    k_sleep(K_MSEC(settle_margin_ms));

    return 0;
}

SYS_INIT(os_detection_test_inject_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_ZMK_OS_DETECTION_TEST_INJECT */

/* Always wrap the real hook, TEST_INJECT or not: --wrap=usb_handle_bos is
 * applied unconditionally by CMakeLists.txt whenever this file is compiled,
 * so __wrap_usb_handle_bos must always exist or the link fails. On
 * native_sim there's no real USB DC to generate SETUP packets, but
 * CONFIG_USB_DEVICE_BOS (selected by CONFIG_ZMK_OS_DETECTION_USB) still
 * compiles usb_handle_bos() in, so the wrap target exists there too. */
int __real_usb_handle_bos(struct usb_setup_packet *setup, int32_t *len, uint8_t **data);

int __wrap_usb_handle_bos(struct usb_setup_packet *setup, int32_t *len, uint8_t **data) {
    zmk_os_detection_observe_setup(setup);
    return __real_usb_handle_bos(setup, len, data);
}
