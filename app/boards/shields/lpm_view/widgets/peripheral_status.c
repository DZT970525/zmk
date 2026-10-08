/*
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>

#include "peripheral_status.h"
#include "screen_config.h"
#include "screen_transfer.h"
#include "screen_image_store.h"
#include <string.h>
#include "transmit_image.h"

// ==================== 图片声明 ====================
LV_IMAGE_DECLARE(cat);
LV_IMAGE_DECLARE(astronaut);
LV_IMAGE_DECLARE(blackhole);
LV_IMAGE_DECLARE(kanagawa);
LV_IMAGE_DECLARE(winnie);
LV_IMAGE_DECLARE(red_blue_yellow);
LV_IMAGE_DECLARE(panda);
LV_IMAGE_DECLARE(king);

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

// ==================== 状态结构体 ====================
struct peripheral_status_state {
    bool connected;
};

// ==================== 图片数组 ====================
static const lv_image_dsc_t *bunny_frames[] = {
    &cat,    &astronaut,       &blackhole, &kanagawa, &winnie,
    &red_blue_yellow, &panda, &king,
};

#define BUNNY_FRAME_COUNT (sizeof(bunny_frames) / sizeof(bunny_frames[0]))

// ==================== 图片切换控制 ====================
static uint8_t current_img_index = 0;
static struct k_work_delayable img_switch_work;
static bool img_switch_work_ready;


#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* Bit i of the mask is bunny_frames[i]; 0 means "every picture", so a bad
 * value cannot leave the screen stuck on one image forever. */
static uint16_t carousel_picture_mask(void) {
    uint16_t mask = zitaotech_screen_enabled_mask();
    return mask == 0 ? (uint16_t)BIT_MASK(BUNNY_FRAME_COUNT) : mask;
}

static bool carousel_picture_enabled(uint8_t index) {
    if (index >= BUNNY_FRAME_COUNT && !zitaotech_screen_image_data(index-BUNNY_FRAME_COUNT)) return false;
    return (carousel_picture_mask() & BIT(index)) != 0;
}

/** Next picture to show: a random enabled one that is not the current one. */
static uint8_t carousel_first_index(void) {
    uint8_t packed[5]; zitaotech_screen_playback_order(packed);
    for(unsigned i=0;i<10;i++){unsigned id=(packed[i/2]>>(i%2?0:4))&15;if(carousel_picture_enabled(id))return id;}
    return 0;
}
static uint8_t carousel_pick_index(void) {
    if (zitaotech_screen_sequential()) {
        uint8_t packed[5], order[10]; zitaotech_screen_playback_order(packed);
        unsigned start=9; for(unsigned i=0;i<10;i++){ order[i]=(packed[i/2]>>(i%2?0:4))&15; if(order[i]==current_img_index)start=i; }
        for(unsigned step=1;step<=10;step++){unsigned id=order[(start+step)%10];if(carousel_picture_enabled(id))return id;}
        return 0;
    }
    uint8_t candidates[BUNNY_FRAME_COUNT+2];
    unsigned count=0;
    for (uint8_t i=0; i<BUNNY_FRAME_COUNT+2; i++)
        if (carousel_picture_enabled(i) && i != current_img_index) candidates[count++]=i;
    if (count) return candidates[sys_rand32_get()%count];
    return carousel_picture_enabled(current_img_index) ? current_img_index : 0;
}
static uint8_t user_pixels[2][64+4320];
static lv_image_dsc_t user_descriptors[2];
static void carousel_show(struct zmk_widget_status *widget, uint8_t index) {
    lv_obj_t *art=lv_obj_get_child(widget->obj,1);
    if (index < BUNNY_FRAME_COUNT) { lv_image_set_src(art,bunny_frames[index]); return; }
    unsigned slot=index-BUNNY_FRAME_COUNT;
    struct zitaotech_screen_image_info info;
    const uint8_t *data=zitaotech_screen_image_data(slot);
    if (!data || !zitaotech_screen_image_info(slot,&info)) return;
    unsigned palette_size=info.format == 1 ? 8 : 64;
    memset(user_pixels[slot],0,palette_size);
    for (unsigned i=0; i<(info.format == 1 ? 2 : 16); i++) {
        unsigned color=info.format == 1 ? (i ? 7 : 0) : i;
        user_pixels[slot][i*4]=(color&1) ? 255 : 0;
        user_pixels[slot][i*4+1]=(color&2) ? 255 : 0;
        user_pixels[slot][i*4+2]=(color&4) ? 255 : 0;
        user_pixels[slot][i*4+3]=255;
    }
    memcpy(user_pixels[slot]+palette_size,data,info.size);
    user_descriptors[slot]=(lv_image_dsc_t){
        .header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=info.format == 1 ? LV_COLOR_FORMAT_I1 : LV_COLOR_FORMAT_I4,
                 .w=120,.h=72,.stride=info.format == 1 ? 15 : 60},
        .data_size=palette_size+info.size,.data=user_pixels[slot]};
    lv_image_set_src(art,&user_descriptors[slot]);
    lv_obj_invalidate(art);
}
static void carousel_schedule(void) {
    if (img_switch_work_ready) {
        k_work_reschedule_for_queue(zmk_display_work_q(), &img_switch_work, K_MSEC(zitaotech_screen_interval_ms()));
    }
}

