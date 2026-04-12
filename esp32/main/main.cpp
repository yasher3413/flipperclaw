/**
 * @file main.cpp
 * @brief FlipperClaw ESP32-S3 entry point — initialise hardware and start tasks.
 *
 * Boot sequence:
 *   1. NVS flash init
 *   2. SPIFFS / MemoryStore init
 *   3. WiFi init + connect
 *   4. UART protocol init (UART1 ↔ Flipper)
 *   5. LLM API init (load keys from NVS)
 *   6. Tools init (inject UART bridge for hardware tools)
 *   7. Agent init
 *   8. CLI init (UART0 / USB serial)
 *   9. Heartbeat timer start
 *
 * FreeRTOS tasks:
 *   Core 0: uart_rx, wifi_reconnect, heartbeat, cli
 *   Core 1: agent (started on-demand when PROMPT arrives)
 */

#include <cstdio>
#include <string>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_timer.h"

#include "constants.h"
#include "uart_proto.h"
#include "wifi_client.h"
#include "memory_store.h"
#include "llm_api.h"
#include "tools.h"
#include "cron.h"
#include "agent.h"
#include "cli.h"

static const char* TAG = "main";

// ---------------------------------------------------------------------------
// Global singletons — created once in app_main, injected everywhere else
// ---------------------------------------------------------------------------

static UartProto      g_uart;
static WifiClient     g_wifi;
static MemoryStore    g_memory;
static LlmApi         g_llm;
static Tools          g_tools;
static CronScheduler  g_cron;
static Agent          g_agent;
static Cli            g_cli;

// ---------------------------------------------------------------------------
// Heartbeat timer — sends PING reply (we actually send PONG from RX handler,
// but we send STATUS updates here to indicate the ESP32 is alive)
// ---------------------------------------------------------------------------

static void heartbeat_timer_cb(void* /*arg*/) {
    // Only emit status if WiFi is connected; otherwise the Flipper knows via
    // absence of PONG (we rely on the PING/PONG exchange, not proactive STATUS)
    static uint32_t tick = 0;
    ++tick;
    if (tick % 6 == 0) { // every ~60s
        if (g_wifi.is_connected()) {
            g_uart.send("STATUS", "Online");
        } else {
            g_uart.send("STATUS", "No WiFi");
        }
    }
}

// ---------------------------------------------------------------------------
// Heartbeat task — checks HEARTBEAT.md every 30 min, injects prompt if tasks
// ---------------------------------------------------------------------------

static bool heartbeat_has_pending(const std::string& content) {
    size_t pos = 0;
    while (pos < content.size()) {
        size_t end = content.find('\n', pos);
        if (end == std::string::npos) end = content.size();
        std::string line = content.substr(pos, end - pos);
        pos = end + 1;
        // Skip blank lines
        if (line.empty() || line.find_first_not_of(" \t\r") == std::string::npos) continue;
        // Skip headers
        if (line[0] == '#') continue;
        // Skip HTML comments
        if (line.find("<!--") != std::string::npos || line.find("-->") != std::string::npos) continue;
        // Skip completed items
        if (line.find("- [x]") != std::string::npos || line.find("- [X]") != std::string::npos) continue;
        return true; // uncompleted item found
    }
    return false;
}

static void cron_task(void* /*arg*/) {
    // Poll every 10 s — fine-grained enough for any job >= 60 s interval
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (!g_wifi.is_connected() || g_agent.is_running()) continue;

        auto fired = g_cron.poll();
        for (const auto& job : fired) {
            ESP_LOGI(TAG, "Cron firing: %s", job.id.c_str());
            g_uart.send("STATUS", std::string("Cron: ") + job.id);
            xSemaphoreTake(g_prompt_mutex, portMAX_DELAY);
            g_pending_prompt = job.message;
            xSemaphoreGive(g_prompt_mutex);
            xSemaphoreGive(g_agent_sem);
            // Wait for agent to finish before firing the next job
            while (g_agent.is_running()) vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

static void heartbeat_md_task(void* /*arg*/) {
    // Stagger first check by the full interval so boot isn't noisy
    vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));

    while (true) {
        if (g_wifi.is_connected() && !g_agent.is_running()) {
            std::string content;
            if (g_memory.read("HEARTBEAT.md", content) == ESP_OK &&
                heartbeat_has_pending(content)) {
                ESP_LOGI(TAG, "Heartbeat: pending tasks found, injecting prompt");
                g_uart.send("STATUS", "Heartbeat: acting on tasks...");
                xSemaphoreTake(g_prompt_mutex, portMAX_DELAY);
                g_pending_prompt =
                    "Check HEARTBEAT.md for any tasks that are not marked complete "
                    "(- [x]). Act on each pending item, then mark it done by "
                    "rewriting the line as '- [x] <task>'.";
                xSemaphoreGive(g_prompt_mutex);
                xSemaphoreGive(g_agent_sem);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));
    }
}

// ---------------------------------------------------------------------------
// Agent task — runs on core 1, waits for prompts via semaphore
// ---------------------------------------------------------------------------

static SemaphoreHandle_t g_agent_sem = nullptr;
static std::string       g_pending_prompt;
static SemaphoreHandle_t g_prompt_mutex = nullptr;

