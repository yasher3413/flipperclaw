/**
 * @file cli.cpp
 * @brief Interactive serial CLI on UART0 — configure WiFi, API keys, model, memory.
 *
 * Supports:
 *   - Tab completion on command names
 *   - Up/down arrow history (circular buffer, CLI_HISTORY_DEPTH entries)
 *   - All settings stored in NVS; fc_secrets.h values are compile-time defaults
 */

#include "cli.h"
#include "constants.h"
#include "fc_secrets.h"
#include <cstring>
#include <cstdio>
#include "esp_log.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "cli";

// All known command names (for tab completion)
static const char* const COMMANDS[] = {
    "wifi_set", "set_api_key", "set_model_provider", "set_model",
    "set_tavily_key", "set_brave_key", "memory_read", "memory_write",
    "config_show", "config_reset", "status", "restart", nullptr
};

// ANSI escape helpers
#define ANSI_UP    "\x1b[A"
#define ANSI_DOWN  "\x1b[B"
#define ANSI_CLEAR_LINE "\r\x1b[K"

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

esp_err_t Cli::init(LlmApi* llm, WifiClient* wifi, MemoryStore* memory, UartProto* uart) {
    llm_    = llm;
    wifi_   = wifi;
    memory_ = memory;
    uart_   = uart;

    xTaskCreatePinnedToCore(cli_task_trampoline, "cli", 4096, this, 3, nullptr, 0);
    ESP_LOGI(TAG, "CLI started on UART0");
    return ESP_OK;
}

void Cli::cli_task_trampoline(void* arg) {
    static_cast<Cli*>(arg)->cli_task();
}

// ---------------------------------------------------------------------------
// CLI task — reads UART0 byte-by-byte, handles editing and history
// ---------------------------------------------------------------------------

