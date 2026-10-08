/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/display.h>
#include "status.h"
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/wpm.h>

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};

struct layer_status_state {
    uint8_t index;
    const char *label;
};

struct wpm_status_state {
    uint8_t wpm;
};

static void draw_top(lv_obj_t *widget, uint16_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);
    lv_draw_label_dsc_t label_dsc_wpm;
    init_label_dsc(&label_dsc_wpm, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_RIGHT);
    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t rect_white_dsc;
    init_rect_dsc(&rect_white_dsc, LVGL_FOREGROUND);
    lv_draw_line_dsc_t line_dsc;
    init_line_dsc(&line_dsc, LVGL_FOREGROUND, 2);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    // Fill background
    lv_area_t area = {0, 0, CANVAS_SIZE - 1, CANVAS_SIZE - 1};
    lv_draw_rect(&layer, &rect_black_dsc, &area);

    // Draw output status
    char output_text[10] = {};
    lv_color_t output_color = LVGL_OUTPUT_CONNECTED;

    switch (state->selected_endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        strcat(output_text, LV_SYMBOL_USB);
        output_color = LVGL_OUTPUT_USB;
        break;
    case ZMK_TRANSPORT_BLE:
        if (state->active_profile_bonded) {
            if (state->active_profile_connected) {
                strcat(output_text, LV_SYMBOL_WIFI);
                output_color = LVGL_OUTPUT_CONNECTED;
            } else {
                strcat(output_text, LV_SYMBOL_CLOSE);
                output_color = LVGL_OUTPUT_DISCONNECTED;
            }
        } else {
            strcat(output_text, LV_SYMBOL_SETTINGS);
            output_color = LVGL_OUTPUT_OPEN;
        }
        break;
    }

    label_dsc.color = output_color;
    label_dsc.text = output_text;
    area = (lv_area_t){0, 0, CANVAS_SIZE - 1, 19};
    lv_draw_label(&layer, &label_dsc, &area);

    // Draw WPM
    area = (lv_area_t){0, 21, 0 + 70 - 1, 21 + 44 - 1};
    lv_draw_rect(&layer, &rect_white_dsc, &area);
    area = (lv_area_t){1, 22, 1 + 66 - 1, 22 + 42 - 1};
    lv_draw_rect(&layer, &rect_black_dsc, &area);

    char wpm_text[6] = {};
    snprintf(wpm_text, sizeof(wpm_text), "%d", state->wpm[9]);
    label_dsc_wpm.text = wpm_text;
    area = (lv_area_t){42, 52, 42 + 24 - 1, 52 + 8 - 1};
    lv_draw_label(&layer, &label_dsc_wpm, &area);

    int max = 0;
    int min = 256;

    for (int i = 0; i < 10; i++) {
        if (state->wpm[i] > max) {
            max = state->wpm[i];
        }
        if (state->wpm[i] < min) {
            min = state->wpm[i];
        }
    }

    int range = max - min;
    if (range == 0) {
        range = 1;
    }

    lv_point_t points[10];
    for (int i = 0; i < 10; i++) {
        points[i].x = 2 + i * 7;
        points[i].y = 62 - (int)(((float)(state->wpm[i] - min) * 38.0f) / range + 0.5f);
    }
    for (int i = 0; i < 9; i++) {
        line_dsc.p1.x = points[i].x;
        line_dsc.p1.y = points[i].y;
        line_dsc.p2.x = points[i + 1].x;
        line_dsc.p2.y = points[i + 1].y;
        lv_draw_line(&layer, &line_dsc);
    }

    lv_canvas_finish_layer(canvas, &layer);

    // Draw battery (its own layer)
    draw_battery(canvas, state);

    // Rotate canvas
    rotate_canvas(canvas, cbuf);
}

static void draw_middle(lv_obj_t *widget, uint16_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 1);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t rect_white_dsc;
    init_rect_dsc(&rect_white_dsc, LVGL_FOREGROUND);
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER);
    lv_draw_label_dsc_t label_dsc_black;
    init_label_dsc(&label_dsc_black, LVGL_BACKGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    // 填充背景
    lv_area_t area = {0, 0, CANVAS_SIZE - 1, CANVAS_SIZE - 1};
    lv_draw_rect(&layer, &rect_black_dsc, &area);

    // 调整后四个圆位置：整体下移 5 像素
    int circle_offsets[4][2] = {
        {18, 15}, // 原18,18 -> y+5
        {50, 15}, // 原50,18 -> y+5
        {18, 46}, // 原18,50 -> y+5
        {50, 46}, // 原50,50 -> y+5
    };

    for (int i = 0; i < 4; i++) {
        bool selected = i == state->active_profile_index;
        /* every slot has its own highlight color */
        lv_color_t highlight = lvgl_profile_color(i);

        /* ring and highlight of the selected slot share the slot color */
        lv_draw_arc_dsc_t arc_dsc;
        init_arc_dsc(&arc_dsc, selected ? highlight : LVGL_FOREGROUND, 2);
        arc_dsc.center.x = circle_offsets[i][0];
        arc_dsc.center.y = circle_offsets[i][1];
        arc_dsc.radius = 13;
        arc_dsc.start_angle = 0;
        arc_dsc.end_angle = 360;
        lv_draw_arc(&layer, &arc_dsc);

        if (selected) {
            lv_draw_arc_dsc_t arc_dsc_filled;
            init_arc_dsc(&arc_dsc_filled, highlight, 9);
            arc_dsc_filled.center.x = circle_offsets[i][0];
            arc_dsc_filled.center.y = circle_offsets[i][1];
            arc_dsc_filled.radius = 9;
            arc_dsc_filled.start_angle = 0;
            arc_dsc_filled.end_angle = 359;
            lv_draw_arc(&layer, &arc_dsc_filled);
        }

        char label[2];
        snprintf(label, sizeof(label), "%d", i + 1);
        label_dsc.text = label;
        label_dsc_black.text = label;
        area = (lv_area_t){circle_offsets[i][0] - 8, circle_offsets[i][1] - 10,
                           circle_offsets[i][0] - 8 + 16 - 1, circle_offsets[i][1] - 10 + 18 - 1};
        lv_draw_label(&layer, (selected ? &label_dsc_black : &label_dsc), &area);
    }

    lv_canvas_finish_layer(canvas, &layer);

    rotate_canvas(canvas, cbuf);
}

