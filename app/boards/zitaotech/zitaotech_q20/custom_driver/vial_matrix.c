/*
 * Minimal read-only Vial support for the ZitaoTech Q20.
 *
 * Vial needs:
 *   - a USB serial number containing "vial:f64c2b3c"
 *   - a raw HID collection with usage page 0xFF60 / usage 0x61
 *   - enough of the Vial/VIA protocol to download a keyboard definition and
 *     poll the physical matrix state
 *
 * Dynamic keymap editing is intentionally not implemented.  The Q20 still
 * uses ZMK's normal keymap for typing; this interface only exposes the raw
 * matrix to Vial's matrix tester.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/usb/usb_device.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/hid_indicators.h>
#include <zmk/matrix.h>
#include <zmk/matrix_transform.h>
#include <zmk/endpoints.h>
#include <zmk/physical_layouts.h>

#include "vial_matrix_def.h"

/*
 * The matrix tester only makes sense while the keyboard really is a GPIO
 * matrix.  Images built on this board with a different kscan - the
 * settings_reset shield swaps in a mock kscan - compile this file to nothing
 * instead of failing on the missing row_gpios/col_gpios properties.
 */
#if DT_NODE_HAS_PROP(DT_CHOSEN(zmk_kscan), row_gpios) &&                                          \
    DT_NODE_HAS_PROP(DT_CHOSEN(zmk_kscan), col_gpios)
#define ZITAOTECH_Q20_VIAL_MATRIX_BUILD 1
#endif

#ifdef ZITAOTECH_Q20_VIAL_MATRIX_BUILD

#define VIAL_RAW_REPORT_SIZE 32
#define VIAL_VIA_PROTOCOL_VERSION 0x0009
#define VIAL_PROTOCOL_VERSION 0x00000003UL
#define VIAL_PREFIX 0xFE

#define VIAL_KSCAN_NODE DT_CHOSEN(zmk_kscan)
#define VIAL_MATRIX_ROWS DT_PROP_LEN(VIAL_KSCAN_NODE, row_gpios)
#define VIAL_MATRIX_COLS DT_PROP_LEN(VIAL_KSCAN_NODE, col_gpios)
#define VIAL_MATRIX_BYTES_PER_ROW ((VIAL_MATRIX_COLS + 7) / 8)
#define VIAL_MATRIX_STATE_SIZE (VIAL_MATRIX_ROWS * VIAL_MATRIX_BYTES_PER_ROW)

static const uint8_t vial_hid_report_desc[] = {
    0x06, 0x60, 0xFF, /* Usage Page (Vendor Defined 0xFF60) */
    0x09, 0x61,       /* Usage (0x61) */
    0xA1, 0x01,       /* Collection (Application) */
    0x09, 0x62,       /*   Usage (0x62) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255) */
    0x95, 0x20,       /*   Report Count (32) */
    0x75, 0x08,       /*   Report Size (8) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x09, 0x63,       /*   Usage (0x63) */
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x95, 0x20,
    0x75, 0x08,
    0x91, 0x02,       /*   Output (Data,Var,Abs) */
    0xC0
};

static const uint8_t vial_keyboard_uid[8] = {
    0x5A, 0x10, 0x9E, 0x42, 0x7C, 0x31, 0xD4, 0xB9
};

static const struct device *vial_hid_dev;
static const struct device *keyboard_hid_dev;
static volatile uint8_t vial_matrix_state[VIAL_MATRIX_STATE_SIZE];
static uint16_t position_to_cell[ZMK_KEYMAP_LEN];
static bool position_map_ready;
static uint8_t vial_rx[VIAL_RAW_REPORT_SIZE];
static struct k_work_delayable keyboard_hid_out_work;

static void build_position_map(void) {
    if (position_map_ready) {
        return;
    }

    const struct zmk_physical_layout *const *layouts = NULL;
    size_t layout_count = zmk_physical_layouts_get_list(&layouts);
    int selected = zmk_physical_layouts_get_selected();

    if (layouts == NULL || selected < 0 || selected >= (int)layout_count) {
        return;
    }

    zmk_matrix_transform_t transform = layouts[selected]->matrix_transform;
    if (transform == NULL) {
        return;
    }

    for (uint32_t row = 0; row < VIAL_MATRIX_ROWS; row++) {
        for (uint32_t col = 0; col < VIAL_MATRIX_COLS; col++) {
            int32_t position = zmk_matrix_transform_row_column_to_position(transform, row, col);
            if (position >= 0 && position < (int32_t)ZMK_KEYMAP_LEN) {
                position_to_cell[position] = (uint16_t)(1 + row * VIAL_MATRIX_COLS + col);
            }
        }
    }

    position_map_ready = true;
}

static int vial_map_init(void) {
    build_position_map();
    return 0;
}

/* Run after ZMK's physical-layout init so the inverse matrix map is ready
 * before the first key event or Vial poll. */
