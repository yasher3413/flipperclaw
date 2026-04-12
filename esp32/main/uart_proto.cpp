/**
 * @file uart_proto.cpp
 * @brief UART protocol driver — Base64 framing, TX queue, RX line parser.
 *
 * Wire format (from docs/PROTOCOL.md):
 *   Outbound:  <TYPE>:<base64_payload>\n   or   <TYPE>\n  (no-payload msgs)
 *   Inbound:   same format, parsed and dispatched via callback.
 *
 * Base64 is RFC 4648 standard with '=' padding. Implemented from scratch —
 * no external dependency.
 */

#include "uart_proto.h"
#include <cstring>
#include <algorithm>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "uart_proto";

// ---------------------------------------------------------------------------
// Base64 tables
// ---------------------------------------------------------------------------

static const char B64_ENC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const int8_t B64_DEC[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 0-15
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 16-31
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63, // 32-47  (+, /)
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1, // 48-63  (0-9, =)
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, // 64-79  (A-O)
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1, // 80-95  (P-Z)
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, // 96-111 (a-o)
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1, // 112-127 (p-z)
    // 128-255: all -1
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

// ---------------------------------------------------------------------------
// Base64 encode (RFC 4648)
// ---------------------------------------------------------------------------

std::string UartProto::base64_encode(const std::string& input) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(input.data());
    size_t len = input.size();
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) b |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) b |= static_cast<uint32_t>(data[i + 2]);

        out += B64_ENC[(b >> 18) & 0x3F];
        out += B64_ENC[(b >> 12) & 0x3F];
        out += (i + 1 < len) ? B64_ENC[(b >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? B64_ENC[(b)      & 0x3F] : '=';
    }
    return out;
}

// ---------------------------------------------------------------------------
// Base64 decode (RFC 4648)
// ---------------------------------------------------------------------------