#endif /* peripheral half */

/*
 * ZitaoTech: this screen is now used by both halves.  The central half keeps
 * the endpoint/profile fields in struct status_state instead of the
 * peripheral's `connected` flag, so derive the "connected" state from
 * whichever one the current role provides.
 */
static inline bool status_connected(const struct status_state *state) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    return state->selected_endpoint.transport != ZMK_TRANSPORT_NONE;
#else
    return state->connected;
#endif
}

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* ---- central half: live status text instead of the picture carousel ---- */
static void draw_top(lv_obj_t *widget, uint16_t cbuf[], const struct status_state *state);
static void draw_layer_name(struct zmk_widget_status *widget,
                            const struct status_state *state);
static void draw_profile_slots(struct zmk_widget_status *widget,
                               const struct status_state *state);
#include <stdio.h>
#include <zmk/keymap.h>
#include <zmk/wpm.h>
#include <zmk/endpoints.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/ble_active_profile_changed.h>

static lv_obj_t *central_layer_label;
static lv_obj_t *central_device_label;
static lv_obj_t *central_wpm_label;
/* Captured at init so the draw helpers do not depend on the child order. */
static lv_obj_t *central_layer_canvas;
static lv_obj_t *central_slots_canvas;

/*
 * Output status (USB / BLE profile state) for the central half.
 *
 * This has to be a *display* widget listener: a plain event listener runs in
 * whichever thread raises the event - the sensor thread when the BLE profile is
 * changed with the encoder, the USB work queue when the cable is unplugged -
 * and drawing into LVGL from there races with the display thread and locks the
 * keyboard up.  ZMK_DISPLAY_WIDGET_LISTENER defers the callback to the display
 * work queue instead.
 */
struct output_status_state {
    int unused;
};

/* LVGL work stays on the display queue, with no intentional slot delay. */
static bool central_output_dirty;
static int central_drawn_profile = -1;
static void central_status_refresh(struct k_work *work) {
    ARG_UNUSED(work);
    bool redraw_output = central_output_dirty;
    central_output_dirty = false;
    int profile = zmk_ble_active_profile_index();
    bool redraw_profile = profile != central_drawn_profile;
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.active_profile_index = profile;
        if (redraw_profile) {
            draw_profile_slots(widget, &widget->state);
        }
        if (redraw_output) {
            draw_top(widget->obj, widget->cbuf, &widget->state);
        }
    }
    central_drawn_profile = profile;
}
static K_WORK_DELAYABLE_DEFINE(central_status_refresh_work, central_status_refresh);

static void set_output_status(struct zmk_widget_status *widget,
                              struct output_status_state state) {
    ARG_UNUSED(state);

    ARG_UNUSED(widget);
    central_output_dirty = true;
    k_work_reschedule_for_queue(zmk_display_work_q(), &central_status_refresh_work,
                                K_NO_WAIT);
}

