#pragma once
#include "flipperclaw.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file uart_bridge.h
 * @brief Furi HAL UART layer — send/receive, Base64 framing, line parser.
 *
 * Outbound messages are Base64-encoded before sending.
 * Inbound messages are Base64-decoded and dispatched as AppEvents.
 *
 * The IRQ callback copies bytes into a FuriStreamBuffer; the uart_rx_thread
 * drains that buffer and accumulates lines. On '\n' the line is parsed and
 * the appropriate AppEvent is posted to app->event_queue.
 */

/**
 * @brief Initialise UART HAL, stream buffer, and RX thread.
 *
 * Must be called after FuriMessageQueue is created in app context.
 *
 * @param app  Application context.
 */
void uart_bridge_init(FlipperClawApp* app);

/**
 * @brief Tear down UART, stop RX thread, free resources.
 *
 * @param app  Application context.
 */
void uart_bridge_deinit(FlipperClawApp* app);

/**
 * @brief Encode text as Base64 and send "PROMPT:<b64>\n" over UART.
 *
 * @param app   Application context.
 * @param text  Raw UTF-8 prompt text (will be Base64-encoded).
 */
void uart_send_prompt(FlipperClawApp* app, const char* text);

/**
 * @brief Send "PING\n" heartbeat over UART.
 *
 * @param app  Application context.
 */
void uart_send_ping(FlipperClawApp* app);

/**
 * @brief Send "CANCEL\n" over UART to abort in-progress inference.
 *
 * @param app  Application context.
 */
void uart_send_cancel(FlipperClawApp* app);

// ---------------------------------------------------------------------------
// Base64 (RFC 4648) — available to other modules
// ---------------------------------------------------------------------------

/**
 * @brief Base64-encode src into dst.
 *
 * @param src       Input bytes.
 * @param src_len   Number of input bytes.
 * @param dst       Output buffer (must be >= base64_encoded_len(src_len) bytes).
 * @param dst_size  Size of output buffer.
 * @return Number of bytes written (excluding null terminator), or 0 on error.
 */
size_t fc_base64_encode(const uint8_t* src, size_t src_len,
                        char* dst, size_t dst_size);

/**
 * @brief Base64-decode src into dst.
 *
 * @param src       Null-terminated Base64 string.
 * @param src_len   Length of src (excluding null terminator).
 * @param dst       Output buffer.
 * @param dst_size  Size of output buffer.
 * @return Number of decoded bytes written, or 0 on error.
 */
size_t fc_base64_decode(const char* src, size_t src_len,
                        uint8_t* dst, size_t dst_size);

/**
 * @brief Return the buffer size needed to hold a Base64-encoded form of src_len bytes.
 */
static inline size_t base64_encoded_len(size_t src_len) {
    return ((src_len + 2) / 3) * 4 + 1; /* +1 for null terminator */
}

/**
 * @brief Return the maximum number of decoded bytes from base64 of encoded_len chars.
 */
static inline size_t base64_decoded_max(size_t encoded_len) {
    return (encoded_len / 4) * 3 + 3;
}

#ifdef __cplusplus
}
#endif
