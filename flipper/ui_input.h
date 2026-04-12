#pragma once
#include "flipperclaw.h"
#include <gui/view.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ui_input.h
 * @brief D-pad character picker for composing prompts.
 *
 * Character layout (4 rows):
 *   Row 0: digits        0-9
 *   Row 1: lowercase     a-z + space
 *   Row 2: uppercase     A-Z
 *   Row 3: symbols+space . , ! ? @ # $ % & * ( ) - _ + = / \\ : ; ' " < > [ ]
 *
 * Controls:
 *   UP / DOWN   — cycle character at cursor position
 *   LEFT / RIGHT — move cursor left / right in the input string
 *   OK          — send prompt, switch to chat view
 *   BACK        — discard, return to chat view
 */

/**
 * @brief Allocate and return the input View.
 *
 * @param app  Application context.
 * @return Allocated View*.
 */
View* ui_input_alloc(FlipperClawApp* app);

/**
 * @brief Clear the input buffer (called after send or cancel).
 *
 * @param view  Input view.
 */
void ui_input_clear(View* view);

/**
 * @brief Get the current input string (null-terminated).
 *
 * Copies up to buf_size-1 bytes into buf.
 *
 * @param view      Input view.
 * @param buf       Output buffer.
 * @param buf_size  Size of output buffer.
 */
void ui_input_get_text(View* view, char* buf, size_t buf_size);

#ifdef __cplusplus
}
#endif