static void central_output_update_cb(struct output_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_output_status(widget, state); }
}

static struct output_status_state central_output_get_state(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    return (struct output_status_state){0};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            central_output_update_cb, central_output_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif

/* BLE profile change: only the profile slot canvas is redrawn (partial). */
static void central_profile_update_cb(struct output_status_state state) {
    ARG_UNUSED(state);

    k_work_reschedule_for_queue(zmk_display_work_q(), &central_status_refresh_work,
                                K_NO_WAIT);
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_profile_status, struct output_status_state,
                            central_profile_update_cb, central_output_get_state)
ZMK_SUBSCRIPTION(widget_profile_status, zmk_ble_active_profile_changed);

/*
 * Layer name, debounced: a layer change (every MO press/release) used to do a
 * full LVGL redraw immediately, which interfered with fast layer switching.
 * Now it schedules one update 120 ms later, so a quick tap collapses into a
 * single redraw instead of stalling the switch.
 */
static void layer_status_update_cb(struct output_status_state state) {
    ARG_UNUSED(state);
    /*
     * This callback runs on the display work queue, so it is safe to draw here.
     * Only the layer-name canvas is touched - that is what makes layer changes
     * cheap enough to do on every event without stalling the switch.
     */
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { draw_layer_name(widget, &widget->state); }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct output_status_state, layer_status_update_cb,
                            central_output_get_state)
ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);
#endif

// ================= 顶部绘制 =================
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static void draw_wpm(lv_obj_t *canvas, uint16_t cbuf[], const struct status_state *state);
#endif
static void draw_top(lv_obj_t *widget, uint16_t cbuf[], const struct status_state *state);

static void draw_top(lv_obj_t *widget, uint16_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    // Fill background
    lv_area_t area = {0, 0, CANVAS_SIZE - 1, CANVAS_SIZE - 1};
    lv_draw_rect(&layer, &rect_black_dsc, &area);

    // Draw connection / output icon
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /*
     * Central half: show the *preferred* output, exactly like the LVGL 8 build
     * did with zmk_endpoints_selected() - the preferred endpoint stays BLE while
     * an unpaired profile is advertising, whereas zmk_endpoint_get_selected()
     * reports "nothing" until a connection actually exists and the icon would
     * flip to X instead of the gear.
     */
    struct zmk_endpoint_instance endpoint = zmk_endpoint_get_selected();
    if (endpoint.transport == ZMK_TRANSPORT_NONE) {
        /* Nothing connected yet: fall back to the preferred transport so an
         * unpaired BLE profile still shows the gear instead of nothing. */
        endpoint = zmk_endpoint_get_preferred();
    }
    if (endpoint.transport == ZMK_TRANSPORT_USB) {
        label_dsc.color = LVGL_OUTPUT_USB;
        label_dsc.text = LV_SYMBOL_USB;
    } else if (endpoint.transport == ZMK_TRANSPORT_BLE) {
        if (zmk_ble_active_profile_is_connected()) {
            label_dsc.color = LVGL_OUTPUT_CONNECTED;
            label_dsc.text = LV_SYMBOL_WIFI;
        } else if (zmk_ble_active_profile_is_open()) {
            label_dsc.color = LVGL_OUTPUT_OPEN;
            label_dsc.text = LV_SYMBOL_SETTINGS;
        } else {
            label_dsc.color = LVGL_OUTPUT_DISCONNECTED;
            label_dsc.text = LV_SYMBOL_CLOSE;
        }
    } else {
        label_dsc.color = LVGL_OUTPUT_DISCONNECTED;
        label_dsc.text = LV_SYMBOL_CLOSE;
    }
#else
    label_dsc.color = status_connected(state) ? LVGL_OUTPUT_CONNECTED : LVGL_OUTPUT_DISCONNECTED;
    label_dsc.text = status_connected(state) ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE;
#endif
    area = (lv_area_t){0, 0, CANVAS_SIZE - 1, 19};
    lv_draw_label(&layer, &label_dsc, &area);

    lv_canvas_finish_layer(canvas, &layer);

    // Draw battery (its own layer)
    draw_battery(canvas, state);

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    // WPM box, graph and value (central half only)
    draw_wpm(canvas, cbuf, state);
#endif

    // Rotate canvas
    rotate_canvas(canvas, cbuf);
}

