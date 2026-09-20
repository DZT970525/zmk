#ifndef A320_INPUT_HANDLER_H
#define A320_INPUT_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/**
 * @brief 查询当前 A320 是否被触碰（运动）。
 *
 * @return true 如果当前有运动（触摸）；false 否则。
 */
bool tp_is_touched(void);

#ifdef __cplusplus
}
#endif

#endif // A320_INPUT_HANDLER_H
