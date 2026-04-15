/**
 * @file tools.cpp
 * @brief Tool registry — web search, time, memory, and Flipper hardware tools.
 */

#include "tools.h"
#include "constants.h"
#include "memory_store.h"
#include "fc_secrets.h"
#include <cstring>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctime>
#include <sys/time.h>
#include "esp_spiffs.h"

static const char* TAG = "tools";

// Percent-encode a string for use in a URL query parameter.
static std::string url_encode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Init — register all tools
// ---------------------------------------------------------------------------

esp_err_t Tools::init(UartBridge bridge) {
    bridge_ = std::move(bridge);

    register_tool({
        "web_search",
        "Search the web and return the top 3 results with titles and snippets.",
        R"({
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "Search query"}
            },
            "required": ["query"]
        })",
        [this](JsonObjectConst p) { return tool_web_search(p); }
    });

    register_tool({
        "get_current_time",
        "Get the current date and time.",
        R"({"type": "object", "properties": {}})",
        [this](JsonObjectConst p) { return tool_get_current_time(p); }
    });

    register_tool({
        "remember",
        "Persist a piece of information to long-term memory (MEMORY.md).",
        R"({
            "type": "object",
            "properties": {
                "content": {"type": "string", "description": "Information to remember"}
            },
            "required": ["content"]
        })",
        [this](JsonObjectConst p) { return tool_remember(p); }
    });

    register_tool({
        "flipper_nfc_read",
        "Ask the Flipper Zero to read a nearby NFC tag. Returns the tag bytes as a hex string.",
        R"({"type": "object", "properties": {}})",
        [this](JsonObjectConst p) { return tool_flipper_nfc_read(p); }
    });

    register_tool({
        "flipper_subghz_replay",
        "Replay a Sub-GHz capture file stored on the Flipper SD card.",
        R"({
            "type": "object",
            "properties": {
                "filename": {"type": "string", "description": "Filename of the .sub file on the Flipper SD card"}
            },
            "required": ["filename"]
        })",
        [this](JsonObjectConst p) { return tool_flipper_subghz_replay(p); }
    });

    register_tool({
        "flipper_ir_send",
        "Send an IR signal from a file stored on the Flipper SD card.",
        R"({
            "type": "object",
            "properties": {
                "filename": {"type": "string", "description": "Filename of the .ir file on the Flipper SD card"}
            },
            "required": ["filename"]
        })",
        [this](JsonObjectConst p) { return tool_flipper_ir_send(p); }
    });

    register_tool({
        "cron_add",
        "Schedule a recurring or one-shot task. For recurring jobs set recurring=true and interval_s. "
        "For one-shot jobs set recurring=false and fire_at (unix timestamp). Returns the job ID.",
        R"json({
            "type": "object",
            "properties": {
                "message":    {"type": "string",  "description": "Prompt to inject when the job fires"},
                "recurring":  {"type": "boolean", "description": "true = repeating, false = one-shot"},
                "interval_s": {"type": "integer", "description": "Recurring: seconds between fires (min 60)"},
                "fire_at":    {"type": "integer", "description": "One-shot: unix timestamp to fire at"}
            },
            "required": ["message", "recurring"]
        })json",
        [this](JsonObjectConst p) { return tool_cron_add(p); }
    });

    register_tool({
        "cron_list",
        "List all scheduled cron jobs.",
        R"({"type": "object", "properties": {}})",
        [this](JsonObjectConst p) { return tool_cron_list(p); }
    });

    register_tool({
        "cron_remove",
        "Remove a scheduled cron job by ID.",
        R"({
            "type": "object",
            "properties": {
                "id": {"type": "string", "description": "Job ID returned by cron_add"}
            },
            "required": ["id"]
        })",
        [this](JsonObjectConst p) { return tool_cron_remove(p); }
    });

    ESP_LOGI(TAG, "Registered %zu tools", registry_.size());
    return ESP_OK;
}

