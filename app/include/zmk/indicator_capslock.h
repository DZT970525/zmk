#ifndef __INDICATOR_CAOSLOCK__
#define __INDICATOR_CAPSLOCK__

uint8_t zmk_indicator_capslock_get_brt();
uint8_t zmk_indicator_capslock_calc_brt_cycle();
int zmk_indicator_capslock_set_brt(uint8_t brightness);
#endif