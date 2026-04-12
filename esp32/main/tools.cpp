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
#include "esp_http_client.h"
#include "esp_tls.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "tools";

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

struct HttpBuf {
    std::string body;
};

static esp_err_t simple_http_event(esp_http_client_event_t* evt) {
    auto* buf = static_cast<HttpBuf*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
        buf->body.append(static_cast<const char*>(evt->data), evt->data_len);
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
    char brave_key[128]  = FC_SECRET_BRAVE_KEY;
    {
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            size_t len = sizeof(tavily_key);
            nvs_get_str(nvs, "tavily_key", tavily_key, &len);
            len = sizeof(brave_key);
            nvs_get_str(nvs, "brave_key", brave_key, &len);
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
            out += std::string(r["title"] | "") + "\n";
            out += std::string(r["url"]   | "") + "\n";
            out += std::string(r["content"] | "") + "\n\n";
        }
        return out.empty() ? "No results found." : out;
    }

    if (brave_key[0]) {
        std::string url = std::string("https://api.search.brave.com/res/v1/web/search?q=") + query + "&count=3";
        std::string resp = http_post_json(
            url.c_str(),
            {{"Accept", "application/json"},
             {"Accept-Encoding", "gzip, deflate"},
             {"X-Subscription-Token", brave_key}},
            ""
        );

        JsonDocument res;
        if (deserializeJson(res, resp) != DeserializationError::Ok) {
            return "Error: failed to parse Brave response";
        }

        std::string out;
        int count = 0;
        for (JsonVariant r : res["web"]["results"].as<JsonArray>()) {
            if (count++ >= 3) break;
            out += std::string(r["title"]       | "") + "\n";
            out += std::string(r["url"]         | "") + "\n";
            out += std::string(r["description"] | "") + "\n\n";
        }
        return out.empty() ? "No results found." : out;
    }

    return "Error: no search API key configured. Use 'fc> set_tavily_key' or 'fc> set_brave_key'.";
}

// ---------------------------------------------------------------------------
// get_current_time
// ---------------------------------------------------------------------------

std::string Tools::tool_get_current_time(JsonObjectConst /*params*/) {
    std::string body = http_get("http://worldtimeapi.org/api/ip");
    if (body.empty()) return "Error: could not reach worldtimeapi.org";

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        return "Error: failed to parse time API response";
    }
    const char* dt = doc["datetime"] | "";
    const char* tz = doc["timezone"] | "";
    if (!dt || !dt[0]) return "Error: datetime not in response";
    return std::string("Current time: ") + dt + " (" + tz + ")";
}

// ---------------------------------------------------------------------------
// remember
// ---------------------------------------------------------------------------

std::string Tools::tool_remember(JsonObjectConst params) {
    const char* content = params["content"] | "";
    if (!content || !content[0]) return "Error: missing content";

    MemoryStore store;
    // store is already mounted by the time tools run; append directly
    store.append("MEMORY.md", std::string(content));
    ESP_LOGI(TAG, "Remembered: %s", content);
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

std::string Tools::tool_flipper_subghz_replay(JsonObjectConst params) {
    const char* filename = params["filename"] | "";
    if (!filename || !filename[0]) return "Error: missing filename";
    if (!bridge_.send_fn) return "Error: UART bridge not available";

    // HW:SUBGHZ:REPLAY is sent raw (filename is ASCII, no base64 needed)
    std::string msg = std::string("HW:SUBGHZ:REPLAY:") + filename + "\n";
    bridge_.send_fn("__raw__", msg); // special raw signal to uart layer
    return std::string("Sub-GHz replay triggered: ") + filename;
}

// ---------------------------------------------------------------------------
// flipper_ir_send
// ---------------------------------------------------------------------------

std::string Tools::tool_flipper_ir_send(JsonObjectConst params) {
    const char* filename = params["filename"] | "";
    if (!filename || !filename[0]) return "Error: missing filename";
    if (!bridge_.send_fn) return "Error: UART bridge not available";

    std::string msg = std::string("HW:IR:SEND:") + filename + "\n";
    bridge_.send_fn("__raw__", msg);
    return std::string("IR send triggered: ") + filename;
}
