#pragma once
#include "flipperclaw.h"
#include <gui/view.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ui_chat.h
 * @brief Chat view — scrollable streaming response display.
 *
 * Layout (128×64):
 *   y=0..10   Header: "FlipperClaw" left, status dot right
 *   y=11      1px horizontal divider
 *   y=13..20  Prompt preview (last sent prompt, truncated to 21 chars)
 *   y=22..54  Response text — word-wrapped, 4 lines × 21 chars, scrollable
 *   y=56..63  Footer: "↑↓ scroll  OK=new" or "thinking..." when waiting
 */

/**
 * @brief Allocate and return the chat View.
 *
 * @param app  Application context (used in callbacks).
 * @return Allocated View* (must be freed with view_free()).
 */
View* ui_chat_alloc(FlipperClawApp* app);

/**
 * @brief Append text to the response buffer and refresh the view.
 *
 * Thread-safe — may be called from any context; posts a view update.
 *
 * @param view     Chat view returned by ui_chat_alloc().
 * @param text     Null-terminated text to append.
 */
void ui_chat_append(View* view, const char* text);

/**
 * @brief Mark the response as complete (stop the "thinking..." animation).
 *
 * @param view  Chat view.
 */
void ui_chat_set_done(View* view);

/**
 * @brief Set waiting state (show "thinking..." footer).
 *
 * @param view     Chat view.
 * @param waiting  True to show animation, false to show normal footer.
 */
void ui_chat_set_waiting(View* view, bool waiting);

/**
 * @brief Update the UART connection indicator dot.
 *
 * @param view       Chat view.
 * @param connected  True = filled dot, false = hollow dot.
 */
void ui_chat_set_uart_ok(View* view, bool connected);

/**
 * @brief Set the status message shown in the footer area.
 *
 * @param view  Chat view.
 * @param text  Null-terminated status text (max 63 chars).
 */
void ui_chat_set_status(View* view, const char* text);

/**
 * @brief Update the prompt preview line.
 *
 * @param view  Chat view.
 * @param text  Null-terminated prompt (truncated to FC_DISPLAY_LINE_CHARS).
 */
void ui_chat_set_prompt_preview(View* view, const char* text);

/**
 * @brief Clear the response buffer.
 *
 * @param view  Chat view.
 */
void ui_chat_clear(View* view);

/**
 * @brief Advance the animation tick counter (call from ViewDispatcher tick callback).
 *
 * @param view  Chat view.
 */
void ui_chat_tick(View* view);

#ifdef __cplusplus
}
#endif
