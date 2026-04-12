#pragma once
#include <string>
#include <vector>
#include <ctime>
#include "esp_err.h"
#include "memory_store.h"

/**
 * @file cron.h
 * @brief Persistent cron scheduler — recurring and one-shot jobs stored in cron.json.
 *
 * The LLM creates/removes jobs via the cron_add / cron_remove / cron_list tools.
 * The cron_task in main.cpp calls poll() every CRON_POLL_INTERVAL_S seconds;
 * any due jobs are returned as prompts to inject into the agent loop.
 *
 * Job types:
 *   recurring  — fires every interval_s seconds (minimum 60 s).
 *   one-shot   — fires once at unix timestamp fire_at, then auto-removes.
 */

struct CronJob {
    std::string id;          ///< Unique ID assigned on creation (e.g. "cron_1")
    std::string message;     ///< Prompt injected into agent when job fires
    bool        recurring;   ///< true = repeating, false = one-shot
    uint32_t    interval_s;  ///< Recurring: seconds between fires
    time_t      fire_at;     ///< One-shot: unix timestamp to fire at
    time_t      last_fired;  ///< Recurring: last fire time (0 = never fired)
};

class CronScheduler {
public:
    CronScheduler() = default;

    /**
     * @brief Load persisted jobs from cron.json on SPIFFS.
     * @param memory  Pointer to the initialised MemoryStore.
     * @return ESP_OK on success.
     */
    esp_err_t init(MemoryStore* memory);

    /**
     * @brief Create a new cron job.
     *
     * @param message     Prompt to inject when the job fires.
     * @param recurring   true = repeating, false = one-shot.
     * @param interval_s  Recurring: seconds between fires (ignored for one-shot).
     * @param fire_at     One-shot: unix timestamp to fire at (ignored for recurring).
     * @return Job ID string on success, empty string on failure.
     */
    std::string add(const std::string& message, bool recurring,
                    uint32_t interval_s, time_t fire_at);

    /**
     * @brief Remove a job by ID.
     * @return true if found and removed, false if not found.
     */
    bool remove(const std::string& id);

    /**
     * @brief Return a copy of all current jobs.
     */
    std::vector<CronJob> list() const;

    /**
     * @brief Check for due jobs and return them.
     *
     * For recurring jobs, updates last_fired. For one-shot jobs, removes them
     * after returning. Persists changes to SPIFFS if any jobs fired.
     * Returns an empty vector if the system clock is not synced (pre-2020).
     *
     * @return List of jobs that fired this poll cycle.
     */
    std::vector<CronJob> poll();

private:
    void save();
    void load();
    std::string next_id();

    std::vector<CronJob> jobs_;
    MemoryStore*         memory_{nullptr};
    uint32_t             id_counter_{0};
};