// ================= 图片切换函数 =================
static void switch_image(struct k_work *work) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* The central half has no picture carousel: its object children are the
     * status canvases, and setting an image source on those made the pictures
     * show up there every interval. */
    ARG_UNUSED(work);
    return;
#else
    struct zmk_widget_status *widget;

    /* Only the pictures ZitaoTech Studio left enabled take part in the
     * rotation; the interval comes from the same configuration. */
    if (zitaotech_screen_transfer_active()) { k_work_schedule_for_queue(zmk_display_work_q(), &img_switch_work, K_SECONDS(1)); return; }
    current_img_index = carousel_pick_index();

    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        carousel_show(widget, current_img_index);
    }

    k_work_schedule_for_queue(zmk_display_work_q(), &img_switch_work, K_MSEC(zitaotech_screen_interval_ms()));
#endif
}

/*
 * Called from screen_config.c when the Studio (or the other half) changes the
 * interval or which pictures are in the rotation.  A picture that was just
 * switched off has to leave the screen now, and either way the timer restarts
 * with the new interval.
 */
void zmk_widget_status_carousel_config_changed(void) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    return;
#else
    struct zmk_widget_status *widget;

    /*
     * A new configuration is exactly the moment the screen should visibly
     * react - both because the picture that was up may have been switched off
     * and because that is the only way to see on the device that the new
     * interval arrived.
     */
    current_img_index = zitaotech_screen_sequential() ? carousel_first_index() : carousel_pick_index();
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        carousel_show(widget, current_img_index);
    }

    carousel_schedule();
#endif
}

// ================= 电池状态 =================
static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif
    widget->state.battery = state.level;
    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    return (struct battery_status_state){
        .level = zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif

// ================= 连接状态 =================
static struct peripheral_status_state get_state(const zmk_event_t *eh) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* The central half reports its own endpoint state, see status_connected(). */
    ARG_UNUSED(eh);
    return (struct peripheral_status_state){.connected = true};
#else
    return (struct peripheral_status_state){.connected = zmk_split_bt_peripheral_is_connected()};
#endif
}

static void set_connection_status(struct zmk_widget_status *widget,
                                  struct peripheral_status_state state) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    ARG_UNUSED(widget);
    ARG_UNUSED(state);
#else
    widget->state.connected = state.connected;
    draw_top(widget->obj, widget->cbuf, &widget->state);
#endif
}

static void output_status_update_cb(struct peripheral_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_connection_status(widget, state); }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_status, struct peripheral_status_state,
                            output_status_update_cb, get_state)
ZMK_SUBSCRIPTION(widget_peripheral_status, zmk_split_peripheral_status_changed);

// ================= 初始化 =================
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* ---- WPM readout (central half) ---- */
struct wpm_status_state {
    uint8_t wpm;
};

/* Keep the existing circles and numbers, using rounded rectangles instead of
 * arc draw tasks. This path runs only after the BLE selection settles. */
