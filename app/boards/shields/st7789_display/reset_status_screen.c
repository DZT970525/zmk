#include <lvgl.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/retention/bootmode.h>
#include <zmk/bootloader_feedback.h>

/* settings_reset selects a mock scanner. The receiver's physical button must
 * remain usable after erasing settings, independently of any saved keymap. */
static void reset_button_bootloader(struct k_work *work) {
    if (bootmode_set(BOOT_MODE_TYPE_BOOTLOADER) == 0) {
        zmk_bootloader_feedback_reboot(SYS_REBOOT_COLD);
    }
}
K_WORK_DEFINE(reset_button_work, reset_button_bootloader);

static void reset_button_pressed(const struct device *dev, uint32_t row,
                                 uint32_t column, bool pressed) {
    if (pressed && row == 0 && column == 0) {
        k_work_submit(&reset_button_work);
    }
}

static int reset_button_init(void) {
    const struct device *scanner = DEVICE_DT_GET(DT_NODELABEL(kscan0));
    if (!device_is_ready(scanner)) {
        return -ENODEV;
    }
    int err = kscan_config(scanner, reset_button_pressed);
    return err ? err : kscan_enable_callback(scanner);
}
SYS_INIT(reset_button_init, APPLICATION, 99);
extern int zmk_settings_reset_result(void);
lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_width(label, 208);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, zmk_settings_reset_result() == 0
        ? "The firmware has been reset, now flash the normal firmware"
        : "Settings reset failed. Please flash the reset firmware again.");
    lv_obj_center(label);
    return screen;
}
