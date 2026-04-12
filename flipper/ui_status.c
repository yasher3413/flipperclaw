/**
 * @file ui_status.c
 * @brief Status view — WiFi, model, memory, last error.
 */

#include "ui_status.h"
#include <gui/canvas.h>
#include <input/input.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TAG "ui_status"

// ---------------------------------------------------------------------------
// View model
// ---------------------------------------------------------------------------

typedef struct {
    bool  wifi_ok;
    char  provider[24];
    char  model[48];
    float memory_kb;
    char  last_error[32];
    int   selected;   // 0 = Clear Memory button, 1 = Back button
} StatusModel;

// ---------------------------------------------------------------------------
// Draw callback
// ---------------------------------------------------------------------------

static void status_draw(Canvas* canvas, void* model_ptr) {
    StatusModel* m = (StatusModel*)model_ptr;
    char buf[64];

    canvas_clear(canvas);

    // Header
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Status");
    canvas_draw_line(canvas, 0, 11, 127, 11);

    canvas_set_font(canvas, FontSecondary);

    // WiFi row
    snprintf(buf, sizeof(buf), "WiFi: %s", m->wifi_ok ? "connected" : "disconnected");
    canvas_draw_str(canvas, 0, 22, buf);

    // Model row (truncate to fit)
    char model_short[32];
    snprintf(model_short, sizeof(model_short), "%s", m->model);
    snprintf(buf, sizeof(buf), "Model: %.22s", model_short);
    canvas_draw_str(canvas, 0, 31, buf);

    // Memory row
    snprintf(buf, sizeof(buf), "Memory: %.1f KB", m->memory_kb);
    canvas_draw_str(canvas, 0, 40, buf);

    // Last error row
    if(m->last_error[0]) {
        snprintf(buf, sizeof(buf), "Last err: %s", m->last_error);
    } else {
        snprintf(buf, sizeof(buf), "Last err: none");
    }
    canvas_draw_str(canvas, 0, 49, buf);

    // Buttons row
    canvas_draw_line(canvas, 0, 53, 127, 53);

    // "Clear Memory" button
    if(m->selected == 0) {
        canvas_draw_rbox(canvas, 0, 55, 74, 9, 2);
        canvas_invert_color(canvas);
        canvas_draw_str(canvas, 2, 63, "Clear Memory");
        canvas_invert_color(canvas);
    } else {
        canvas_draw_rframe(canvas, 0, 55, 74, 9, 2);
        canvas_draw_str(canvas, 2, 63, "Clear Memory");
    }

    // "Back" button
    if(m->selected == 1) {
        canvas_draw_rbox(canvas, 78, 55, 49, 9, 2);
        canvas_invert_color(canvas);
        canvas_draw_str(canvas, 80, 63, "Back");
        canvas_invert_color(canvas);
    } else {
        canvas_draw_rframe(canvas, 78, 55, 49, 9, 2);
        canvas_draw_str(canvas, 80, 63, "Back");
    }
}

// ---------------------------------------------------------------------------
// Input callback
// ---------------------------------------------------------------------------

static bool status_input(InputEvent* event, void* ctx) {
    FlipperClawApp* app = (FlipperClawApp*)ctx;
    furi_assert(app);

    if(event->type != InputTypeShort) return false;

    if(event->key == InputKeyLeft || event->key == InputKeyRight) {
        with_view_model(app->view_status, StatusModel* m, {
            m->selected = (m->selected == 0) ? 1 : 0;
        }, true);
        return true;
    }

    if(event->key == InputKeyOk) {
        int sel = 0;
        with_view_model(app->view_status, StatusModel* m, {
            sel = m->selected;
        }, false);

        if(sel == 0) {
            // Clear Memory — stub (log only in Phase 1)
            FURI_LOG_I(TAG, "Clear Memory requested (stub)");
            // In Phase 2: send CLEAR_MEMORY command to ESP32
        } else {
            view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdChat);
        }
        return true;
    }

    if(event->key == InputKeyBack) {
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdChat);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

View* ui_status_alloc(FlipperClawApp* app) {
    View* view = view_alloc();
    view_set_context(view, app);
    view_allocate_model(view, ViewModelTypeLocking, sizeof(StatusModel));
    view_set_draw_callback(view, status_draw);
    view_set_input_callback(view, status_input);

    with_view_model(view, StatusModel* m, {
        memset(m, 0, sizeof(StatusModel));
        strncpy(m->provider, "anthropic", sizeof(m->provider) - 1);
        strncpy(m->model, "claude-haiku-4-5-20251001", sizeof(m->model) - 1);
    }, false);

    return view;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ui_status_update(View* view,
                      bool wifi_ok,
                      const char* provider,
                      const char* model,
                      float memory_kb,
                      const char* last_error) {
    furi_assert(view);
    with_view_model(view, StatusModel* m, {
        m->wifi_ok    = wifi_ok;
        m->memory_kb  = memory_kb;
        if(provider)   strncpy(m->provider,   provider,   sizeof(m->provider)   - 1);
        if(model)      strncpy(m->model,      model,      sizeof(m->model)      - 1);
        if(last_error) strncpy(m->last_error, last_error, sizeof(m->last_error) - 1);
    }, true);
}
