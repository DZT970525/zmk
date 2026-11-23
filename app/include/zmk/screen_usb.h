#pragma once
#include <zephyr/sys/util.h>
#include <stdbool.h>
#include <stdint.h>
#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && IS_ENABLED(CONFIG_ZMK_STUDIO_RPC) && !IS_ENABLED(CONFIG_ZMK_SETTINGS_RESET_ON_START) && ((IS_ENABLED(CONFIG_ZITAOTECH_SCREEN_STUDIO_RPC) && IS_ENABLED(CONFIG_SHIELD_LPM_VIEW) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) || (IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER) && IS_ENABLED(CONFIG_SHIELD_ST7789_DISPLAY)))
struct zitaotech_usb_image_status {
    bool active, stored;
    uint8_t token, op, slot;
    uint16_t received, total;
    uint32_t crc;
    int error;
};
bool zitaotech_usb_image_feed(uint8_t byte);
bool zitaotech_usb_image_active(void);
void zitaotech_usb_image_status(struct zitaotech_usb_image_status *out);
bool zitaotech_screen_rpc_upload_active(void);
#else
static inline bool zitaotech_usb_image_active(void) { return false; }
#endif
