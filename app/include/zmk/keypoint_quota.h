/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
bool zmk_keypoint_quota_key(uint32_t position, bool pressed);
bool zmk_keypoint_quota_cycle(void);
int zmk_keypoint_quota_relay_receive(const uint8_t *bytes, size_t length);
void zmk_keypoint_quota_clear(void);
