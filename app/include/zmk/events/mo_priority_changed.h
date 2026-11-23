#pragma once
#include <stdint.h>
#include <zmk/event_manager.h>
/* Visual priority changed; unlike layer_state_changed, no active bit changed. */
struct zmk_mo_priority_changed { uint8_t unused; };
ZMK_EVENT_DECLARE(zmk_mo_priority_changed);