bool UartProto::base64_decode(const std::string& input, std::string& output) {
    output.clear();
    const size_t len = input.size();
    if (len % 4 != 0) {
        ESP_LOGW(TAG, "base64_decode: length %zu not a multiple of 4", len);
        return false;
    }
    output.reserve((len / 4) * 3);

    for (size_t i = 0; i < len; i += 4) {
        int8_t a = B64_DEC[static_cast<uint8_t>(input[i])];
        int8_t b = B64_DEC[static_cast<uint8_t>(input[i + 1])];
        int8_t c = B64_DEC[static_cast<uint8_t>(input[i + 2])];
        int8_t d = B64_DEC[static_cast<uint8_t>(input[i + 3])];

        if (a < 0 || b < 0) {
            ESP_LOGW(TAG, "base64_decode: invalid char at pos %zu", i);
            return false;
        }

        uint32_t triple = (static_cast<uint32_t>(a) << 18) |
                          (static_cast<uint32_t>(b) << 12) |
                          (static_cast<uint32_t>(c < 0 ? 0 : c) << 6) |
                          (static_cast<uint32_t>(d < 0 ? 0 : d));

        output += static_cast<char>((triple >> 16) & 0xFF);
        if (input[i + 2] != '=') output += static_cast<char>((triple >> 8) & 0xFF);
        if (input[i + 3] != '=') output += static_cast<char>(triple & 0xFF);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Init / teardown
// ---------------------------------------------------------------------------

esp_err_t UartProto::init(uart_port_t port, int tx_pin, int rx_pin, int baud) {
    port_ = port;

    uart_config_t cfg = {};
    cfg.baud_rate  = baud;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_RETURN_ON_ERROR(uart_param_config(port_, &cfg), TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(
        uart_set_pin(port_, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
        TAG, "uart_set_pin failed"
    );
    ESP_RETURN_ON_ERROR(
        uart_driver_install(port_, UART_RX_BUF_LEN, UART_TX_BUF_LEN, 20,
                            &uart_event_queue_, 0),
        TAG, "uart_driver_install failed"
    );

    // TX message queue: up to 32 pre-formatted strings
    tx_queue_ = xQueueCreate(32, sizeof(std::string*));
    if (!tx_queue_) {
        ESP_LOGE(TAG, "Failed to create TX queue");
        return ESP_ERR_NO_MEM;
    }

    running_ = true;

    xTaskCreatePinnedToCore(rx_task_trampoline, "uart_rx", 4096, this, 5,
                            &rx_task_handle_, 0);
    xTaskCreatePinnedToCore(tx_task_trampoline, "uart_tx", 3072, this, 5,
                            &tx_task_handle_, 0);

    ESP_LOGI(TAG, "Initialised UART%d tx=%d rx=%d baud=%d", (int)port_, tx_pin, rx_pin, baud);
    return ESP_OK;
}

UartProto::~UartProto() {
    running_ = false;
    if (rx_task_handle_) vTaskDelete(rx_task_handle_);
    if (tx_task_handle_) vTaskDelete(tx_task_handle_);
    if (tx_queue_) vQueueDelete(tx_queue_);
    uart_driver_delete(port_);
}

void UartProto::set_callback(std::function<void(MsgType, const std::string&)> cb) {
    callback_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Send helpers
// ---------------------------------------------------------------------------

esp_err_t UartProto::send(const std::string& msg_type, const std::string& payload) {
    std::string encoded = base64_encode(payload);
    std::string* msg = new std::string(msg_type + ":" + encoded + "\n");
    if (xQueueSend(tx_queue_, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "TX queue full, dropping %s message", msg_type.c_str());
        delete msg;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t UartProto::send_raw(const std::string& msg) {
    std::string* m = new std::string(msg);
    if (xQueueSend(tx_queue_, &m, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "TX queue full, dropping raw message");
        delete m;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// TX task — dequeues and writes to UART
// ---------------------------------------------------------------------------

void UartProto::tx_task_trampoline(void* arg) {
    static_cast<UartProto*>(arg)->tx_task();
}

void UartProto::tx_task() {
    std::string* msg = nullptr;
    while (running_) {
        if (xQueueReceive(tx_queue_, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            uart_write_bytes(port_, msg->c_str(), msg->size());
            delete msg;
            msg = nullptr;
        }
    }
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// RX task — reads UART events, accumulates lines, calls parse_line()
// ---------------------------------------------------------------------------

void UartProto::rx_task_trampoline(void* arg) {
    static_cast<UartProto*>(arg)->rx_task();
}

void UartProto::rx_task() {
    uart_event_t event;
    uint8_t byte_buf[128];

    while (running_) {
        if (xQueueReceive(uart_event_queue_, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        switch (event.type) {
        case UART_DATA: {
            size_t remaining = event.size;
            while (remaining > 0) {
                int to_read = std::min(remaining, sizeof(byte_buf));
                int n = uart_read_bytes(port_, byte_buf, to_read, pdMS_TO_TICKS(10));
                if (n <= 0) break;
                remaining -= n;

                for (int i = 0; i < n; ++i) {
                    char c = static_cast<char>(byte_buf[i]);
                    if (c == '\n') {
                        if (rx_line_len_ > 0) {
                            parse_line(rx_line_buf_, rx_line_len_);
                            rx_line_len_ = 0;
                        }
                    } else {
                        if (rx_line_len_ < MAX_MSG_LINE_LEN - 1) {
                            rx_line_buf_[rx_line_len_++] = c;
                        } else {
                            ESP_LOGW(TAG, "RX line too long, discarding");
                            rx_line_len_ = 0;
                        }
                    }
                }
            }
            break;
        }
        case UART_FIFO_OVF:
            ESP_LOGW(TAG, "UART FIFO overflow — flushing");
            uart_flush_input(port_);
            xQueueReset(uart_event_queue_);
            rx_line_len_ = 0;
            break;
        case UART_BUFFER_FULL:
            ESP_LOGW(TAG, "UART ring buffer full — flushing");
            uart_flush_input(port_);
            xQueueReset(uart_event_queue_);
            rx_line_len_ = 0;
            break;
        default:
            break;
        }
    }
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Line parser — maps wire format to MsgType + decoded payload
// ---------------------------------------------------------------------------

void UartProto::parse_line(const char* buf, size_t len) {
    if (len == 0 || !callback_) return;

    // Null-terminate for string ops
    // buf is in rx_line_buf_ which has space for MAX_MSG_LINE_LEN bytes
    const_cast<char*>(buf)[len] = '\0';

    // Strip trailing \r if present
    if (len > 0 && buf[len - 1] == '\r') {
        const_cast<char*>(buf)[--len] = '\0';
    }

    ESP_LOGD(TAG, "RX line: %.*s", (int)std::min(len, (size_t)80), buf);

    // No-payload messages
    if (len == 4 && strncmp(buf, "PING", 4) == 0) {
        callback_(MsgType::PING, "");
        return;
    }
    if (len == 6 && strncmp(buf, "CANCEL", 6) == 0) {
        callback_(MsgType::CANCEL, "");
        return;
    }

    // Find first ':'
    const char* colon = static_cast<const char*>(memchr(buf, ':', len));
    if (!colon) {
        ESP_LOGW(TAG, "RX: no colon in message, ignoring");
        callback_(MsgType::UNKNOWN, "");
        return;
    }

    size_t type_len = colon - buf;
    const char* payload_b64 = colon + 1;
    size_t payload_b64_len = len - type_len - 1;

    std::string decoded;
    if (payload_b64_len > 0) {
        if (!base64_decode(std::string(payload_b64, payload_b64_len), decoded)) {
            ESP_LOGW(TAG, "RX: base64 decode failed");
            callback_(MsgType::UNKNOWN, "");
            return;
        }
    }

    // Match type prefix
    if (type_len == 6 && strncmp(buf, "PROMPT", 6) == 0) {
        callback_(MsgType::PROMPT, decoded);
    } else if (type_len == 11 && strncmp(buf, "HW:NFC:DATA", 11) == 0) {
        callback_(MsgType::HW_NFC_DATA, decoded);
    } else if (type_len == 14 && strncmp(buf, "HW:SUBGHZ:DATA", 14) == 0) {
        callback_(MsgType::HW_SUBGHZ_DATA, decoded);
    } else {
        ESP_LOGW(TAG, "RX: unknown message type '%.*s'", (int)type_len, buf);
        callback_(MsgType::UNKNOWN, decoded);
    }
}
