#pragma once
#include <functional>
#include <string>
#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/**
 * @file wifi_client.h
 * @brief WiFi connection manager with automatic reconnect and status callbacks.
 *
 * Reads SSID and password from NVS (set via CLI or fc_secrets.h defaults).
 * Reconnects with exponential backoff (1s → 2s → 4s … 30s max) on disconnect.
 * Reports status changes as STATUS messages over UART via the optional callback.
 */
class WifiClient {
public:
    WifiClient() = default;
    ~WifiClient();

    /**
     * @brief Initialise TCP/IP stack, event loop, and WiFi driver.
     *
     * Must be called once after nvs_flash_init(). Reads credentials from NVS.
     * Falls back to FC_SECRET_WIFI_SSID / FC_SECRET_WIFI_PASS if NVS empty.
     *
     * @return ESP_OK on success.
     */
    esp_err_t init();

    /**
     * @brief Begin connection attempt (non-blocking).
     * @return ESP_OK if start succeeded.
     */
    esp_err_t connect();

    /**
     * @brief Block until WiFi is connected or timeout elapses.
     * @param timeout_ms  Maximum wait in milliseconds.
     * @return true if connected within timeout.
     */
    bool wait_connected(uint32_t timeout_ms);

    /**
     * @brief Return current connection state.
     */
    bool is_connected() const;

    /**
     * @brief Register a callback for status messages (e.g. "Connecting to WiFi...").
     *
     * The callback is called from the WiFi event task. Keep it short — post to
     * a queue rather than doing I/O inline.
     *
     * @param cb  Callback: (message) → void.
     */
    void set_status_callback(std::function<void(const std::string&)> cb);

    /**
     * @brief Update stored credentials in NVS (used by CLI).
     */
    esp_err_t set_credentials(const std::string& ssid, const std::string& password);

private:
    static void wifi_event_handler(void* arg, esp_event_base_t base,
                                   int32_t id, void* data);

    void on_connected();
    void on_disconnected();

    EventGroupHandle_t                    event_group_{nullptr};
    std::function<void(const std::string&)> status_cb_;
    bool                                  initialised_{false};
    int                                   retry_count_{0};
    uint32_t                              backoff_ms_{1000};

    static constexpr int WIFI_CONNECTED_BIT = BIT0;
    static constexpr int WIFI_FAIL_BIT      = BIT1;
};
