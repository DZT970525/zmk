#ifndef A320_INPUT_HANDLER_H
#define A320_INPUT_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/**
 * @brief check if the touchpad is touched
 *
 * @return true if touched
 */
bool tp_is_touched(void);

#ifdef __cplusplus
}
#endif

#endif // A320_INPUT_HANDLER_H
