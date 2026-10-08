#include <pb_decode.h>
#include <pb_encode.h>
#include <zmk/studio/custom.h>
#include <zmk/device_info/device_info.pb.h>
#include <zmk/sensors.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
/* Zephyr 3.5 generates version.h at the include root, not under zephyr/. */
#include <version.h>
#include <app_version.h>

#if IS_ENABLED(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_HWINFO)
/*
 * The watchdog module's boot-time reset-cause audit clears the chip's reset
 * cause register a moment after boot, so reading it when Studio asks -- seconds
 * or minutes later -- always reported 0 and the field was useless for "why did
 * my keyboard reboot" questions. Capture the real value first, at init time,
 * and report that snapshot instead.
 */
static uint32_t boot_reset_cause;

static int device_info_capture_reset_cause(void) {
    (void)hwinfo_get_reset_cause(&boot_reset_cause);
    return 0;
}

SYS_INIT(device_info_capture_reset_cause, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif

/*
 * GNU build-id note. On real hardware it is emitted by the linker and pinned
 * into flash by src/build_id.ld (see CMakeLists.txt). It is skipped on
 * native_sim/posix, where there is no flash image to match and the native link
 * discards the note section, so we compile out the reader there.
 */
#if !IS_ENABLED(CONFIG_ARCH_POSIX)

/*
 * The linker always defines these boundary symbols; when build-id emission is
 * disabled the range is empty (start == end) and we simply report no hash.
 */
extern const uint8_t __build_id_note_start[];
extern const uint8_t __build_id_note_end[];

struct build_id_note {
    uint32_t namesz; /* size of the note name (e.g. "GNU\0") */
    uint32_t descsz; /* size of the descriptor (the build-id bytes) */
    uint32_t type;   /* NT_GNU_BUILD_ID == 3 */
    uint8_t data[];  /* name (namesz, 4-byte aligned) then descriptor (descsz) */
};

#define NT_GNU_BUILD_ID 3

/* Hex-encode the build-id descriptor into out (out_size includes the null). */
static void fill_build_hash(char *out, size_t out_size) {
    if (out_size == 0) {
        return;
    }
    out[0] = '\0';

    size_t note_bytes = (size_t)(__build_id_note_end - __build_id_note_start);
    if (note_bytes < sizeof(struct build_id_note)) {
        return;
    }

    const struct build_id_note *note = (const struct build_id_note *)__build_id_note_start;
    if (note->type != NT_GNU_BUILD_ID) {
        return;
    }

    /* The note name is padded to a 4-byte boundary; the descriptor follows. */
    const uint8_t *desc = note->data + ROUND_UP(note->namesz, 4);
    if (desc + note->descsz > __build_id_note_end) {
        return;
    }

    for (size_t i = 0; i < note->descsz && (i * 2 + 2) < out_size; i++) {
        snprintf(out + i * 2, 3, "%02x", desc[i]);
    }
}

#else

static void fill_build_hash(char *out, size_t out_size) {
    if (out_size > 0) {
        out[0] = '\0';
    }
}

#endif /* !CONFIG_ARCH_POSIX */

static struct zmk_rpc_custom_subsystem_meta device_info_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("http://cormoran.github.io/zmk-feature-device-info/"),
#if IS_ENABLED(CONFIG_ZMK_DEVICE_INFO_STUDIO_RPC_REQUIRE_UNLOCK)
    .security = ZMK_STUDIO_RPC_HANDLER_SECURED,
#else
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
#endif
};

ZMK_RPC_CUSTOM_SUBSYSTEM(zmk__device_info, &device_info_meta, device_info_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(zmk__device_info, zmk_device_info_Response);

static bool device_info_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                           pb_callback_t *encode_response);

