#pragma once
#include "esp_err.h"
#include "llm_api.h"
#include "wifi_client.h"
#include "memory_store.h"
#include "uart_proto.h"

/**
 * @file cli.h
 * @brief Interactive serial CLI on UART0 (USB serial) for runtime configuration.
 *
 * Presents a "fc> " prompt. Commands are stored in NVS and take effect
 * immediately. Supports basic command history (up-arrow) and tab completion.
 */
class Cli {
public:
    Cli() = default;

    /**
     * @brief Inject dependencies and start the CLI task.
     *
     * @param llm     LlmApi instance (for set_api_key, set_model, etc.)
     * @param wifi    WifiClient instance (for wifi_set)
     * @param memory  MemoryStore instance (for memory_read/write)
     * @param uart    UartProto instance (for status display)
     * @return ESP_OK on success.
     */
    esp_err_t init(LlmApi* llm, WifiClient* wifi, MemoryStore* memory, UartProto* uart);

private:
    static void cli_task_trampoline(void* arg);
    void cli_task();

    void process_line(const char* line);
    void print_prompt();

    // Command handlers
    void cmd_wifi_set(const char* args);
    void cmd_set_api_key(const char* args);
    void cmd_set_model_provider(const char* args);
    void cmd_set_model(const char* args);
    void cmd_set_tavily_key(const char* args);
    void cmd_set_exa_key(const char* args);
    void cmd_memory_read(const char* args);
    void cmd_memory_write(const char* args);
    void cmd_config_show();
    void cmd_config_reset();
    void cmd_status();
    void cmd_restart();

    // History helpers
    void history_push(const char* line);
    const char* history_get(int offset); // 1 = most recent

    LlmApi*      llm_{nullptr};
    WifiClient*  wifi_{nullptr};
    MemoryStore* memory_{nullptr};
    UartProto*   uart_{nullptr};

    // Command history circular buffer
    char history_buf_[CLI_HISTORY_DEPTH][CLI_LINE_MAX]{};
    int  history_head_{0};
    int  history_count_{0};
    int  history_nav_{0};   // navigation cursor (0 = not navigating)
};