void Cli::cli_task() {
    // UART0 is already installed by IDF for logging; we just read from it
    char line_buf[CLI_LINE_MAX];
    size_t line_pos = 0;
    bool   escape   = false;
    bool   bracket  = false;

    printf("\r\nFlipperClaw CLI ready. Type 'config_show' to verify settings.\r\n");
    print_prompt();

    uint8_t ch;
    while (true) {
        int n = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(20));
        if (n <= 0) continue;

        // Escape sequence state machine (arrow keys)
        if (escape) {
            if (bracket) {
                bracket = false;
                escape  = false;
                if (ch == 'A') { // UP arrow
                    ++history_nav_;
                    if (history_nav_ > history_count_) history_nav_ = history_count_;
                    const char* h = history_get(history_nav_);
                    if (h) {
                        printf(ANSI_CLEAR_LINE "fc> %s", h);
                        fflush(stdout);
                        strncpy(line_buf, h, CLI_LINE_MAX - 1);
                        line_pos = strlen(line_buf);
                    }
                } else if (ch == 'B') { // DOWN arrow
                    --history_nav_;
                    if (history_nav_ < 0) history_nav_ = 0;
                    if (history_nav_ == 0) {
                        printf(ANSI_CLEAR_LINE "fc> ");
                        fflush(stdout);
                        line_buf[0] = '\0';
                        line_pos = 0;
                    } else {
                        const char* h = history_get(history_nav_);
                        if (h) {
                            printf(ANSI_CLEAR_LINE "fc> %s", h);
                            fflush(stdout);
                            strncpy(line_buf, h, CLI_LINE_MAX - 1);
                            line_pos = strlen(line_buf);
                        }
                    }
                }
                continue;
            }
            if (ch == '[') { bracket = true; continue; }
            escape = false;
            continue;
        }
        if (ch == 0x1b) { escape = true; continue; }

        // Enter
        if (ch == '\r' || ch == '\n') {
            printf("\r\n");
            line_buf[line_pos] = '\0';
            if (line_pos > 0) {
                history_push(line_buf);
                history_nav_ = 0;
                process_line(line_buf);
            }
            line_pos = 0;
            line_buf[0] = '\0';
            print_prompt();
            continue;
        }

        // Backspace
        if (ch == 0x7f || ch == '\b') {
            if (line_pos > 0) {
                --line_pos;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        // Tab completion
        if (ch == '\t') {
            line_buf[line_pos] = '\0';
            // Find first match
            const char* match = nullptr;
            int match_count = 0;
            for (int i = 0; COMMANDS[i]; ++i) {
                if (strncmp(COMMANDS[i], line_buf, line_pos) == 0) {
                    match = COMMANDS[i];
                    ++match_count;
                }
            }
            if (match_count == 1 && match) {
                printf(ANSI_CLEAR_LINE "fc> %s ", match);
                fflush(stdout);
                strncpy(line_buf, match, CLI_LINE_MAX - 2);
                line_pos = strlen(line_buf);
                line_buf[line_pos++] = ' ';
                line_buf[line_pos]   = '\0';
            } else if (match_count > 1) {
                printf("\r\n");
                for (int i = 0; COMMANDS[i]; ++i) {
                    if (strncmp(COMMANDS[i], line_buf, line_pos) == 0) {
                        printf("  %s\r\n", COMMANDS[i]);
                    }
                }
                print_prompt();
                printf("%s", line_buf);
                fflush(stdout);
            }
            continue;
        }

        // Printable character
        if (ch >= 0x20 && ch < 0x7f && line_pos < CLI_LINE_MAX - 1) {
            line_buf[line_pos++] = static_cast<char>(ch);
            printf("%c", ch);
            fflush(stdout);
        }
    }
}

void Cli::print_prompt() {
    printf("fc> ");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// History helpers
// ---------------------------------------------------------------------------

void Cli::history_push(const char* line) {
    strncpy(history_buf_[history_head_], line, CLI_LINE_MAX - 1);
    history_head_ = (history_head_ + 1) % CLI_HISTORY_DEPTH;
    if (history_count_ < (int)CLI_HISTORY_DEPTH) ++history_count_;
}

const char* Cli::history_get(int offset) {
    if (offset <= 0 || offset > history_count_) return nullptr;
    int idx = (history_head_ - offset + CLI_HISTORY_DEPTH) % CLI_HISTORY_DEPTH;
    return history_buf_[idx];
}

// ---------------------------------------------------------------------------
// Command dispatcher
// ---------------------------------------------------------------------------

void Cli::process_line(const char* line) {
    // Skip leading whitespace
    while (*line == ' ') ++line;

    // Find command word end
    const char* space = strchr(line, ' ');
    size_t cmd_len = space ? (size_t)(space - line) : strlen(line);
    const char* args = space ? space + 1 : "";

    if      (strncmp(line, "wifi_set",           cmd_len) == 0) cmd_wifi_set(args);
    else if (strncmp(line, "set_api_key",        cmd_len) == 0) cmd_set_api_key(args);
    else if (strncmp(line, "set_model_provider", cmd_len) == 0) cmd_set_model_provider(args);
    else if (strncmp(line, "set_model",          cmd_len) == 0) cmd_set_model(args);
    else if (strncmp(line, "set_tavily_key",     cmd_len) == 0) cmd_set_tavily_key(args);
    else if (strncmp(line, "set_brave_key",      cmd_len) == 0) cmd_set_brave_key(args);
    else if (strncmp(line, "memory_read",        cmd_len) == 0) cmd_memory_read(args);
    else if (strncmp(line, "memory_write",       cmd_len) == 0) cmd_memory_write(args);
    else if (strncmp(line, "config_show",        cmd_len) == 0) cmd_config_show();
    else if (strncmp(line, "config_reset",       cmd_len) == 0) cmd_config_reset();
    else if (strncmp(line, "status",             cmd_len) == 0) cmd_status();
    else if (strncmp(line, "restart",            cmd_len) == 0) cmd_restart();
    else if (strncmp(line, "help",               cmd_len) == 0) {
        printf("Commands: wifi_set set_api_key set_model_provider set_model\r\n"
               "  set_tavily_key set_brave_key memory_read memory_write\r\n"
               "  config_show config_reset status restart\r\n");
    } else {
        printf("Unknown command. Type 'help' for command list.\r\n");
    }
}

// ---------------------------------------------------------------------------
// NVS helper
// ---------------------------------------------------------------------------

static esp_err_t nvs_set(const char* key, const char* value) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    nvs_set_str(nvs, key, value);
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static std::string nvs_get(const char* key, const char* default_val = "") {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return default_val;
    char buf[128] = {};
    size_t len = sizeof(buf);
    if (nvs_get_str(nvs, key, buf, &len) != ESP_OK) {
        nvs_close(nvs);
        return default_val;
    }
    nvs_close(nvs);
    return buf;
}

// ---------------------------------------------------------------------------
// Command implementations
// ---------------------------------------------------------------------------

void Cli::cmd_wifi_set(const char* args) {
    // Parse: <ssid> <password>
    char ssid[64] = {}, pass[64] = {};
    if (sscanf(args, "%63s %63s", ssid, pass) < 2) {
        printf("Usage: wifi_set <ssid> <password>\r\n");
        return;
    }
    if (wifi_) wifi_->set_credentials(ssid, pass);
    else {
        nvs_set("wifi_ssid", ssid);
        nvs_set("wifi_pass", pass);
    }
    printf("WiFi credentials saved. Restart to reconnect.\r\n");
}

void Cli::cmd_set_api_key(const char* args) {
    if (!args || !args[0]) { printf("Usage: set_api_key <key>\r\n"); return; }
    if (llm_) llm_->set_api_key(args);
    else nvs_set("api_key", args);
    printf("API key saved.\r\n");
}

void Cli::cmd_set_model_provider(const char* args) {
    if (strcmp(args, "anthropic") != 0 && strcmp(args, "openai") != 0) {
        printf("Usage: set_model_provider <anthropic|openai>\r\n");
        return;
    }
    if (llm_) llm_->set_provider(args);
    else nvs_set("provider", args);
    printf("Provider set to: %s\r\n", args);
}

void Cli::cmd_set_model(const char* args) {
    if (!args || !args[0]) { printf("Usage: set_model <model_name>\r\n"); return; }
    if (llm_) llm_->set_model(args);
    else nvs_set("model", args);
    printf("Model set to: %s\r\n", args);
}

void Cli::cmd_set_tavily_key(const char* args) {
    if (!args || !args[0]) { printf("Usage: set_tavily_key <key>\r\n"); return; }
    nvs_set("tavily_key", args);
    printf("Tavily key saved.\r\n");
}

void Cli::cmd_set_brave_key(const char* args) {
    if (!args || !args[0]) { printf("Usage: set_brave_key <key>\r\n"); return; }
    nvs_set("brave_key", args);
    printf("Brave Search key saved.\r\n");
}

void Cli::cmd_memory_read(const char* args) {
    if (!memory_) { printf("Memory store not available.\r\n"); return; }
    const char* fname = (args && args[0]) ? args : "MEMORY.md";
    std::string content;
    if (memory_->read(fname, content) != ESP_OK) {
        printf("File not found: %s\r\n", fname);
        return;
    }
    printf("--- %s ---\r\n%s\r\n--- end ---\r\n", fname, content.c_str());
}

void Cli::cmd_memory_write(const char* args) {
    if (!memory_) { printf("Memory store not available.\r\n"); return; }
    if (!args || !args[0]) { printf("Usage: memory_write \"<content>\"\r\n"); return; }
    // Strip optional surrounding quotes
    std::string content = args;
    if (content.front() == '"') content = content.substr(1);
    if (!content.empty() && content.back() == '"') content.pop_back();
    memory_->append("MEMORY.md", content);
    printf("Written to MEMORY.md.\r\n");
}

void Cli::cmd_config_show() {
    printf("=== FlipperClaw Configuration ===\r\n");
    printf("  WiFi SSID:    %s\r\n", nvs_get("wifi_ssid", FC_SECRET_WIFI_SSID).c_str());
    printf("  WiFi Pass:    %s\r\n", nvs_get("wifi_pass", "***").empty() ? "(not set)" : "***");
    std::string key = nvs_get("api_key", FC_SECRET_API_KEY);
    printf("  API Key:      %s\r\n", key.empty() ? "(not set)" : "***");
    printf("  Provider:     %s\r\n", nvs_get("provider", FC_SECRET_MODEL_PROVIDER).c_str());
    printf("  Model:        %s\r\n", llm_ ? llm_->model().c_str() : nvs_get("model", "default").c_str());
    printf("  Tavily key:   %s\r\n", nvs_get("tavily_key", "").empty() ? "(not set)" : "***");
    printf("  Brave key:    %s\r\n", nvs_get("brave_key", "").empty() ? "(not set)" : "***");
    if (memory_) {
        size_t total = 0, used = 0;
        memory_->partition_info(total, used);
        printf("  SPIFFS:       %zu KB used / %zu KB total\r\n", used/1024, total/1024);
    }
    printf("=================================\r\n");
}

void Cli::cmd_config_reset() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    printf("Configuration reset to compile-time defaults. Restart to apply.\r\n");
}

void Cli::cmd_status() {
    printf("=== Status ===\r\n");
    printf("  WiFi:     %s\r\n", wifi_ && wifi_->is_connected() ? "connected" : "disconnected");
    printf("  Free heap: %lu bytes\r\n", (unsigned long)esp_get_free_heap_size());
    printf("  Min heap:  %lu bytes\r\n", (unsigned long)esp_get_minimum_free_heap_size());
    printf("==============\r\n");
}

void Cli::cmd_restart() {
    printf("Restarting...\r\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}
