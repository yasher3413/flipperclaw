/**
 * @file wifi_client.cpp
 * @brief WiFi connection manager — connect, reconnect, status callbacks.
 */

#include "wifi_client.h"
#include "constants.h"
#include "fc_secrets.h"
#include <cstring>
#include <algorithm>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "wifi_client";

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

esp_err_t WifiClient::init() {
    if (initialised_) return ESP_OK;

    event_group_ = xEventGroupCreate();
    if (!event_group_) return ESP_ERR_NO_MEM;

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event_loop_create failed");

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            wifi_event_handler, this, nullptr),
        TAG, "register WIFI_EVENT failed"
    );
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            wifi_event_handler, this, nullptr),
        TAG, "register IP_EVENT failed"
    );

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode failed");

    initialised_ = true;
    ESP_LOGI(TAG, "WiFi driver initialised");
    return ESP_OK;
}

esp_err_t WifiClient::connect() {
    if (!initialised_) {
        ESP_LOGE(TAG, "call init() before connect()");
        return ESP_ERR_INVALID_STATE;
    }

    // Load credentials: prefer NVS, fall back to compile-time secrets
    char ssid[64] = FC_SECRET_WIFI_SSID;
    char pass[64] = FC_SECRET_WIFI_PASS;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(ssid);
        nvs_get_str(nvs, "wifi_ssid", ssid, &len);
        len = sizeof(pass);
        nvs_get_str(nvs, "wifi_pass", pass, &len);
        nvs_close(nvs);
    }

    wifi_config_t wifi_cfg = {};
    strncpy(reinterpret_cast<char*>(wifi_cfg.sta.ssid),     ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(wifi_cfg.sta.password), pass, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set_config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    if (status_cb_) status_cb_("Connecting to WiFi...");
    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    return ESP_OK;
}

WifiClient::~WifiClient() {
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    if (event_group_) vEventGroupDelete(event_group_);
}

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

bool WifiClient::wait_connected(uint32_t timeout_ms) {
    EventBits_t bits = xEventGroupWaitBits(
        event_group_,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms)
    );
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool WifiClient::is_connected() const {
    if (!event_group_) return false;
    return (xEventGroupGetBits(event_group_) & WIFI_CONNECTED_BIT) != 0;
}

void WifiClient::set_status_callback(std::function<void(const std::string&)> cb) {
    status_cb_ = std::move(cb);
}

esp_err_t WifiClient::set_credentials(const std::string& ssid, const std::string& password) {
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(
        nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs),
        TAG, "nvs_open failed"
    );
    nvs_set_str(nvs, "wifi_ssid", ssid.c_str());
    nvs_set_str(nvs, "wifi_pass", password.c_str());
    esp_err_t err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

// ---------------------------------------------------------------------------
// Event handler
// ---------------------------------------------------------------------------

void WifiClient::wifi_event_handler(void* arg, esp_event_base_t base,
                                    int32_t id, void* data) {
    auto* self = static_cast<WifiClient*>(arg);

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(self->event_group_, WIFI_CONNECTED_BIT);
        self->on_disconnected();

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        self->on_connected();
    }
}

void WifiClient::on_connected() {
    retry_count_ = 0;
    backoff_ms_  = WIFI_BACKOFF_BASE_MS;
    xEventGroupSetBits(event_group_, WIFI_CONNECTED_BIT);
    xEventGroupClearBits(event_group_, WIFI_FAIL_BIT);
    ESP_LOGI(TAG, "WiFi connected");
    if (status_cb_) status_cb_("WiFi connected");
}

void WifiClient::on_disconnected() {
    ESP_LOGW(TAG, "WiFi disconnected — reconnecting in %lu ms (attempt %d)",
             backoff_ms_, retry_count_ + 1);
    if (status_cb_) status_cb_("WiFi reconnecting...");

    vTaskDelay(pdMS_TO_TICKS(backoff_ms_));
    backoff_ms_ = std::min(backoff_ms_ * 2, WIFI_BACKOFF_MAX_MS);
    ++retry_count_;
    esp_wifi_connect();
}