static void draw_profile_slots(struct zmk_widget_status *widget,
                               const struct status_state *state) {
    lv_obj_t *canvas = central_slots_canvas;
    if (canvas == NULL) {
        return;
    }
    const int offsets[4][2] = {{18, 15}, {50, 15}, {18, 46}, {50, 46}};
    uint16_t background = lv_color_to_u16(LVGL_BACKGROUND);
    for (int i = 0; i < CANVAS_SIZE * CANVAS_SIZE; i++) {
        widget->cbuf3[i] = background;
    }
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    for (int slot = 0; slot < 4; slot++) {
        bool selected = slot == state->active_profile_index;
        int cx = offsets[slot][0];
        int cy = offsets[slot][1];
        lv_draw_rect_dsc_t circle;
        init_rect_dsc(&circle, selected ? lvgl_profile_color(slot) : LVGL_FOREGROUND);
        circle.radius = LV_RADIUS_CIRCLE;
        lv_area_t area = {cx - 13, cy - 13, cx + 12, cy + 12};
        lv_draw_rect(&layer, &circle, &area);
        init_rect_dsc(&circle, LVGL_BACKGROUND);
        circle.radius = LV_RADIUS_CIRCLE;
        area = (lv_area_t){cx - 11, cy - 11, cx + 10, cy + 10};
        lv_draw_rect(&layer, &circle, &area);
        if (selected) {
            init_rect_dsc(&circle, lvgl_profile_color(slot));
            circle.radius = LV_RADIUS_CIRCLE;
            area = (lv_area_t){cx - 9, cy - 9, cx + 8, cy + 8};
            lv_draw_rect(&layer, &circle, &area);
        }
        char label[2] = {(char)('1' + slot), '\0'};
        lv_draw_label_dsc_t label_dsc;
        init_label_dsc(&label_dsc, selected ? LVGL_BACKGROUND : LVGL_FOREGROUND,
                       &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER);
        label_dsc.text = label;
        area = (lv_area_t){cx - 8, cy - 10, cx + 7, cy + 7};
        lv_draw_label(&layer, &label_dsc, &area);
    }
    lv_canvas_finish_layer(canvas, &layer);
    rotate_canvas(canvas, widget->cbuf3);
    lv_obj_invalidate(canvas);
}
static void draw_layer_name(struct zmk_widget_status *widget,
                            const struct status_state *state) {
    ARG_UNUSED(state);

    lv_obj_t *canvas = central_layer_canvas;
    if (canvas == NULL) {
        return;
    }

    /* Background first (plain pixel writes), then the layer name as a label. */
    uint16_t layer_bg = lv_color_to_u16(LVGL_BACKGROUND);
    for (int i = 0; i < CANVAS_SIZE * CANVAS_SIZE; i++) {
        widget->cbuf2[i] = layer_bg;
    }

    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    const char *name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index));
    char text[24];
    if (name != NULL) {
        snprintf(text, sizeof(text), "%s", name);
    } else {
        snprintf(text, sizeof(text), "LAYER %d", (int)index);
    }

    lv_draw_label_dsc_t name_dsc;
    /* Same font and text box as the LVGL 8 build's draw_bottom(). */
    init_label_dsc(&name_dsc, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);
    name_dsc.text = text;

    lv_layer_t name_layer;
    lv_canvas_init_layer(canvas, &name_layer);
    lv_area_t name_area = {0, 0, CANVAS_SIZE - 1, 14};
    lv_draw_label(&name_layer, &name_dsc, &name_area);
    lv_canvas_finish_layer(canvas, &name_layer);

    /* Same rotation bake as the battery/WPM canvas (the old draw_bottom() did
     * this too), otherwise the layer name comes out sideways. */
    rotate_canvas(canvas, widget->cbuf2);

    lv_obj_invalidate(canvas);
    return;
}

