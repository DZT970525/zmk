/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <lvgl.h>
#include <zmk/endpoints.h>

#define CANVAS_SIZE 72
/* Enlarged status canvas only; BLE/layer canvases remain 72 square. */
#define STATUS_CANVAS_SIZE 80
#define LAYER_CANVAS_HEIGHT 16

#define LVGL_BACKGROUND                                                                            \
    IS_ENABLED(CONFIG_NICE_VIEW_WIDGET_INVERTED) ? lv_color_black() : lv_color_white()
#define LVGL_FOREGROUND                                                                            \
    IS_ENABLED(CONFIG_NICE_VIEW_WIDGET_INVERTED) ? lv_color_white() : lv_color_black()

/*
 * Colors of the battery indicator. The panel is an 8 color display (black, red,
 * green, blue, cyan, magenta, yellow and white), so the charge level is shown in
 * green / yellow / red, on a monochrome display all of them are rendered as the
 * foreground color.
 */
#define LVGL_BATTERY_FULL_LEVEL 60
#define LVGL_BATTERY_MEDIUM_LEVEL 25

#define LVGL_BATTERY_FULL lv_color_make(0x00, 0xff, 0x00)
#define LVGL_BATTERY_MEDIUM lv_color_make(0xff, 0xff, 0x00)
#define LVGL_BATTERY_LOW lv_color_make(0xff, 0x00, 0x00)

static inline lv_color_t lvgl_battery_level_color(uint8_t level) {
    if (level >= LVGL_BATTERY_FULL_LEVEL) {
        return LVGL_BATTERY_FULL;
    }
    if (level >= LVGL_BATTERY_MEDIUM_LEVEL) {
        return LVGL_BATTERY_MEDIUM;
    }
    return LVGL_BATTERY_LOW;
}

/*
 * Colors of the output / connection symbol in the top right corner:
 * USB output, BLE connected, BLE bonded but not connected (cross) and BLE
 * waiting for a profile (gear).
 */
#define LVGL_OUTPUT_USB lv_color_make(0xff, 0xff, 0x00)
#define LVGL_OUTPUT_CONNECTED LVGL_FOREGROUND
#define LVGL_OUTPUT_DISCONNECTED lv_color_make(0xff, 0x00, 0x00)
#define LVGL_OUTPUT_OPEN lv_color_make(0x00, 0xff, 0xff)

/* Highlight color of the selected BLE profile slot (1st to 4th circle) */
static inline lv_color_t lvgl_profile_color(int index) {
    switch (index) {
    case 0:
        return lv_color_make(0x00, 0xff, 0xff);
    case 1:
        return lv_color_make(0x00, 0xff, 0x00);
    case 2:
        return lv_color_make(0xff, 0xff, 0x00);
    default:
        return LVGL_FOREGROUND;
    }
}

struct status_state {
    uint8_t battery;
    bool charging;
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
    uint8_t layer_index;
    const char *layer_label;
    uint8_t wpm[10];
#else
    bool connected;
#endif
};

struct battery_status_state {
    uint8_t level;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    bool usb_present;
#endif
};

void rotate_canvas(lv_obj_t *canvas, uint16_t cbuf[]);
void draw_battery(lv_obj_t *canvas, const struct status_state *state);
void init_label_dsc(lv_draw_label_dsc_t *label_dsc, lv_color_t color, const lv_font_t *font,
                    lv_text_align_t align);
void init_rect_dsc(lv_draw_rect_dsc_t *rect_dsc, lv_color_t bg_color);
void init_line_dsc(lv_draw_line_dsc_t *line_dsc, lv_color_t color, uint8_t width);
void init_arc_dsc(lv_draw_arc_dsc_t *arc_dsc, lv_color_t color, uint8_t width);
