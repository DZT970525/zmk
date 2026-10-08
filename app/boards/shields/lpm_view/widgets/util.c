/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>
#include "util.h"

LV_IMAGE_DECLARE(bolt);

void rotate_canvas(lv_obj_t *canvas, uint16_t cbuf[]) {
    /*
     * Orientation that has been verified on this panel: draw the canvas buffer
     * back into itself through the draw image path with a -90 degree rotation
     * (the LVGL 8 code did the same with lv_canvas_transform()).
     */
    static uint16_t cbuf_tmp[CANVAS_SIZE * CANVAS_SIZE];
    memcpy(cbuf_tmp, cbuf, sizeof(cbuf_tmp));

    lv_image_dsc_t img;
    memset(&img, 0, sizeof(img));
    img.header.magic = LV_IMAGE_HEADER_MAGIC;
    img.header.cf = LV_COLOR_FORMAT_NATIVE;
    img.header.w = CANVAS_SIZE;
    img.header.h = CANVAS_SIZE;
    img.data = (void *)cbuf_tmp;
    img.data_size = sizeof(cbuf_tmp);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_image_dsc_t img_dsc;
    lv_draw_image_dsc_init(&img_dsc);
    img_dsc.src = &img;
    img_dsc.rotation = -900;
    img_dsc.pivot.x = CANVAS_SIZE / 2;
    img_dsc.pivot.y = CANVAS_SIZE / 2 - 1;

    lv_area_t area = {0, 0, CANVAS_SIZE - 1, CANVAS_SIZE - 1};
    lv_draw_image(&layer, &img_dsc, &area);

    lv_canvas_finish_layer(canvas, &layer);

    lv_obj_invalidate(canvas);
}

void draw_battery(lv_obj_t *canvas, const struct status_state *state) {
    /* At least one pixel of charge so that a nearly empty battery is still visible */
    uint8_t charge = (state->battery + 2) / 4;
    if (state->battery > 0 && charge == 0) {
        charge = 1;
    }

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t rect_frame_dsc;
    init_rect_dsc(&rect_frame_dsc, LVGL_FOREGROUND);
    lv_draw_rect_dsc_t rect_level_dsc;
    init_rect_dsc(&rect_level_dsc, lvgl_battery_level_color(state->battery));

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    /*
     * The panel drops the canvas' leftmost column, so a frame starting at x=0
     * loses its left border (the same thing the WPM frame hit).  Both halves
     * draw the same battery, so both inset it by 3px.
     */
    const int bx = 3;

    /* Battery frame and the + terminal stay white */
    lv_area_t area = {bx + 0, 2, bx + 0 + 29 - 1, 2 + 12 - 1};
    lv_draw_rect(&layer, &rect_frame_dsc, &area);
    area = (lv_area_t){bx + 1, 3, bx + 1 + 27 - 1, 3 + 10 - 1};
    lv_draw_rect(&layer, &rect_black_dsc, &area);
    area = (lv_area_t){bx + 30, 5, bx + 30 + 3 - 1, 5 + 6 - 1};
    lv_draw_rect(&layer, &rect_frame_dsc, &area);
    area = (lv_area_t){bx + 31, 6, bx + 31 + 1 - 1, 6 + 4 - 1};
    lv_draw_rect(&layer, &rect_black_dsc, &area);

    /* Remaining charge: green when full, yellow when half, red when low */
    area = (lv_area_t){bx + 2, 4, bx + 2 + charge - 1, 4 + 8 - 1};
    lv_draw_rect(&layer, &rect_level_dsc, &area);

    if (state->charging) {
        lv_draw_image_dsc_t img_dsc;
        lv_draw_image_dsc_init(&img_dsc);
        img_dsc.src = &bolt;
        lv_area_t img_area = {bx + 9, -1, bx + 9 + 11 - 1, -1 + 18 - 1};
        lv_draw_image(&layer, &img_dsc, &img_area);
    }

    lv_canvas_finish_layer(canvas, &layer);
}

void init_label_dsc(lv_draw_label_dsc_t *label_dsc, lv_color_t color, const lv_font_t *font,
                    lv_text_align_t align) {
    lv_draw_label_dsc_init(label_dsc);
    /*
     * All the labels on these screens are built in local buffers (the layer
     * name, the BLE slot numbers, the WPM value, ...).  LVGL 9 only stores the
     * pointer in the draw descriptor and renders the text later, when the draw
     * tasks run in lv_canvas_finish_layer() - by then those locals are gone.
     * text_local makes LVGL copy the string when the task is created.
     */
    label_dsc->text_local = 1;
    label_dsc->color = color;
    label_dsc->font = font;
    label_dsc->align = align;
}

void init_rect_dsc(lv_draw_rect_dsc_t *rect_dsc, lv_color_t bg_color) {
    lv_draw_rect_dsc_init(rect_dsc);
    rect_dsc->bg_color = bg_color;
}

void init_line_dsc(lv_draw_line_dsc_t *line_dsc, lv_color_t color, uint8_t width) {
    lv_draw_line_dsc_init(line_dsc);
    line_dsc->color = color;
    line_dsc->width = width;
}

void init_arc_dsc(lv_draw_arc_dsc_t *arc_dsc, lv_color_t color, uint8_t width) {
    lv_draw_arc_dsc_init(arc_dsc);
    arc_dsc->color = color;
    arc_dsc->width = width;
}
