#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 获取当前指示灯应该使用的亮度（非 Caps Lock 呼吸模式下）
 *
 * @return uint8_t 有效亮度值
 */
uint8_t indicator_tp_get_last_valid_brightness(void);

#ifdef __cplusplus
}
#endif