static void draw_bottom(lv_obj_t *widget, uint16_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 2);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    // Fill background
    lv_area_t area = {0, 0, CANVAS_SIZE - 1, CANVAS_SIZE - 1};
    lv_draw_rect(&layer, &rect_black_dsc, &area);

    // Draw layer
    if (state->layer_label == NULL) {
        char text[10] = {};

        sprintf(text, "LAYER %i", state->layer_index);

        label_dsc.text = text;
        area = (lv_area_t){0, 0, 72 - 1, 14};
        lv_draw_label(&layer, &label_dsc, &area);
    } else {
        label_dsc.text = state->layer_label;
        area = (lv_area_t){0, 0, 72 - 1, 14};
        lv_draw_label(&layer, &label_dsc, &area);
    }

    lv_canvas_finish_layer(canvas, &layer);

    // Rotate canvas
    rotate_canvas(canvas, cbuf);
}

static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

    widget->state.battery = state.level;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    return (struct battery_status_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

static void set_output_status(struct zmk_widget_status *widget,
                              const struct output_status_state *state) {
    widget->state.selected_endpoint = state->selected_endpoint;
    widget->state.active_profile_index = state->active_profile_index;
    widget->state.active_profile_connected = state->active_profile_connected;
    widget->state.active_profile_bonded = state->active_profile_bonded;

    draw_top(widget->obj, widget->cbuf, &widget->state);
    draw_middle(widget->obj, widget->cbuf2, &widget->state);
}

static void output_status_update_cb(struct output_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_output_status(widget, &state); }
}

static struct output_status_state output_status_get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoint_get_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
#endif

static void set_layer_status(struct zmk_widget_status *widget, struct layer_status_state state) {
    widget->state.layer_index = state.index;
    widget->state.layer_label = state.label;

    draw_bottom(widget->obj, widget->cbuf3, &widget->state);
}

static void layer_status_update_cb(struct layer_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_layer_status(widget, state); }
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    uint8_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state){.index = index, .label = zmk_keymap_layer_name(index)};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_status_get_state)

ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

static void set_wpm_status(struct zmk_widget_status *widget, struct wpm_status_state state) {
    for (int i = 0; i < 9; i++) {
        widget->state.wpm[i] = widget->state.wpm[i + 1];
    }
    widget->state.wpm[9] = state.wpm;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void wpm_status_update_cb(struct wpm_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_wpm_status(widget, state); }
}

struct wpm_status_state wpm_status_get_state(const zmk_event_t *eh) {
    return (struct wpm_status_state){.wpm = zmk_wpm_get_state()};
};

ZMK_DISPLAY_WIDGET_LISTENER(widget_wpm_status, struct wpm_status_state, wpm_status_update_cb,
                            wpm_status_get_state)
ZMK_SUBSCRIPTION(widget_wpm_status, zmk_wpm_state_changed);

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 144, 72);

    // top battery status and output, wpm status
    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_COLOR_FORMAT_NATIVE);
    // middle connecion status
    lv_obj_t *middle = lv_canvas_create(widget->obj);
    lv_obj_align(middle, LV_ALIGN_TOP_LEFT, 68, 0);
    lv_canvas_set_buffer(middle, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, LV_COLOR_FORMAT_NATIVE);
    // bottom layer status
    lv_obj_t *bottom = lv_canvas_create(widget->obj);
    lv_obj_align(bottom, LV_ALIGN_TOP_LEFT, 128, 0);
    lv_canvas_set_buffer(bottom, widget->cbuf3, CANVAS_SIZE, CANVAS_SIZE, LV_COLOR_FORMAT_NATIVE);

    /*
     * Same as the peripheral half: canvas content lands 90 degrees clockwise
     * on this panel, while normal LVGL objects are upright, so rotate the
     * canvases back in the draw pipeline (never in the buffer, which the
     * display thread reads asynchronously).
     */
    lv_obj_t *canvases[] = {top, middle, bottom};
    for (int i = 0; i < 3; i++) {
        lv_image_set_pivot(canvases[i], CANVAS_SIZE / 2, CANVAS_SIZE / 2);
        lv_image_set_rotation(canvases[i], -900);
    }

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_output_status_init();
    widget_layer_status_init();
    widget_wpm_status_init();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