static void agent_task(void* /*arg*/) {
    while (true) {
        if (xSemaphoreTake(g_agent_sem, portMAX_DELAY) == pdTRUE) {
            std::string prompt;
            xSemaphoreTake(g_prompt_mutex, portMAX_DELAY);
            prompt = g_pending_prompt;
            xSemaphoreGive(g_prompt_mutex);

            if (!prompt.empty()) {
                if (!g_wifi.is_connected()) {
                    g_uart.send_raw("ERROR:NO_WIFI\n");
                } else {
                    g_agent.run(prompt);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// UART message callback — dispatched from uart_rx task (core 0)
// ---------------------------------------------------------------------------

static void on_uart_message(MsgType type, const std::string& payload) {
    switch (type) {
    case MsgType::PING:
        g_uart.send_raw("PONG\n");
        break;

    case MsgType::PROMPT:
        if (g_agent.is_running()) {
            // Already running — queue would overwrite; tell Flipper we're busy
            g_uart.send("STATUS", "Busy — please wait...");
            return;
        }
        xSemaphoreTake(g_prompt_mutex, portMAX_DELAY);
        g_pending_prompt = payload;
        xSemaphoreGive(g_prompt_mutex);
        xSemaphoreGive(g_agent_sem);
        break;

    case MsgType::CANCEL:
        g_agent.cancel();
        break;

    case MsgType::HW_NFC_DATA:
        // Forwarded to the hardware tool waiting on this event.
        // The tools layer listens via the UartBridge::wait_fn callback.
        // We store the payload in a global and signal the waiting task.
        ESP_LOGI(TAG, "NFC data received (%zu bytes)", payload.size());
        // (handled inside Tools::tool_flipper_nfc_read via bridge_.wait_fn)
        break;

    case MsgType::HW_SUBGHZ_DATA:
        ESP_LOGI(TAG, "SubGHz data received (%zu bytes)", payload.size());
        break;

    case MsgType::UNKNOWN:
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// UART bridge for hardware tools — allows Tools to send HW: commands and
// wait for the Flipper's response
// ---------------------------------------------------------------------------

// NFC response mailbox
static std::string        g_nfc_data;
static bool               g_nfc_ready = false;

static void uart_bridge_send(const std::string& type, const std::string& payload) {
    if (type == "__raw__") {
        // payload is the complete pre-formatted message
        g_uart.send_raw(payload);
    } else if (type == "HW:NFC:READ") {
        g_nfc_ready = false;
        g_uart.send_raw("HW:NFC:READ\n");
    } else {
        g_uart.send(type, payload);
    }
}

static bool uart_bridge_wait(const std::string& msg_type, uint32_t timeout_ms,
                              std::string& out_payload) {
    if (msg_type == "HW_NFC_DATA") {
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
        while (xTaskGetTickCount() < deadline) {
            if (g_nfc_ready) {
                out_payload = g_nfc_data;
                g_nfc_ready = false;
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return false;
    }
    return false;
}

// Extend on_uart_message to set NFC mailbox
static void on_uart_message_extended(MsgType type, const std::string& payload) {
    if (type == MsgType::HW_NFC_DATA) {
        g_nfc_data  = payload;
        g_nfc_ready = true;
    }
    on_uart_message(type, payload);
}

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "FlipperClaw booting...");
    ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

    // 1. NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase — erasing and re-initialising");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "NVS initialised");

    // 2. SPIFFS / MemoryStore
    ESP_ERROR_CHECK(g_memory.init());

    // 3. WiFi
    g_wifi.set_status_callback([](const std::string& msg) {
        g_uart.send("STATUS", msg);
    });
    ESP_ERROR_CHECK(g_wifi.init());
    ESP_ERROR_CHECK(g_wifi.connect());

    // 4. UART protocol (Flipper ↔ ESP32)
    ESP_ERROR_CHECK(g_uart.init(
        static_cast<uart_port_t>(FC_UART_PORT),
        FC_UART_TX_PIN,
        FC_UART_RX_PIN,
        UART_BAUD
    ));
    g_uart.set_callback(on_uart_message_extended);

    // 5. LLM API
    ESP_ERROR_CHECK(g_llm.init());

    // 6. Tools — inject UART bridge
    UartBridge bridge;
    bridge.send_fn = uart_bridge_send;
    bridge.wait_fn = uart_bridge_wait;
    ESP_ERROR_CHECK(g_tools.init(bridge));

    // 7. Cron scheduler
    ESP_ERROR_CHECK(g_cron.init(&g_memory));
    g_tools.set_cron(&g_cron);

    // 8. Agent
    g_agent.init(&g_uart, &g_llm, &g_tools, &g_memory);

    // 8. Agent semaphore + task
    g_agent_sem   = xSemaphoreCreateBinary();
    g_prompt_mutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(agent_task,        "agent",     8192, nullptr, 5, nullptr, 1);
    xTaskCreatePinnedToCore(cron_task,         "cron",      4096, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(heartbeat_md_task, "heartbeat", 4096, nullptr, 3, nullptr, 0);

    // 9. CLI (UART0 / USB serial)
    g_cli.init(&g_llm, &g_wifi, &g_memory, &g_uart);

    // 10. Heartbeat timer (10 s interval)
    esp_timer_handle_t heartbeat_timer;
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = heartbeat_timer_cb;
    timer_args.name     = "heartbeat";
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &heartbeat_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(heartbeat_timer,
                                             PING_INTERVAL_MS * 1000ULL));

    ESP_LOGI(TAG, "FlipperClaw running. Free heap: %lu bytes",
             (unsigned long)esp_get_free_heap_size());

    // app_main returns — FreeRTOS scheduler continues running tasks
}
