/**
 * @file cron.cpp
 * @brief Persistent cron scheduler — load/save cron.json, poll for due jobs.
 */

#include "cron.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include "esp_log.h"
#include "ArduinoJson.h"

static const char* TAG = "cron";

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

esp_err_t CronScheduler::init(MemoryStore* memory) {
    memory_ = memory;
    load();
    ESP_LOGI(TAG, "CronScheduler ready — %zu job(s) loaded", jobs_.size());
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// ID generation
// ---------------------------------------------------------------------------

std::string CronScheduler::next_id() {
    char buf[16];
    snprintf(buf, sizeof(buf), "cron_%u", (unsigned)++id_counter_);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void CronScheduler::load() {
    if (!memory_) return;
    std::string json;
    if (memory_->read("cron.json", json) != ESP_OK || json.empty()) return;

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        ESP_LOGW(TAG, "Failed to parse cron.json — starting fresh");
        return;
    }

    for (JsonVariant v : doc.as<JsonArray>()) {
        CronJob job;
        job.id         = v["id"]         | "";
        job.message    = v["message"]    | "";
        job.recurring  = v["recurring"]  | false;
        job.interval_s = v["interval_s"] | 0U;
        job.fire_at    = (time_t)(v["fire_at"]    | 0);
        job.last_fired = (time_t)(v["last_fired"] | 0);
        if (!job.id.empty() && !job.message.empty()) {
            jobs_.push_back(std::move(job));
            // Keep id_counter ahead of any loaded IDs
            unsigned n = 0;
            if (sscanf(job.id.c_str(), "cron_%u", &n) == 1 && n >= id_counter_) {
                id_counter_ = n;
            }
        }
    }
}

void CronScheduler::save() {
    if (!memory_) return;
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& job : jobs_) {
        JsonObject obj = arr.add<JsonObject>();
        obj["id"]         = job.id;
        obj["message"]    = job.message;
        obj["recurring"]  = job.recurring;
        obj["interval_s"] = job.interval_s;
        obj["fire_at"]    = (long)job.fire_at;
        obj["last_fired"] = (long)job.last_fired;
    }
    std::string json;
    serializeJson(doc, json);
    memory_->write("cron.json", json);
    ESP_LOGD(TAG, "cron.json saved (%zu jobs)", jobs_.size());
}

// ---------------------------------------------------------------------------
// add
// ---------------------------------------------------------------------------

std::string CronScheduler::add(const std::string& message, bool recurring,
                                uint32_t interval_s, time_t fire_at) {
    if (message.empty()) return "";
    if (recurring && interval_s < 60) {
        ESP_LOGW(TAG, "Minimum interval is 60 s, clamping");
        interval_s = 60;
    }

    CronJob job;
    job.id         = next_id();
    job.message    = message;
    job.recurring  = recurring;
    job.interval_s = interval_s;
    job.fire_at    = fire_at;
    job.last_fired = 0;

    jobs_.push_back(job);
    save();
    ESP_LOGI(TAG, "Job added: %s recurring=%d interval=%us",
             job.id.c_str(), (int)recurring, interval_s);
    return job.id;
}

// ---------------------------------------------------------------------------
// remove
// ---------------------------------------------------------------------------

bool CronScheduler::remove(const std::string& id) {
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
        if (it->id == id) {
            jobs_.erase(it);
            save();
            ESP_LOGI(TAG, "Job removed: %s", id.c_str());
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// list
// ---------------------------------------------------------------------------

std::vector<CronJob> CronScheduler::list() const {
    return jobs_;
}

// ---------------------------------------------------------------------------
// poll
// ---------------------------------------------------------------------------

std::vector<CronJob> CronScheduler::poll() {
    time_t now = time(nullptr);
    if (now < 1577836800L) return {}; // clock not synced

    std::vector<CronJob> fired;
    bool changed = false;

    auto it = jobs_.begin();
    while (it != jobs_.end()) {
        bool due = false;
        if (it->recurring) {
            time_t next = it->last_fired + (time_t)it->interval_s;
            if (it->last_fired == 0 || now >= next) due = true;
        } else {
            if (now >= it->fire_at) due = true;
        }

        if (due) {
            fired.push_back(*it);
            ESP_LOGI(TAG, "Job fired: %s", it->id.c_str());
            if (it->recurring) {
                it->last_fired = now;
                ++it;
            } else {
                it = jobs_.erase(it); // one-shot: remove after firing
            }
            changed = true;
        } else {
            ++it;
        }
    }

    if (changed) save();
    return fired;
}
