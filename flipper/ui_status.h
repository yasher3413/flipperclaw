#pragma once
#include "flipperclaw.h"
#include <gui/view.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ui_status.h
 * @brief Status view — WiFi, model, memory, and last error display.
 *
 * Layout:
 *   WiFi:      connected | disconnected
 *   Model:     anthropic/claude-haiku-4-5
 *   Memory:    4.2 KB used
 *   Last err:  NO_WIFI
 *   [Clear Memory]  [Back]
 */

/**
 * @brief Allocate and return the status View.
 * @param app  Application context.
 */
View* ui_status_alloc(FlipperClawApp* app);

/**
 * @brief Update the displayed status fields.
 *
 * @param view         Status view.
 * @param wifi_ok      True if WiFi is connected.
 * @param provider     LLM provider string (e.g. "anthropic").
 * @param model        Model name string (e.g. "claude-haiku-4-5-20251001").
 * @param memory_kb    SPIFFS used kilobytes.
 * @param last_error   Last error code string (may be empty).
 */
void ui_status_update(View* view,
                      bool wifi_ok,
                      const char* provider,
                      const char* model,
                      float memory_kb,
                      const char* last_error);

#ifdef __cplusplus
}
#endif
