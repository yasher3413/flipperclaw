#pragma once
#include <string>
#include "esp_err.h"

/**
 * @file memory_store.h
 * @brief SPIFFS-backed persistent storage for FlipperClaw agent memory files.
 *
 * Files live at /spiffs/<filename>. Standard files:
 *   SOUL.md       — Agent system prompt / personality (written on first boot)
 *   USER.md       — Information about the user
 *   MEMORY.md     — Long-term memory recalled by the agent
 *   session.jsonl — Current session conversation history
 *   YYYY-MM-DD.md — Daily summaries
 */
class MemoryStore {
public:
    MemoryStore() = default;

    /**
     * @brief Mount the SPIFFS partition and write default files if missing.
     * @return ESP_OK on success.
     */
    esp_err_t init();

    /**
     * @brief Read a file from SPIFFS.
     * @param filename  Filename relative to mount point (e.g. "SOUL.md").
     * @param content   Output: file contents as string.
     * @return ESP_OK on success, ESP_ERR_NOT_FOUND if file absent.
     */
    esp_err_t read(const std::string& filename, std::string& content);

    /**
     * @brief Write (overwrite) a file on SPIFFS.
     * @param filename  Filename relative to mount point.
     * @param content   Data to write.
     * @return ESP_OK on success.
     */
    esp_err_t write(const std::string& filename, const std::string& content);

    /**
     * @brief Append a line to a file (creates if absent).
     * @param filename  Filename relative to mount point.
     * @param line      Text to append (a '\n' is appended automatically).
     * @return ESP_OK on success.
     */
    esp_err_t append(const std::string& filename, const std::string& line);

    /**
     * @brief Check whether a file exists on SPIFFS.
     * @param filename  Filename relative to mount point.
     * @return true if file exists.
     */
    bool exists(const std::string& filename);

    /**
     * @brief Return the size of a file in bytes, or 0 if absent.
     * @param filename  Filename relative to mount point.
     */
    size_t size(const std::string& filename);

    /**
     * @brief Return total and used bytes on the SPIFFS partition.
     * @param total_bytes  Output: partition size.
     * @param used_bytes   Output: bytes in use.
     */
    void partition_info(size_t& total_bytes, size_t& used_bytes);

    /**
     * @brief Return today's daily note filename (e.g. "2026-04-12.md").
     *
     * Returns an empty string if the system clock has not been synced
     * (i.e. time() returns a value before 2020).
     */
    std::string today_filename();

private:
    std::string full_path(const std::string& filename);
    void write_default_soul();

    bool mounted_{false};
};
