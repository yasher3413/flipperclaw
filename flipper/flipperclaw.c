/**
 * @file flipperclaw.c
 * @brief FlipperClaw .fap entry point — lifecycle, event loop, view wiring.
 *
 * Startup sequence:
 *   1. Allocate FlipperClawApp and FuriMessageQueue
 *   2. Allocate and register views with ViewDispatcher
 *   3. Initialise UART bridge (Furi HAL UART, RX thread)
 *   4. Start PING timer (every 10 s)
 *   5. Run ViewDispatcher event loop (blocks until exit)
 *   6. Teardown: stop timer, deinit UART, free views, free app
 */

#include "flipperclaw.h"
#include "uart_bridge.h"
#include "ui_chat.h"
#include "ui_input.h"
#include "ui_status.h"

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <stdlib.h>
#include <string.h>

#define TAG "flipperclaw"

// ---------------------------------------------------------------------------
// Heartbeat timer callback
// ---------------------------------------------------------------------------

static void ping_timer_cb(void* ctx) {
    FlipperClawApp* app = (FlipperClawApp*)ctx;
    uart_send_ping(app);

    // Check PONG timeout
    uint32_t now = furi_get_tick();
    bool was_connected = app->uart_connected;
    bool still_connected = (now - app->last_pong_tick) < FC_PONG_TIMEOUT_MS;

    if(was_connected != still_connected) {
        app->uart_connected = still_connected;
        ui_chat_set_uart_ok(app->view_chat, still_connected);
        FURI_LOG_I(TAG, "UART %s", still_connected ? "connected" : "disconnected");
    }
}

// ---------------------------------------------------------------------------
// Event queue drain — called from both custom_event_cb and tick_cb
// ---------------------------------------------------------------------------

static void drain_event_queue(FlipperClawApp* app) {
    AppEvent evt;
    while(furi_message_queue_get(app->event_queue, &evt, 0) == FuriStatusOk) {
        switch(evt.type) {

        case AppEventTypeUartChunk:
            ui_chat_append(app->view_chat, evt.data.chunk.text);
            free(evt.data.chunk.text); // heap-allocated in uart_bridge.c
            break;

        case AppEventTypeUartDone:
            app->waiting_response = false;
            ui_chat_set_done(app->view_chat);
            break;

        case AppEventTypeUartError:
            app->waiting_response = false;
            strncpy(app->last_error, evt.data.error.code, sizeof(app->last_error) - 1);
            app->last_error[sizeof(app->last_error) - 1] = '\0';
            ui_chat_set_done(app->view_chat);
            {
                char err_msg[48];
                snprintf(err_msg, sizeof(err_msg), "Error: %s", evt.data.error.code);
                ui_chat_set_status(app->view_chat, err_msg);
            }
            FURI_LOG_W(TAG, "Error from ESP32: %s", evt.data.error.code);
            break;

        case AppEventTypeUartStatus:
            ui_chat_set_status(app->view_chat, evt.data.status.text);
            FURI_LOG_I(TAG, "Status: %s", evt.data.status.text);
            break;

        case AppEventTypeUartPong:
            app->uart_connected  = true;
            app->last_pong_tick  = furi_get_tick();
            ui_chat_set_uart_ok(app->view_chat, true);
            break;

        case AppEventTypeKey:
            break;

        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// ViewDispatcher callbacks
// ---------------------------------------------------------------------------

// Called when uart_bridge posts a custom event to wake the dispatcher.
// Signature must be bool(*)(void* context, uint32_t event).
static bool custom_event_cb(void* ctx, uint32_t event) {
    UNUSED(event);
    drain_event_queue((FlipperClawApp*)ctx);
    return true;
}

// 250 ms tick — drives the "thinking..." dot animation in the chat view.
static void tick_event_cb(void* ctx) {
    FlipperClawApp* app = (FlipperClawApp*)ctx;
    // Drain any queued events that arrived between custom events
    drain_event_queue(app);
    // Advance animation counter — ui_chat_tick only triggers a redraw when waiting
    ui_chat_tick(app->view_chat);
}

// ---------------------------------------------------------------------------
// App entry point
// ---------------------------------------------------------------------------

int32_t flipperclaw_app(void* p) {
    UNUSED(p);

    FURI_LOG_I(TAG, "FlipperClaw starting");

    // 1. Allocate app context
    FlipperClawApp* app = malloc(sizeof(FlipperClawApp));
    furi_assert(app);
    memset(app, 0, sizeof(FlipperClawApp));

    app->uart_connected = false;
    app->last_pong_tick = 0;

    // 2. Event queue (capacity 32 AppEvents)
    app->event_queue = furi_message_queue_alloc(32, sizeof(AppEvent));
    furi_assert(app->event_queue);

    // 3. GUI
    app->gui = furi_record_open(RECORD_GUI);
    furi_assert(app->gui);

    // 4. Allocate views
    app->view_chat   = ui_chat_alloc(app);
    app->view_input  = ui_input_alloc(app);
    app->view_status = ui_status_alloc(app);

    furi_assert(app->view_chat);
    furi_assert(app->view_input);
    furi_assert(app->view_status);

    // 5. ViewDispatcher
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, custom_event_cb);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, tick_event_cb, 250);

    view_dispatcher_add_view(app->view_dispatcher, ViewIdChat,   app->view_chat);
    view_dispatcher_add_view(app->view_dispatcher, ViewIdInput,  app->view_input);
    view_dispatcher_add_view(app->view_dispatcher, ViewIdStatus, app->view_status);

    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdChat);

    // 6. UART bridge
    uart_bridge_init(app);

    // 7. Heartbeat PING timer (10 s)
    app->ping_timer = furi_timer_alloc(ping_timer_cb, FuriTimerTypePeriodic, app);
    furi_assert(app->ping_timer);
    furi_timer_start(app->ping_timer, FC_PING_INTERVAL_MS);

    // Show initial status
    ui_chat_set_status(app->view_chat, "Connecting...");

    FURI_LOG_I(TAG, "Entering event loop");

    // 8. Run event loop (blocks here until exit)
    view_dispatcher_run(app->view_dispatcher);

    FURI_LOG_I(TAG, "Event loop exited — tearing down");

    // 9. Teardown
    furi_timer_stop(app->ping_timer);
    furi_timer_free(app->ping_timer);

    uart_bridge_deinit(app);

    view_dispatcher_remove_view(app->view_dispatcher, ViewIdChat);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdInput);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdStatus);
    view_dispatcher_free(app->view_dispatcher);

    view_free(app->view_chat);
    view_free(app->view_input);
    view_free(app->view_status);

    furi_message_queue_free(app->event_queue);
    furi_record_close(RECORD_GUI);

    free(app);

    FURI_LOG_I(TAG, "FlipperClaw exited cleanly");
    return 0;
}
