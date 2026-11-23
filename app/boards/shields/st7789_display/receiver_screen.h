#pragma once
#include <stdbool.h>
#include <stdint.h>
uint8_t *receiver_screen_usb_begin(unsigned slot, unsigned total, uint32_t crc);
int receiver_screen_usb_commit(unsigned slot, unsigned count);
void receiver_screen_usb_abort(void);
void receiver_screen_note_input(void);
void receiver_screen_set_idle(bool idle);
void receiver_backlight_set_idle(bool idle, unsigned char brightness);
void receiver_backlight_preview(unsigned char brightness);
bool receiver_screen_draw_picture(void);
bool receiver_screen_is_idle(void);

void receiver_backlight_boot_ready(void);
void receiver_screen_boot_complete(void);
