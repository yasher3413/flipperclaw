#pragma once
#include <cstddef>
#include <cstdint>

/**
 * @file constants.h
 * @brief Compile-time constants shared across all FlipperClaw ESP32 modules.
 *
 * All buffer sizes, timeouts, and limits are defined here. Never use magic
 * numbers in source files — reference these constants instead so that the
 * memory budget can be audited in one place.
 */

// ---------------------------------------------------------------------------
// Buffer sizes
// ---------------------------------------------------------------------------

/// Maximum length of a single UART protocol message (including header + '\n').
constexpr size_t MAX_MSG_LINE_LEN = 4096;

/// Maximum length of the accumulated LLM response (shared with Flipper buffer).
constexpr size_t MAX_RESPONSE_LEN = 2048;

/// Maximum length of a user input prompt.
constexpr size_t MAX_INPUT_LEN = 256;

/// Maximum conversation history turns (user + assistant pairs each count as 1).
constexpr size_t MAX_HISTORY_TURNS = 50;

/// UART RX ring buffer size (bytes). Must be >= MAX_MSG_LINE_LEN.
constexpr size_t UART_RX_BUF_LEN = 8192;

/// UART TX ring buffer size (bytes).
constexpr size_t UART_TX_BUF_LEN = 4096;

/// Size of the CLI command history circular buffer (number of entries).
constexpr size_t CLI_HISTORY_DEPTH = 10;

/// Maximum length of a single CLI command line.
constexpr size_t CLI_LINE_MAX = 256;

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

/// UART baud rate for Flipper ↔ ESP32 communication.
constexpr uint32_t UART_BAUD = 115200;

/// Interval at which the Flipper sends PING (milliseconds).
constexpr uint32_t PING_INTERVAL_MS = 10000;

/// Time after which missing PONG is treated as disconnect (milliseconds).
constexpr uint32_t PONG_TIMEOUT_MS = 15000;

/// HTTP connect timeout for LLM API calls (milliseconds).
constexpr int HTTP_CONNECT_TIMEOUT_MS = 10000;

/// HTTP network (read) timeout for LLM API calls (milliseconds).
constexpr int HTTP_NETWORK_TIMEOUT_MS = 30000;

/// WiFi reconnect base backoff (milliseconds). Doubles on each retry, cap 30s.
constexpr uint32_t WIFI_BACKOFF_BASE_MS = 1000;

/// WiFi reconnect maximum backoff (milliseconds).
constexpr uint32_t WIFI_BACKOFF_MAX_MS = 30000;

/// Timeout for waiting on NFC data from Flipper (milliseconds).
constexpr uint32_t NFC_WAIT_TIMEOUT_MS = 30000;

/// Interval at which the heartbeat task checks HEARTBEAT.md (milliseconds).
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 1800000; // 30 minutes

// ---------------------------------------------------------------------------
// Agent limits
// ---------------------------------------------------------------------------

/// Maximum tool-call iterations the agent will execute per user prompt.
constexpr int MAX_TOOL_LOOPS = 5;

// ---------------------------------------------------------------------------
// Display geometry (Flipper 128×64 monochrome OLED)
// ---------------------------------------------------------------------------

/// Maximum characters per display line in the response view.
constexpr int DISPLAY_LINE_CHARS = 21;

/// Number of response lines visible at once (without scrolling).
constexpr int DISPLAY_VISIBLE_LINES = 4;

// ---------------------------------------------------------------------------
// UART hardware (ESP32-S3 defaults, overridable via sdkconfig)
// ---------------------------------------------------------------------------

/// ESP32-S3 UART port used for Flipper communication.
constexpr int FC_UART_PORT = 1;  // UART1

/// Default TX GPIO for Flipper communication.
constexpr int FC_UART_TX_PIN = 17;

/// Default RX GPIO for Flipper communication.
constexpr int FC_UART_RX_PIN = 18;

// ---------------------------------------------------------------------------
// SPIFFS
// ---------------------------------------------------------------------------

/// SPIFFS mount point.
constexpr const char* SPIFFS_BASE_PATH = "/spiffs";

/// SPIFFS partition label (must match partitions.csv).
constexpr const char* SPIFFS_PARTITION_LABEL = "spiffs";

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------

/// NVS namespace for FlipperClaw configuration.
constexpr const char* NVS_NAMESPACE = "flipperclaw";