#define VIAL_MAP_INIT_PRIORITY 91
SYS_INIT(vial_map_init, APPLICATION, VIAL_MAP_INIT_PRIORITY);

static int vial_position_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    build_position_map();

    if (ev->position >= ZMK_KEYMAP_LEN) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint16_t cell = position_to_cell[ev->position];
    if (cell == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint16_t index = cell - 1;
    uint8_t row = (uint8_t)(index / VIAL_MATRIX_COLS);
    uint8_t col = (uint8_t)(index % VIAL_MATRIX_COLS);
    uint8_t byte_index = (uint8_t)((VIAL_MATRIX_BYTES_PER_ROW - 1) - (col / 8));
    uint8_t mask = BIT(col % 8);
    volatile uint8_t *state = &vial_matrix_state[row * VIAL_MATRIX_BYTES_PER_ROW + byte_index];

    if (ev->state) {
        *state |= mask;
    } else {
        *state &= (uint8_t)~mask;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(vial_matrix, vial_position_listener);
ZMK_SUBSCRIPTION(vial_matrix, zmk_position_state_changed);

static void vial_matrix_poll(uint8_t *data) {
    build_position_map();

    uint8_t offset = data[2];
    uint8_t row_bytes = VIAL_MATRIX_BYTES_PER_ROW;
    uint8_t max_rows = (uint8_t)(28 / row_bytes);

    memset(&data[2], 0, VIAL_RAW_REPORT_SIZE - 2);

    for (uint8_t row = 0; row < max_rows && (row + offset) < VIAL_MATRIX_ROWS; row++) {
        for (uint8_t byte = 0; byte < row_bytes; byte++) {
            data[2 + row * row_bytes + byte] =
                vial_matrix_state[(row + offset) * row_bytes + byte];
        }
    }
}

static void vial_process_vial(uint8_t *data) {
    switch (data[1]) {
    case 0x00: { /* Get keyboard ID and Vial protocol version. */
        memset(data, 0, VIAL_RAW_REPORT_SIZE);
        data[0] = (uint8_t)(VIAL_PROTOCOL_VERSION & 0xFF);
        data[1] = (uint8_t)((VIAL_PROTOCOL_VERSION >> 8) & 0xFF);
        data[2] = (uint8_t)((VIAL_PROTOCOL_VERSION >> 16) & 0xFF);
        data[3] = (uint8_t)((VIAL_PROTOCOL_VERSION >> 24) & 0xFF);
        memcpy(&data[4], vial_keyboard_uid, sizeof(vial_keyboard_uid));
        break;
    }
    case 0x01: { /* Get compressed keyboard definition size. */
        uint32_t size = VIAL_KEYBOARD_DEFINITION_SIZE;
        data[0] = (uint8_t)(size & 0xFF);
        data[1] = (uint8_t)((size >> 8) & 0xFF);
        data[2] = (uint8_t)((size >> 16) & 0xFF);
        data[3] = (uint8_t)((size >> 24) & 0xFF);
        break;
    }
    case 0x02: { /* Get one 32-byte page of the compressed definition. */
        uint32_t page = (uint32_t)data[2] | ((uint32_t)data[3] << 8);
        uint32_t start = page * VIAL_RAW_REPORT_SIZE;
        uint32_t length;

        memset(data, 0, VIAL_RAW_REPORT_SIZE);
        if (start >= VIAL_KEYBOARD_DEFINITION_SIZE) {
            break;
        }

        length = VIAL_KEYBOARD_DEFINITION_SIZE - start;
        if (length > VIAL_RAW_REPORT_SIZE) {
            length = VIAL_RAW_REPORT_SIZE;
        }
        memcpy(data, &VialKeyboardDefinition[start], length);
        break;
    }
    case 0x05: /* Unlock status: always unlocked, no unlock key combination. */
        memset(data, 0xFF, VIAL_RAW_REPORT_SIZE);
        data[0] = 1;
        data[1] = 0;
        break;
    default:
        break;
    }
}

static void vial_process_report(uint8_t *data) {
    switch (data[0]) {
    case 0x01: /* VIA get protocol version. */
        data[1] = (uint8_t)(VIAL_VIA_PROTOCOL_VERSION >> 8);
        data[2] = (uint8_t)(VIAL_VIA_PROTOCOL_VERSION & 0xFF);
        break;

    case 0x02: /* VIA get keyboard value. */
        if (data[1] == 0x03) { /* Matrix tester. */
            vial_matrix_poll(data);
        }
        break;

    case 0x0C: /* Macro count: none. */
        data[1] = 0;
        break;

    case 0x0D: /* Macro buffer size: none. */
        data[1] = 0;
        data[2] = 0;
        break;

    case 0x11: /* One keymap layer is enough for the tester. */
        data[1] = 1;
        break;

    case 0x12: { /* Keymap buffer read: return all KC_NO. */
        uint8_t size = data[3];
        if (size > 28) {
            size = 28;
        }
        memset(&data[4], 0, size);
        break;
    }

    case VIAL_PREFIX:
        vial_process_vial(data);
        break;

    default:
        break;
    }
}

#if IS_ENABLED(CONFIG_ENABLE_HID_INT_OUT_EP)
static void vial_int_out_ready(const struct device *dev) {
    uint32_t length = 0;

    if (hid_int_ep_read(dev, vial_rx, sizeof(vial_rx), &length) != 0) {
        return;
    }

    if (length == 0) {
        return;
    }

    if (length > sizeof(vial_rx)) {
        length = sizeof(vial_rx);
    }
    if (length < sizeof(vial_rx)) {
        memset(&vial_rx[length], 0, sizeof(vial_rx) - length);
    }

    vial_process_report(vial_rx);
    (void)hid_int_ep_write(dev, vial_rx, sizeof(vial_rx), NULL);
}
#endif

/*
 * Vial needs an OUT endpoint on its raw HID interface, and Zephyr's HID OUT
 * endpoint option is global.  Consequently the normal keyboard HID interface
 * also advertises an OUT endpoint, and some hosts send the CapsLock/NumLock
 * LED output report there instead of using a control SET_REPORT transfer.
 *
 * Poll HID_0's OUT endpoint from this board-specific Vial module so the
 * indicator state still reaches ZMK's HID indicator layer.  This keeps the
 * workaround out of app/src/usb_hid.c.
 */
static void keyboard_hid_out_poll(struct k_work *work) {
    uint8_t buffer[CONFIG_HID_INTERRUPT_EP_MPS];
    uint32_t length = 0;

    if ((keyboard_hid_dev != NULL) && device_is_ready(keyboard_hid_dev)) {
        if ((hid_int_ep_read(keyboard_hid_dev, buffer, sizeof(buffer), &length) == 0) &&
            (length > 0)) {
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
            struct zmk_hid_led_report_body *body = NULL;

            /*
             * Only accept a well-formed LED report.  The old code also took a
             * bare one-byte transfer as an LED bitmap ("some hosts omit the
             * report ID"), which meant any stray byte landing on the keyboard
             * interface's OUT endpoint overwrote the indicators: a 0x01 report
             * ID read as a bitmap, for instance, sets Num Lock and clears Caps
             * Lock, so the Caps Lock LED and scroll mode dropped out while the
             * lock was still on.  Hosts that use the interrupt OUT path send
             * the report ID with the report; the keyboard's own Caps Lock key
             * keeps the indicator right on hosts that do not report at all.
             */
            if ((length == sizeof(struct zmk_hid_led_report)) &&
                (buffer[0] == ZMK_HID_REPORT_ID_LEDS)) {
                body = (struct zmk_hid_led_report_body *)&buffer[1];
            }

            if (body != NULL) {
                struct zmk_endpoint_instance endpoint = {
                    .transport = ZMK_TRANSPORT_USB,
                };
                zmk_hid_indicators_process_report(body, endpoint);
            }
#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
        }
    }

    k_work_reschedule(&keyboard_hid_out_work, K_MSEC(5));
}

static int vial_set_report(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                           uint8_t **data) {
    uint8_t buffer[VIAL_RAW_REPORT_SIZE] = {0};
    size_t length;

    ARG_UNUSED(setup);

    if (*data == NULL || *len <= 0) {
        return -EINVAL;
    }

    length = MIN((size_t)*len, sizeof(buffer));
    memcpy(buffer, *data, length);
    vial_process_report(buffer);
    (void)hid_int_ep_write(dev, buffer, sizeof(buffer), NULL);
    return 0;
}

static const struct hid_ops vial_ops = {
    .set_report = vial_set_report,
#if IS_ENABLED(CONFIG_ENABLE_HID_INT_OUT_EP)
    .int_out_ready = vial_int_out_ready,
#endif
};

static int vial_hid_init(void) {
    vial_hid_dev = device_get_binding("HID_1");
    if (vial_hid_dev == NULL) {
        return -ENODEV;
    }

    keyboard_hid_dev = device_get_binding("HID_0");
    if (keyboard_hid_dev != NULL) {
        k_work_init_delayable(&keyboard_hid_out_work, keyboard_hid_out_poll);
        k_work_reschedule(&keyboard_hid_out_work, K_MSEC(100));
    }

    usb_hid_register_device(vial_hid_dev, vial_hid_report_desc, sizeof(vial_hid_report_desc),
                            &vial_ops);
    return usb_hid_init(vial_hid_dev);
}

SYS_INIT(vial_hid_init, APPLICATION, CONFIG_ZMK_USB_HID_INIT_PRIORITY);

/*
 * Zephyr's default serial number is derived from the chip's hardware ID.
 * Vial keys off this magic string, so return it verbatim.  CONFIG_USB_DEVICE_SN
 * in the board defconfig must have exactly the same length.
 */
uint8_t *usb_update_sn_string_descriptor(void) {
    return (uint8_t *)"vial:f64c2b3c";
}

#endif /* ZITAOTECH_Q20_VIAL_MATRIX_BUILD */