void Tools::register_tool(ToolDef def) {
    std::string name = def.name;
    registry_[name] = std::move(def);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

std::string Tools::dispatch(const std::string& tool_name, const std::string& args_json) {
    auto it = registry_.find(tool_name);
    if (it == registry_.end()) {
        ESP_LOGW(TAG, "Unknown tool: %s", tool_name.c_str());
        return "Error: unknown tool '" + tool_name + "'";
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, args_json);
    if (err) {
        ESP_LOGW(TAG, "Tool arg parse error: %s", err.c_str());
        return "Error: invalid tool arguments";
    }

    ESP_LOGI(TAG, "Dispatching tool: %s", tool_name.c_str());
    return it->second.handler(doc.as<JsonObjectConst>());
}

// ---------------------------------------------------------------------------
// Tool definitions JSON builders
// ---------------------------------------------------------------------------

std::string Tools::build_anthropic_tools_json() const {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& [name, def] : registry_) {
        JsonObject tool = arr.add<JsonObject>();
        tool["name"]        = def.name;
        tool["description"] = def.description;
        JsonDocument schema_doc;
        if (deserializeJson(schema_doc, def.parameters_schema) == DeserializationError::Ok) {
            tool["input_schema"] = schema_doc.as<JsonObject>();
        }
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

std::string Tools::build_openai_tools_json() const {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& [name, def] : registry_) {
        JsonObject tool = arr.add<JsonObject>();
        tool["type"] = "function";
        JsonObject fn = tool["function"].to<JsonObject>();
        fn["name"]        = def.name;
        fn["description"] = def.description;
        JsonDocument schema_doc;
        if (deserializeJson(schema_doc, def.parameters_schema) == DeserializationError::Ok) {
            fn["parameters"] = schema_doc.as<JsonObject>();
        }
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

// ---------------------------------------------------------------------------
// Helper: simple HTTPS GET → response body
// ---------------------------------------------------------------------------

// Hard cap on tool HTTP responses — prevents heap exhaustion from large API replies.
static constexpr size_t MAX_HTTP_RESPONSE_BYTES = 32 * 1024; // 32 KB

struct HttpBuf {
    std::string body;
    bool truncated = false;
};

static esp_err_t simple_http_event(esp_http_client_event_t* evt) {
    auto* buf = static_cast<HttpBuf*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
        if (buf->body.size() >= MAX_HTTP_RESPONSE_BYTES) {
            buf->truncated = true;
            return ESP_FAIL; // abort transfer — prevents further heap growth
        }
        size_t space = MAX_HTTP_RESPONSE_BYTES - buf->body.size();
        size_t to_append = (evt->data_len < (int)space)
                           ? (size_t)evt->data_len : space;
        buf->body.append(static_cast<const char*>(evt->data), to_append);
    }
    return ESP_OK;
}

static std::string http_get(const char* url) {
    HttpBuf buf;
    esp_http_client_config_t cfg = {};
    cfg.url               = url;
    cfg.event_handler     = simple_http_event;
    cfg.user_data         = &buf;
    cfg.timeout_ms        = HTTP_NETWORK_TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    auto* client = esp_http_client_init(&cfg);
    if (!client) return "";
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    return buf.body;
}

static std::string http_post_json(const char* url,
                                   const std::vector<std::pair<std::string,std::string>>& headers,
                                   const std::string& body) {
    HttpBuf buf;
    esp_http_client_config_t cfg = {};
    cfg.url               = url;
    cfg.method            = HTTP_METHOD_POST;
    cfg.event_handler     = simple_http_event;
    cfg.user_data         = &buf;
    cfg.timeout_ms        = HTTP_NETWORK_TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    auto* client = esp_http_client_init(&cfg);
    if (!client) return "";
    for (const auto& h : headers) {
        esp_http_client_set_header(client, h.first.c_str(), h.second.c_str());
    }
    esp_http_client_set_post_field(client, body.c_str(), body.size());
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    return buf.body;
}

// ---------------------------------------------------------------------------
// web_search
// ---------------------------------------------------------------------------

std::string Tools::tool_web_search(JsonObjectConst params) {
    const char* query = params["query"] | "";
    if (!query || !query[0]) return "Error: missing query";

    // Load search keys from NVS / secrets
    char tavily_key[128] = FC_SECRET_TAVILY_KEY;
    char exa_key[128]    = FC_SECRET_EXA_KEY;
    {
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            size_t len = sizeof(tavily_key);
            nvs_get_str(nvs, "tavily_key", tavily_key, &len);
            len = sizeof(exa_key);
            nvs_get_str(nvs, "exa_key", exa_key, &len);
            nvs_close(nvs);
        }
    }

    if (tavily_key[0]) {
        // Tavily search
        JsonDocument req;
        req["api_key"]         = tavily_key;
        req["query"]           = query;
        req["max_results"]     = 3;
        req["include_answer"]  = false;
        std::string body;
        serializeJson(req, body);

        std::string resp = http_post_json(
            "https://api.tavily.com/search",
            {{"Content-Type", "application/json"}},
            body
        );

        JsonDocument res;
        if (deserializeJson(res, resp) != DeserializationError::Ok) {
            return "Error: failed to parse Tavily response";
        }

        std::string out;
        int count = 0;
        for (JsonVariant r : res["results"].as<JsonArray>()) {
            if (count++ >= 3) break;
            out += std::string(r["title"]   | "") + "\n";
            out += std::string(r["url"]     | "") + "\n";
            out += std::string(r["content"] | "") + "\n\n";
        }
        return out.empty() ? "No results found." : out;
    }

    if (exa_key[0]) {
        // Exa neural search
        JsonDocument req;
        req["query"]      = query;
        req["numResults"] = 3;
        req["type"]       = "neural";
        req["contents"]["text"]["maxCharacters"] = 500;
        std::string body;
        serializeJson(req, body);

        std::string resp = http_post_json(
            "https://api.exa.ai/search",
            {{"Content-Type", "application/json"}, {"x-api-key", exa_key}},
            body
        );

        JsonDocument res;
        if (deserializeJson(res, resp) != DeserializationError::Ok) {
            return "Error: failed to parse Exa response";
        }

        std::string out;
        int count = 0;
        for (JsonVariant r : res["results"].as<JsonArray>()) {
            if (count++ >= 3) break;
            out += std::string(r["title"] | "") + "\n";
            out += std::string(r["url"]   | "") + "\n";
            out += std::string(r["text"]  | "") + "\n\n";
        }
        return out.empty() ? "No results found." : out;
    }

    return "Error: no search API key configured. Use 'fc> set_tavily_key' or 'fc> set_exa_key'.";
}

// ---------------------------------------------------------------------------
// cron_add / cron_list / cron_remove
// ---------------------------------------------------------------------------

std::string Tools::tool_cron_add(JsonObjectConst params) {
    if (!cron_) return "Error: cron scheduler not available";

    const char* message = params["message"] | "";
    if (!message || !message[0]) return "Error: message is required";

    bool     recurring  = params["recurring"]  | false;
    uint32_t interval_s = params["interval_s"] | 3600U;
    time_t   fire_at    = (time_t)(params["fire_at"] | 0);

    if (!recurring && fire_at <= 0) {
        return "Error: one-shot jobs require fire_at (unix timestamp)";
    }

    std::string id = cron_->add(message, recurring, interval_s, fire_at);
    if (id.empty()) return "Error: failed to create job";
    return "Scheduled job " + id + ". It will fire " +
           (recurring ? "every " + std::to_string(interval_s) + "s"
                      : "once at unix " + std::to_string((long)fire_at)) + ".";
}

std::string Tools::tool_cron_list(JsonObjectConst /*params*/) {
    if (!cron_) return "Error: cron scheduler not available";

    auto jobs = cron_->list();
    if (jobs.empty()) return "No scheduled jobs.";

    std::string out;
    for (const auto& job : jobs) {
        out += job.id + ": ";
        if (job.recurring) {
            out += "every " + std::to_string(job.interval_s) + "s";
        } else {
            out += "once at unix " + std::to_string((long)job.fire_at);
        }
        out += " — \"" + job.message.substr(0, 60) + "\"\n";
    }
    return out;
}

std::string Tools::tool_cron_remove(JsonObjectConst params) {
    if (!cron_) return "Error: cron scheduler not available";

    const char* id = params["id"] | "";
    if (!id || !id[0]) return "Error: id is required";

    if (cron_->remove(std::string(id))) {
        return std::string("Job ") + id + " removed.";
    }
    return std::string("Error: job ") + id + " not found.";
}

// ---------------------------------------------------------------------------
// get_current_time
// ---------------------------------------------------------------------------

std::string Tools::tool_get_current_time(JsonObjectConst /*params*/) {
    std::string body = http_get("https://worldtimeapi.org/api/timezone/UTC");
    if (body.empty()) return "Error: could not reach worldtimeapi.org";

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        return "Error: failed to parse time API response";
    }

    const char* dt = doc["datetime"] | "";
    long unixtime   = doc["unixtime"] | 0L;

    if (!dt || !dt[0]) return "Error: datetime not in response";

    // Sync system clock so time() works for daily notes etc.
    if (unixtime > 0) {
        struct timeval tv = { .tv_sec = (time_t)unixtime, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        ESP_LOGI(TAG, "System clock set to unix=%ld", unixtime);
    }

    return std::string("Current UTC time: ") + dt;
}

// ---------------------------------------------------------------------------
// remember
// ---------------------------------------------------------------------------

std::string Tools::tool_remember(JsonObjectConst params) {
    const char* content = params["content"] | "";
    if (!content || !content[0]) return "Error: missing content";

    // Cap individual entries — prevents a runaway agent from writing
    // arbitrarily large strings into MEMORY.md.
    if (strlen(content) > 512) return "Error: content too long (max 512 chars)";

    // Refuse to write if SPIFFS is nearly full — protects daily notes,
    // cron.json, and other files from being crowded out.
    size_t total = 0, used = 0;
    esp_spiffs_info(SPIFFS_PARTITION_LABEL, &total, &used);
    if (total > 0 && (total - used) < 10 * 1024) {
        ESP_LOGW(TAG, "SPIFFS nearly full (%zu KB free) — skipping remember", (total - used) / 1024);
        return "Error: storage nearly full, cannot remember right now";
    }

    MemoryStore store;
    store.append("MEMORY.md", std::string(content));
    ESP_LOGI(TAG, "Remembered (%zu bytes)", strlen(content));
    return "Remembered.";
}

// ---------------------------------------------------------------------------
// flipper_nfc_read
// ---------------------------------------------------------------------------

std::string Tools::tool_flipper_nfc_read(JsonObjectConst /*params*/) {
    if (!bridge_.send_fn || !bridge_.wait_fn) {
        return "Error: UART bridge not available";
    }
    bridge_.send_fn("HW:NFC:READ", "");

    std::string raw_bytes;
    bool got = bridge_.wait_fn("HW_NFC_DATA", NFC_WAIT_TIMEOUT_MS, raw_bytes);
    if (!got) return "Error: NFC read timed out (no tag presented within 30s)";

    // Format as hex string
    std::string hex;
    hex.reserve(raw_bytes.size() * 3);
    char nibble[4];
    for (unsigned char c : raw_bytes) {
        snprintf(nibble, sizeof(nibble), "%02X ", c);
        hex += nibble;
    }
    if (!hex.empty()) hex.pop_back(); // remove trailing space
    return "NFC tag bytes: " + hex;
}

// ---------------------------------------------------------------------------
// flipper_subghz_replay
// ---------------------------------------------------------------------------

// Returns true if every character in filename is safe to embed in a UART line.
// Rejects control characters (including \n, \r) and anything outside the
// printable ASCII subset used by Flipper file names.
static bool hw_filename_safe(const char* filename) {
    for (size_t i = 0; filename[i]; i++) {
        unsigned char c = (unsigned char)filename[i];
        // Allow: alphanumeric, '.', '-', '_', '/', space
        if (!isalnum(c) && c != '.' && c != '-' && c != '_' && c != '/' && c != ' ')
            return false;
    }
    return true;
}

std::string Tools::tool_flipper_subghz_replay(JsonObjectConst params) {
    const char* filename = params["filename"] | "";
    if (!filename || !filename[0]) return "Error: missing filename";
    if (strlen(filename) > 200)    return "Error: filename too long";
    if (!hw_filename_safe(filename)) return "Error: filename contains invalid characters";
    if (!bridge_.send_fn) return "Error: UART bridge not available";

    std::string msg = std::string("HW:SUBGHZ:REPLAY:") + filename + "\n";
    bridge_.send_fn("__raw__", msg);
    return std::string("Sub-GHz replay triggered: ") + filename;
}

// ---------------------------------------------------------------------------
// flipper_ir_send
// ---------------------------------------------------------------------------

std::string Tools::tool_flipper_ir_send(JsonObjectConst params) {
    const char* filename = params["filename"] | "";
    if (!filename || !filename[0]) return "Error: missing filename";
    if (strlen(filename) > 200)    return "Error: filename too long";
    if (!hw_filename_safe(filename)) return "Error: filename contains invalid characters";
    if (!bridge_.send_fn) return "Error: UART bridge not available";

    std::string msg = std::string("HW:IR:SEND:") + filename + "\n";
    bridge_.send_fn("__raw__", msg);
    return std::string("IR send triggered: ") + filename;
}
