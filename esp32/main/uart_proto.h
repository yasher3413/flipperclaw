#pragma once
#include <functional>
#include <string>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "constants.h"

/**
 * @file uart_proto.h
 * @brief UART protocol layer for FlipperClaw ESP32 ↔ Flipper Zero communication.
 *
 * Implements the newline-framed, Base64-encoded ASCII wire protocol defined in
 * docs/PROTOCOL.md. All outbound payloads are Base64-encoded before sending.
 * All inbound payloads are Base64-decoded before dispatching to the callback.
 *
 * Thread safety: send() and send_raw() are safe to call from any task. They
 * enqueue messages to a FreeRTOS queue consumed by the TX task. The RX task
 * and its callback are called from the uart_rx_task context only.
 */

/// Parsed message type from Flipper → ESP32.
enum class MsgType : uint8_t {
    PROMPT,        ///< User prompt text (payload: Base64-decoded string)
    CANCEL,        ///< Abort current inference
    PING,          ///< Heartbeat from Flipper
    HW_NFC_DATA,   ///< NFC tag bytes (payload: Base64-decoded binary)
    HW_SUBGHZ_DATA,///< Sub-GHz capture bytes (payload: Base64-decoded binary)
    UNKNOWN,       ///< Unrecognised message type
};

/**
 * @brief UART protocol driver.
 *
 * Manages the UART peripheral, a TX queue, and an RX parsing task. Callbacks
 * are dispatched synchronously on the RX task stack — keep them short or post
 * to another queue.
 */
class UartProto {
public:
    UartProto() = default;
    ~UartProto();

    /**
     * @brief Initialise UART peripheral and start RX task.
     * @param port   UART port number (e.g. UART_NUM_1).
     * @param tx_pin GPIO number for TX.
     * @param rx_pin GPIO number for RX.
     * @param baud   Baud rate (default 115200).
     * @return ESP_OK on success.
     */
    esp_err_t init(uart_port_t port, int tx_pin, int rx_pin, int baud = 115200);

    /**
     * @brief Register the callback invoked for every complete received message.
     *
     * The callback receives the message type and the decoded payload string.
     * For CANCEL and PING the payload string is empty.
     * Called from the RX task context.
     *
     * @param cb  Callback function: (MsgType, decoded_payload) → void.
     */
    void set_callback(std::function<void(MsgType, const std::string&)> cb);

    /**
     * @brief Encode payload as Base64 and send "<type>:<b64>\n" over UART.
     * @param msg_type  ASCII type prefix (e.g. "CHUNK", "STATUS").
     * @param payload   Raw payload string — will be Base64-encoded.
     * @return ESP_OK on success, ESP_ERR_NO_MEM if TX queue full.
     */
    esp_err_t send(const std::string& msg_type, const std::string& payload);

    /**
     * @brief Send a pre-formatted message verbatim (no Base64 encoding).
     *
     * Use for type-only messages: "DONE\n", "PONG\n", "HW:NFC:READ\n".
     *
     * @param msg  Complete message string including trailing '\n'.
     * @return ESP_OK on success.
     */
    esp_err_t send_raw(const std::string& msg);

    // ------------------------------------------------------------------
    // Base64 utilities (public for unit testing)
    // ------------------------------------------------------------------

    /**
     * @brief RFC 4648 Base64 encode.
     * @param input  Raw bytes.
     * @return Base64-encoded string (no newlines, padded with '=').
     */
    static std::string base64_encode(const std::string& input);

    /**
     * @brief RFC 4648 Base64 decode.
     * @param input  Base64-encoded string.
     * @param output Decoded bytes written here.
     * @return true on success, false if input is malformed.
     */
    static bool base64_decode(const std::string& input, std::string& output);

private:
    // FreeRTOS task entry trampolines
    static void rx_task_trampoline(void* arg);
    static void tx_task_trampoline(void* arg);

    void rx_task();
    void tx_task();

    /// Parse a complete '\n'-terminated line and dispatch via callback.
    void parse_line(const char* buf, size_t len);

    uart_port_t                                     port_{UART_NUM_1};
    QueueHandle_t                                   uart_event_queue_{nullptr};
    QueueHandle_t                                   tx_queue_{nullptr};
    TaskHandle_t                                    rx_task_handle_{nullptr};
    TaskHandle_t                                    tx_task_handle_{nullptr};
    std::function<void(MsgType, const std::string&)> callback_;

    /// Line accumulation buffer for RX parser.
    char     rx_line_buf_[MAX_MSG_LINE_LEN]{};
    size_t   rx_line_len_{0};

    bool     running_{false};
};