static bool encode_zephyr_devices(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const struct device *devices;
    size_t count = z_device_get_all_static(&devices);

    for (size_t i = 0; i < count; i++) {
        if (!devices[i].name) {
            continue;
        }
        zmk_device_info_ZephyrDevice dev = zmk_device_info_ZephyrDevice_init_zero;
        strncpy(dev.name, devices[i].name, sizeof(dev.name) - 1);
        dev.ready = device_is_ready(&devices[i]);
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        if (!pb_encode_submessage(stream, zmk_device_info_ZephyrDevice_fields, &dev)) {
            return false;
        }
    }
    return true;
}

static int handle_get_device_info(const zmk_device_info_GetDeviceInfoRequest *req,
                                  zmk_device_info_Response *resp) {
    zmk_device_info_DeviceInfoResponse result = zmk_device_info_DeviceInfoResponse_init_zero;

    /* Build info */
    result.has_build = true;
    strncpy(result.build.zmk_version, ZMK_BUILD_VERSION, sizeof(result.build.zmk_version) - 1);
    result.build.zmk_dirty = strstr(result.build.zmk_version, "dirty") != NULL;

    strncpy(result.build.zmk_config_version, ZMK_CONFIG_BUILD_VERSION,
            sizeof(result.build.zmk_config_version) - 1);
    result.build.zmk_config_dirty = strstr(result.build.zmk_config_version, "dirty") != NULL;

    strncpy(result.build.module_version, MODULE_BUILD_VERSION,
            sizeof(result.build.module_version) - 1);
    result.build.module_dirty = strstr(result.build.module_version, "dirty") != NULL;

    strncpy(result.build.zephyr_version, KERNEL_VERSION_STRING,
            sizeof(result.build.zephyr_version) - 1);

    strncpy(result.build.build_timestamp, BUILD_TIMESTAMP,
            sizeof(result.build.build_timestamp) - 1);

    /* Zephyr 3.5 has no CONFIG_BOARD symbol (added later); the keyboard name is
     * the closest thing this tree can report. */
    strncpy(result.build.board, CONFIG_ZMK_KEYBOARD_NAME, sizeof(result.build.board) - 1);

    fill_build_hash(result.build.build_hash, sizeof(result.build.build_hash));

    /* Hardware info */
    result.has_hardware = true;
#if IS_ENABLED(CONFIG_HWINFO)
    {
        uint8_t dev_id[16];
        ssize_t id_len = hwinfo_get_device_id(dev_id, sizeof(dev_id));
        if (id_len > 0) {
            for (int i = 0; i < id_len && (i * 2 + 2) < (int)sizeof(result.hardware.device_id);
                 i++) {
                snprintf(result.hardware.device_id + i * 2, 3, "%02x", dev_id[i]);
            }
        }
        result.hardware.reset_cause = boot_reset_cause;
    }
#endif

#ifdef CONFIG_FLASH_SIZE
    result.hardware.flash_size_kb = CONFIG_FLASH_SIZE;
#endif
#ifdef CONFIG_SRAM_SIZE
    result.hardware.sram_size_kb = CONFIG_SRAM_SIZE;
#endif

    /* Zephyr device list via callback */
    result.zephyr_devices.funcs.encode = encode_zephyr_devices;

    /* ZMK config (compile-time Kconfig/DT values) */
    result.has_zmk_config = true;
#if DT_HAS_CHOSEN(zmk_kscan)
    /* DT_PROP_BY_IDX for compatible is unreliable; check known types explicitly */
#if DT_NODE_HAS_COMPAT(DT_CHOSEN(zmk_kscan), zmk_kscan_gpio_matrix)
    strncpy(result.zmk_config.kscan_compatible, "zmk,kscan-gpio-matrix",
            sizeof(result.zmk_config.kscan_compatible) - 1);
#elif DT_NODE_HAS_COMPAT(DT_CHOSEN(zmk_kscan), zmk_kscan_gpio_direct)
    strncpy(result.zmk_config.kscan_compatible, "zmk,kscan-gpio-direct",
            sizeof(result.zmk_config.kscan_compatible) - 1);
#elif DT_NODE_HAS_COMPAT(DT_CHOSEN(zmk_kscan), zmk_kscan_gpio_demux)
    strncpy(result.zmk_config.kscan_compatible, "zmk,kscan-gpio-demux",
            sizeof(result.zmk_config.kscan_compatible) - 1);
#elif DT_NODE_HAS_COMPAT(DT_CHOSEN(zmk_kscan), zmk_kscan_composite)
    strncpy(result.zmk_config.kscan_compatible, "zmk,kscan-composite",
            sizeof(result.zmk_config.kscan_compatible) - 1);
#elif DT_NODE_HAS_COMPAT(DT_CHOSEN(zmk_kscan), zmk_kscan_mock)
    strncpy(result.zmk_config.kscan_compatible, "zmk,kscan-mock",
            sizeof(result.zmk_config.kscan_compatible) - 1);
#endif
#endif

    result.zmk_config.ble_enabled = IS_ENABLED(CONFIG_ZMK_BLE);
#if IS_ENABLED(CONFIG_ZMK_BLE)
    result.zmk_config.ble_profile_count = CONFIG_BT_MAX_PAIRED;
#endif

    result.zmk_config.usb_enabled = IS_ENABLED(CONFIG_ZMK_USB);
    result.zmk_config.split_enabled = IS_ENABLED(CONFIG_ZMK_SPLIT);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    strncpy(result.zmk_config.split_role, "central", sizeof(result.zmk_config.split_role) - 1);
#elif IS_ENABLED(CONFIG_ZMK_SPLIT)
    strncpy(result.zmk_config.split_role, "peripheral", sizeof(result.zmk_config.split_role) - 1);
#else
    strncpy(result.zmk_config.split_role, "none", sizeof(result.zmk_config.split_role) - 1);
#endif

    result.zmk_config.display_enabled = IS_ENABLED(CONFIG_ZMK_DISPLAY);
    result.zmk_config.rgb_underglow_enabled = IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW);
    result.zmk_config.backlight_enabled = IS_ENABLED(CONFIG_ZMK_BACKLIGHT);
    result.zmk_config.battery_level_enabled = IS_ENABLED(CONFIG_ZMK_BATTERY_LEVEL_FETCHING);
    /* How many encoder (sensor-rotate) bindings the keymap declares. The web UI
     * uses this to skip the "enable runtime sensor rotate" hint entirely on
     * keyboards that have no encoders. */
    result.zmk_config.encoder_count = ZMK_KEYMAP_SENSORS_LEN;

    /* Runtime status */
    result.has_runtime = true;
    result.runtime.uptime_ms = (uint32_t)k_uptime_get();

    resp->which_response_type = zmk_device_info_Response_device_info_tag;
    resp->response_type.device_info = result;
    return 0;
}

static bool device_info_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                           pb_callback_t *encode_response) {
    zmk_device_info_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(zmk__device_info, encode_response);

    zmk_device_info_Request req = zmk_device_info_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, zmk_device_info_Request_fields, &req)) {
        LOG_WRN("Failed to decode device info request: %s", PB_GET_ERROR(&req_stream));
        zmk_device_info_ErrorResponse err = zmk_device_info_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to decode request");
        resp->which_response_type = zmk_device_info_Response_error_tag;
        resp->response_type.error = err;
        return true;
    }

    int rc = 0;
    switch (req.which_request_type) {
    case zmk_device_info_Request_get_device_info_tag:
        rc = handle_get_device_info(&req.request_type.get_device_info, resp);
        break;
    default:
        LOG_WRN("Unsupported device info request type: %d", req.which_request_type);
        rc = -1;
    }

    if (rc != 0) {
        zmk_device_info_ErrorResponse err = zmk_device_info_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to process request");
        resp->which_response_type = zmk_device_info_Response_error_tag;
        resp->response_type.error = err;
    }
    return true;
}
