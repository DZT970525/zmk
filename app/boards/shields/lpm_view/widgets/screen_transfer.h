#pragma once
#include <zmk/screen_transfer.h>
struct zitaotech_image_transfer_status {
    bool active;
    uint32_t slot, sent, total, crc;
    int error;
};
int zitaotech_screen_image_sync(uint32_t slot);
int zitaotech_screen_image_delete(uint32_t slot);
void zitaotech_screen_image_transfer_status(struct zitaotech_image_transfer_status *out);
int zitaotech_screen_preferred_image(void);
void zmk_widget_status_transfer_changed(void);
int zitaotech_screen_image_fetch(uint32_t slot);
bool zitaotech_screen_image_read_ready(uint32_t slot);
void zitaotech_screen_image_read_finish(uint32_t slot);
