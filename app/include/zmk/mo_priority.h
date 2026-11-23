#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Return 1 for a newly tracked press, 0 for duplicate, negative on failure. */
int zmk_mo_priority_press(uint8_t source, uint32_t position, uint8_t layer);
/* Return 1 when normal layer deactivation is needed, 0 when it must be skipped. */
int zmk_mo_priority_release(uint8_t source, uint32_t position, uint8_t layer);
int zmk_mo_priority_current(void);
void zmk_mo_priority_notify_display(void);