static void draw_wpm(lv_obj_t *canvas, uint16_t cbuf[], const struct status_state *state) {
    lv_draw_rect_dsc_t rect_white_dsc;
    init_rect_dsc(&rect_white_dsc, LVGL_FOREGROUND);
    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_line_dsc_t line_dsc;
    init_line_dsc(&line_dsc, LVGL_FOREGROUND, 2);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    /* Shifted right by 3px: at x=0 the left border of the frame ended up on
     * the edge of the visible area and disappeared. */
    /* v0.3 geometry: 70x44 frame at y=21, inset by 2px so the left border is
     * not eaten by the panel edge. */
    lv_area_t area = {2, 21, 2 + 70 - 1, 21 + 44 - 1};
    lv_draw_rect(&layer, &rect_white_dsc, &area);
    area = (lv_area_t){3, 22, 3 + 68 - 1, 22 + 42 - 1};
    lv_draw_rect(&layer, &rect_black_dsc, &area);

    char wpm_text[6] = {};
    snprintf(wpm_text, sizeof(wpm_text), "%d", state->wpm[9]);
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_RIGHT);
    label_dsc.text = wpm_text;
    area = (lv_area_t){42, 52, 42 + 24 - 1, 52 + 8 - 1};
    lv_draw_label(&layer, &label_dsc, &area);

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

    lv_canvas_finish_layer(canvas, &layer);

    /*
     * The polyline is drawn with the regular LVGL call - the same thing the
     * LVGL 8 build did - in its own layer, exactly like draw_battery() works.
     * The redraw rate is throttled in set_wpm_status() so a fast typist cannot
     * flood the draw queue (which is what locked the display up before).
     */
    lv_point_t points[10];
    for (int i = 0; i < 10; i++) {
        points[i].x = 4 + i * 7;
        points[i].y = 62 - (int)(((float)(state->wpm[i] - min) * 38.0f) / range + 0.5f);
        /* Hard clamp into the frame: WPM swings can push the computed y past
         * the frame border, and those stray pixels ended up as a grey halftone
         * blob at the left edge of the WPM box. */
        if (points[i].y < 23) {
            points[i].y = 23;
        }
        if (points[i].y > 62) {
            points[i].y = 62;
        }
    }

    lv_layer_t graph_layer;
    lv_canvas_init_layer(canvas, &graph_layer);

    /* Clear the inside of the frame in this layer too, so the graph never
     * accumulates over the previous frame. */
    lv_draw_rect_dsc_t inner_bg_dsc;
    init_rect_dsc(&inner_bg_dsc, LVGL_BACKGROUND);
    lv_area_t inner_area = {4, 23, 4 + 64 - 1, 23 + 38 - 1};
    lv_draw_rect(&graph_layer, &inner_bg_dsc, &inner_area);

    lv_draw_line_dsc_t graph_dsc;
    init_line_dsc(&graph_dsc, LVGL_FOREGROUND, 2);
    for (int i = 0; i < 9; i++) {
        graph_dsc.p1.x = points[i].x;
        graph_dsc.p1.y = points[i].y;
        graph_dsc.p2.x = points[i + 1].x;
        graph_dsc.p2.y = points[i + 1].y;
        lv_draw_line(&graph_layer, &graph_dsc);
    }

    /* The WPM value has to be drawn *after* the graph layer's background
     * clear, otherwise the clear rect paints over it (it disappeared). */
    lv_draw_label(&graph_layer, &label_dsc, &area);

    lv_canvas_finish_layer(canvas, &graph_layer);

    lv_obj_invalidate(canvas);
}

static void set_wpm_status(struct zmk_widget_status *widget, struct wpm_status_state state) {
    for (int i = 0; i < 9; i++) {
        widget->state.wpm[i] = widget->state.wpm[i + 1];
    }
    widget->state.wpm[9] = state.wpm;

    /*
     * WPM events fire on every keypress.  Redrawing the whole canvas that
     * often stacks up more draw tasks than the queue can retire, which locked
     * the display up while typing; one redraw per 500 ms is plenty for a graph
     * that is read at a glance.
     */
    static int64_t last_redraw;
    int64_t now = k_uptime_get();
    if (now - last_redraw < 500) {
        return;
    }
    last_redraw = now;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void wpm_status_update_cb(struct wpm_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_wpm_status(widget, state); }
}

static struct wpm_status_state wpm_status_get_state(const zmk_event_t *eh) {
    return (struct wpm_status_state){.wpm = zmk_wpm_get_state()};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_wpm_status, struct wpm_status_state, wpm_status_update_cb,
                            wpm_status_get_state)
