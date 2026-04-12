#pragma once
#include <furi.h>
#include <furi_hal_uart.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/view.h>
#include <input/input.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file flipperclaw.h
 * @brief Shared types and application state for the FlipperClaw .fap.
 *
 * The app is structured as three views managed by a ViewDispatcher:
 *   ViewIdChat   — scrollable response display (main view)
 *   ViewIdInput  — d-pad character picker for composing prompts
 *   ViewIdStatus — WiFi / memory / error status panel
 */

// ---------------------------------------------------------------------------
// View IDs
// ---------------------------------------------------------------------------

typedef enum {
    ViewIdChat   = 0,
    ViewIdInput  = 1,
    ViewIdStatus = 2,
} ViewId;

// ---------------------------------------------------------------------------
// Event types
// ---------------------------------------------------------------------------

typedef enum {
    AppEventTypeKey,          ///< D-pad / button input event
    AppEventTypeUartChunk,    ///< CHUNK message received — partial response text
    AppEventTypeUartDone,     ///< DONE message received — response complete
    AppEventTypeUartError,    ///< ERROR:<code> received
    AppEventTypeUartStatus,   ///< STATUS:<text> received
    AppEventTypeUartPong,     ///< PONG received — heartbeat reply
} AppEventType;

/** Application event passed through the FuriMessageQueue. */
typedef struct {
    AppEventType type;
    union {
        InputEvent input;                    ///< For AppEventTypeKey
        struct { char* text; } chunk;        ///< Heap-allocated decoded text; caller frees
        struct { char code[16]; } error;     ///< Null-terminated error code string
        struct { char text[64]; } status;    ///< Null-terminated status text
    } data;
} AppEvent;

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------

/** Top-level application context. Allocated on heap in flipperclaw_app(). */
typedef struct {
    // Core Furi handles
    ViewDispatcher*  view_dispatcher;
    FuriMessageQueue* event_queue;      ///< Queue for AppEvent (capacity 32)
    FuriThread*      uart_rx_thread;
    FuriHalUartId    uart_id;
    Gui*             gui;

    // Views (opaque to the app entry point; each ui_*.c manages its own model)
    View* view_chat;
    View* view_input;
    View* view_status;

    // Accumulated response buffer
    char   response_buf[2048];
    size_t response_len;

    // UI state
    bool   waiting_response;    ///< True while ESP32 is streaming
    char   last_error[32];      ///< Last ERROR code received
    bool   uart_connected;      ///< True if PONG received within PONG_TIMEOUT_MS
    uint32_t last_pong_tick;    ///< furi_get_tick() of last PONG

    // Heartbeat timer
    FuriTimer* ping_timer;
} FlipperClawApp;

// ---------------------------------------------------------------------------
// Shared constants (mirror constants.h on ESP32 side)
// ---------------------------------------------------------------------------

#define FC_UART_BAUD           115200U
#define FC_PING_INTERVAL_MS    10000U
#define FC_PONG_TIMEOUT_MS     15000U
#define FC_MAX_RESPONSE_LEN    2048U
#define FC_MAX_INPUT_LEN       256U
#define FC_DISPLAY_LINE_CHARS  21
#define FC_DISPLAY_VISIBLE_LINES 4

#ifdef __cplusplus
}
#endif
