/**
 * @file memory_store.cpp
 * @brief SPIFFS read/write/append wrapper for agent memory files.
 */

#include "memory_store.h"
#include "constants.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_spiffs.h"

static const char* TAG = "memory_store";

static const char* DEFAULT_SOUL =
    "You are FlipperClaw, a pocket AI assistant running on a Flipper Zero.\n"
    "You are helpful, concise, and aware that responses display on a 128x64 monochrome screen.\n"
    "Keep responses under 200 words unless the user asks for detail.\n"
    "You have access to tools and can interact with the Flipper Zero's hardware.\n";

static const char* DEFAULT_USER =
    "# User Profile\n"
    "Name: (not set)\n"
    "Preferences: (not set)\n"
    "Notes: Edit this file to tell FlipperClaw about yourself.\n";

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

esp_err_t MemoryStore::init() {
    if (mounted_) return ESP_OK;

    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path              = SPIFFS_BASE_PATH;
    conf.partition_label        = SPIFFS_PARTITION_LABEL;
    conf.max_files              = 10;
    conf.format_if_mount_failed = true;

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    mounted_ = true;
    ESP_LOGI(TAG, "SPIFFS mounted at %s", SPIFFS_BASE_PATH);

    size_t total = 0, used = 0;
    esp_spiffs_info(SPIFFS_PARTITION_LABEL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: %zu KB total, %zu KB used", total / 1024, used / 1024);

    // Write default SOUL.md on first boot
    if (!exists("SOUL.md")) {
        write_default_soul();
    }
    // Write default USER.md on first boot
    if (!exists("USER.md")) {
        write("USER.md", DEFAULT_USER);
    }
    // Write default HEARTBEAT.md on first boot
    if (!exists("HEARTBEAT.md")) {
        write("HEARTBEAT.md",
            "# Heartbeat Tasks\n"
            "Add tasks here. FlipperClaw checks this file every 30 minutes.\n"
            "Mark completed tasks with - [x]. Unmarked items will be acted on.\n\n"
            "<!-- Example:\n"
            "- [ ] Send me a daily weather summary\n"
            "- [x] Remind me to drink water\n"
            "-->\n");
    }
    return ESP_OK;
}

void MemoryStore::write_default_soul() {
    ESP_LOGI(TAG, "Writing default SOUL.md");
    write("SOUL.md", DEFAULT_SOUL);
}

// ---------------------------------------------------------------------------
// Path helper
// ---------------------------------------------------------------------------

std::string MemoryStore::full_path(const std::string& filename) {
    return std::string(SPIFFS_BASE_PATH) + "/" + filename;
}

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------

esp_err_t MemoryStore::read(const std::string& filename, std::string& content) {
    std::string path = full_path(filename);
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        ESP_LOGD(TAG, "read: %s not found", filename.c_str());
        return ESP_ERR_NOT_FOUND;
    }

    content.clear();
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        content += buf;
    }
    fclose(f);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Write
// ---------------------------------------------------------------------------

esp_err_t MemoryStore::write(const std::string& filename, const std::string& content) {
    std::string path = full_path(filename);
    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        ESP_LOGE(TAG, "write: failed to open %s for writing", filename.c_str());
        return ESP_FAIL;
    }
    fwrite(content.c_str(), 1, content.size(), f);
    fclose(f);
    ESP_LOGD(TAG, "write: %s (%zu bytes)", filename.c_str(), content.size());
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Append
// ---------------------------------------------------------------------------

esp_err_t MemoryStore::append(const std::string& filename, const std::string& line) {
    std::string path = full_path(filename);
    FILE* f = fopen(path.c_str(), "a");
    if (!f) {
        ESP_LOGE(TAG, "append: failed to open %s", filename.c_str());
        return ESP_FAIL;
    }
    fwrite(line.c_str(), 1, line.size(), f);
    fputc('\n', f);
    fclose(f);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

bool MemoryStore::exists(const std::string& filename) {
    std::string path = full_path(filename);
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

size_t MemoryStore::size(const std::string& filename) {
    std::string path = full_path(filename);
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return static_cast<size_t>(st.st_size);
}

void MemoryStore::partition_info(size_t& total_bytes, size_t& used_bytes) {
    esp_spiffs_info(SPIFFS_PARTITION_LABEL, &total_bytes, &used_bytes);
}
