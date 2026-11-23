#include <lvgl.h>

extern int zmk_settings_reset_result(void);

/* Bake the portrait text into the native 144x72 framebuffer. Object transforms
 * can clip the tall wrapped label before rotation on the narrow display. */
static uint16_t portrait_pixels[72 * 144];
static uint16_t panel_pixels[144 * 72];

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x00ffff), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(canvas, portrait_pixels, 72, 144, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(canvas, lv_color_hex(0x00ffff), LV_OPA_COVER);

    lv_draw_label_dsc_t text;
    lv_draw_label_dsc_init(&text);
    text.font = &lv_font_unscii_8;
    text.color = lv_color_black();
    text.align = LV_TEXT_ALIGN_CENTER;
    text.text = zmk_settings_reset_result() == 0
        ? "The firmware has been reset, now flash the normal firmware"
        : "Settings reset failed. Please flash the reset firmware again.";
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_point_t text_size;
    lv_text_get_size(&text_size, text.text, text.font, text.letter_space,
                     text.line_space, 64, LV_TEXT_FLAG_NONE);
    int32_t text_top = LV_MAX(0, (144 - text_size.y) / 2);
    lv_area_t area = {4, text_top, 67, 143};
    lv_draw_label(&layer, &text, &area);
    lv_canvas_finish_layer(canvas, &layer);

    /* Counterclockwise 90 degrees: physical panel rotates this back upright. */
    for (int y = 0; y < 144; y++) {
        for (int x = 0; x < 72; x++) {
            panel_pixels[(71 - x) * 144 + y] = portrait_pixels[y * 72 + x];
        }
    }
    lv_canvas_set_buffer(canvas, panel_pixels, 144, 72, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas);
    return screen;
}