ZMK_SUBSCRIPTION(widget_wpm_status, zmk_wpm_state_changed);
#endif /* central half */

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 144, 72);

    // --- 顶部 canvas ---
    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_COLOR_FORMAT_NATIVE);
    /*
     * The status canvas comes out rotated 90 degrees clockwise on this panel
     * (the battery, which is drawn along the top-left edge, showed up on the
     * right edge standing upright), while normal LVGL objects like the picture
     * below are already upright.  Rotate the canvas back at the object level -
     * LVGL does this in its own draw pipeline, so the buffer is never touched
     * while the display is reading it.
     */

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* BLE profile slots (1-4): their own canvas, placed like the old layout. */
    lv_obj_t *middle = lv_canvas_create(widget->obj);
    /*
     * Same placement as the old firmware's layer name canvas: LVGL x = 128
     * (the panel maps LVGL x to the portrait panel's y axis, so this is the
     * strip along the bottom edge) - the canvas offset has to match, otherwise
     * the text lands somewhere else even though the in-canvas coordinates are
     * identical.
     */
    lv_obj_align(middle, LV_ALIGN_TOP_LEFT, 128, 0);
    lv_canvas_set_buffer(middle, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, LV_COLOR_FORMAT_NATIVE);

    /* BLE profile slots canvas - x=68 like the old firmware's middle canvas. */
    lv_obj_t *slots = lv_canvas_create(widget->obj);
    lv_obj_align(slots, LV_ALIGN_TOP_LEFT, 68, 0);
    lv_canvas_set_buffer(slots, widget->cbuf3, CANVAS_SIZE, CANVAS_SIZE, LV_COLOR_FORMAT_NATIVE);
    central_slots_canvas = slots;
    central_layer_canvas = middle;
    /* The old firmware created the layer canvas last, so it sat on top of the
     * slot canvas; keep that order so the layer name is not covered. */
    lv_obj_move_foreground(middle);
    /* NOTE: disabled for now - a third canvas with its own draw tasks locks this
     * panel up at boot.  See the next attempt in the notes / plan. */
#else
    // Initial selected image.
    current_img_index = zitaotech_screen_sequential() ? carousel_first_index() : carousel_pick_index();

    lv_obj_t *art = lv_image_create(widget->obj);
    int preferred=zitaotech_screen_preferred_image();
    if (!zitaotech_screen_sequential() && preferred >= 0 && carousel_picture_enabled(BUNNY_FRAME_COUNT+preferred)) current_img_index=BUNNY_FRAME_COUNT+preferred;
    carousel_show(widget,current_img_index);
    lv_obj_align(art, LV_ALIGN_TOP_LEFT, 20, 0);
#endif

    lv_obj_t *transmit=lv_image_create(widget->obj);
    lv_image_set_src(transmit,&transmit_image);
    lv_obj_center(transmit);
    lv_obj_add_flag(transmit,LV_OBJ_FLAG_HIDDEN);
    sys_slist_append(&widgets, &widget->node);

    widget_battery_status_init();
    widget_peripheral_status_init();
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    widget_wpm_status_init();
    widget_output_status_init();
    widget_profile_status_init();
    widget_layer_status_init();
#endif

    // 初始化定时器（只初始化一次）
    static bool work_initialized = false;
    if (!work_initialized) {
        k_work_init_delayable(&img_switch_work, switch_image);
        img_switch_work_ready = true;
        k_work_schedule_for_queue(zmk_display_work_q(), &img_switch_work, K_MSEC(zitaotech_screen_interval_ms()));
        work_initialized = true;
    }

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }

static void transfer_display_update(struct k_work *work) {
    struct zmk_widget_status *widget;
    bool active=zitaotech_screen_transfer_active();
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets,widget,node) {
        lv_obj_t *label=lv_obj_get_child(widget->obj,-1);
        if (active) { lv_obj_remove_flag(label,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(label); }
        else {
            lv_obj_add_flag(label,LV_OBJ_FLAG_HIDDEN);
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
            int slot=zitaotech_screen_preferred_image();
            if (slot >= 0 && carousel_picture_enabled(BUNNY_FRAME_COUNT+slot)) current_img_index=BUNNY_FRAME_COUNT+slot;
            else if (!carousel_picture_enabled(current_img_index)) current_img_index=carousel_pick_index();
            carousel_show(widget,current_img_index);
            carousel_schedule();
#endif
        }
    }
}
static K_WORK_DEFINE(transfer_display_work,transfer_display_update);
void zmk_widget_status_transfer_changed(void) {
    if (sys_slist_is_empty(&widgets)) return;
    k_work_submit_to_queue(zmk_display_work_q(),&transfer_display_work);
}






