/**
 * @file llm_api.cpp
 * @brief Streaming HTTPS LLM client for Anthropic and OpenAI APIs.
 *
 * Uses esp_http_client in streaming mode. SSE lines are parsed incrementally
 * in the HTTP event handler. Tool calls are collected and returned after the
 * stream ends.
 */

#include "llm_api.h"
#include "constants.h"
#include "fc_secrets.h"
#include <cstring>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "ArduinoJson.h"

static const char* TAG = "llm_api";

// ---------------------------------------------------------------------------
// Streaming context shared between http_event_handler and stream_*()
// ---------------------------------------------------------------------------

struct StreamCtx {
    std::function<void(const std::string&)>* token_cb{nullptr};
    std::function<bool()>*                  cancelled_cb{nullptr};
    std::vector<ToolCall>*                  tool_calls{nullptr};

    // SSE line accumulation
    char   line_buf[MAX_MSG_LINE_LEN]{};
    size_t line_len{0};

    // Anthropic tool-use accumulation
    std::string current_tool_id;
    std::string current_tool_name;
    std::string current_tool_input;

    bool   done{false};
    bool   cancelled{false};
    std::string provider; // "anthropic" or "openai"
};

// ---------------------------------------------------------------------------
// SSE line dispatch
// ---------------------------------------------------------------------------

static void process_sse_line(StreamCtx& ctx, const char* line, size_t len) {
    // Strip trailing whitespace
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == ' ')) --len;
    if (len == 0) return;

    // Only process "data: " lines
    if (len < 6 || strncmp(line, "data: ", 6) != 0) return;
    const char* json_str = line + 6;
    size_t json_len = len - 6;

    if (strncmp(json_str, "[DONE]", 6) == 0) {
        ctx.done = true;
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json_str, json_len);
    if (err) {
        ESP_LOGD(TAG, "SSE JSON parse error: %s", err.c_str());
        return;
    }

    if (ctx.provider == "anthropic") {
        const char* event_type = doc["type"] | "";

        if (strcmp(event_type, "content_block_delta") == 0) {
            const char* delta_type = doc["delta"]["type"] | "";
            if (strcmp(delta_type, "text_delta") == 0) {
                const char* text = doc["delta"]["text"] | "";
                if (text && text[0] && ctx.token_cb) {
                    (*ctx.token_cb)(std::string(text));
                }
            } else if (strcmp(delta_type, "input_json_delta") == 0) {
                const char* partial = doc["delta"]["partial_json"] | "";
                ctx.current_tool_input += partial;
            }
        } else if (strcmp(event_type, "content_block_start") == 0) {
            const char* block_type = doc["content_block"]["type"] | "";
            if (strcmp(block_type, "tool_use") == 0) {
                ctx.current_tool_id    = doc["content_block"]["id"]   | "";
                ctx.current_tool_name  = doc["content_block"]["name"] | "";
                ctx.current_tool_input = "";
            }
        } else if (strcmp(event_type, "content_block_stop") == 0) {
            if (!ctx.current_tool_name.empty() && ctx.tool_calls) {
                ToolCall tc;
                tc.id        = ctx.current_tool_id;
                tc.name      = ctx.current_tool_name;
                tc.arguments = ctx.current_tool_input;
                ctx.tool_calls->push_back(std::move(tc));
                ctx.current_tool_name.clear();
                ctx.current_tool_id.clear();
                ctx.current_tool_input.clear();
            }
        } else if (strcmp(event_type, "message_stop") == 0) {
            ctx.done = true;
        }
    } else {
        // OpenAI SSE
        JsonVariant delta = doc["choices"][0]["delta"];
        if (delta.isNull()) return;

        const char* content = delta["content"] | "";
        if (content && content[0] && ctx.token_cb) {
            (*ctx.token_cb)(std::string(content));
        }

        // Tool calls
        JsonVariant tc_arr = delta["tool_calls"];
        if (!tc_arr.isNull() && tc_arr.is<JsonArray>() && ctx.tool_calls) {
            for (JsonVariant tc : tc_arr.as<JsonArray>()) {
                int idx = tc["index"] | 0;
                // Grow vector if needed
                while ((int)ctx.tool_calls->size() <= idx) {
                    ctx.tool_calls->emplace_back();
                }
                ToolCall& t = (*ctx.tool_calls)[idx];
                const char* name = tc["function"]["name"] | "";
                if (name && name[0]) t.name = name;
                const char* args = tc["function"]["arguments"] | "";
                if (args) t.arguments += args;
                const char* id = tc["id"] | "";
                if (id && id[0]) t.id = id;
            }
        }

        const char* finish = doc["choices"][0]["finish_reason"] | "";
        if (finish && strcmp(finish, "stop") == 0) ctx.done = true;
        if (finish && strcmp(finish, "tool_calls") == 0) ctx.done = true;
    }
}

// ---------------------------------------------------------------------------
// HTTP event handler
// ---------------------------------------------------------------------------

esp_err_t LlmApi::http_event_handler(esp_http_client_event_t* evt) {
    StreamCtx* ctx = static_cast<StreamCtx*>(evt->user_data);
    if (!ctx) return ESP_OK;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA: {
        if (ctx->cancelled_cb && (*ctx->cancelled_cb)()) {
            ctx->cancelled = true;
            return ESP_FAIL; // abort transfer
        }
        const char* data = static_cast<const char*>(evt->data);
        int data_len = evt->data_len;

        for (int i = 0; i < data_len; ++i) {
            char c = data[i];
            if (c == '\n') {
                if (ctx->line_len > 0) {
                    process_sse_line(*ctx, ctx->line_buf, ctx->line_len);
                    ctx->line_len = 0;
                }
            } else {
                if (ctx->line_len < sizeof(ctx->line_buf) - 1) {
                    ctx->line_buf[ctx->line_len++] = c;
                }
            }
        }
        break;
    }
    case HTTP_EVENT_ERROR:
        ESP_LOGW(TAG, "HTTP error event");
        break;
    default:
        break;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Init — load config from NVS
// ---------------------------------------------------------------------------

esp_err_t LlmApi::init() {
    api_key_  = FC_SECRET_API_KEY;
    provider_ = FC_SECRET_MODEL_PROVIDER;
    model_    = (provider_ == "openai") ? "gpt-4o-mini" : "claude-haiku-4-5-20251001";

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        char buf[128];
        size_t len;

        len = sizeof(buf);
        if (nvs_get_str(nvs, "api_key", buf, &len) == ESP_OK) api_key_ = buf;
        len = sizeof(buf);
        if (nvs_get_str(nvs, "provider", buf, &len) == ESP_OK) provider_ = buf;
        len = sizeof(buf);
        if (nvs_get_str(nvs, "model", buf, &len) == ESP_OK) model_ = buf;

        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "LLM provider=%s model=%s", provider_.c_str(), model_.c_str());
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// NVS setters
// ---------------------------------------------------------------------------

static esp_err_t nvs_set_string(const char* key, const std::string& val) {
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), "llm_api", "nvs_open");
    nvs_set_str(nvs, key, val.c_str());
    esp_err_t err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t LlmApi::set_api_key(const std::string& key) {
    api_key_ = key;
    return nvs_set_string("api_key", key);
}
esp_err_t LlmApi::set_provider(const std::string& p) {
    provider_ = p;
    return nvs_set_string("provider", p);
}
esp_err_t LlmApi::set_model(const std::string& m) {
    model_ = m;
    return nvs_set_string("model", m);
}

// ---------------------------------------------------------------------------
// Public stream() — dispatches to provider implementation
// ---------------------------------------------------------------------------

esp_err_t LlmApi::stream(
    const std::vector<Message>& messages,
    const std::string& system_prompt,
    const std::string& tool_defs,
    std::function<void(const std::string&)> token_cb,
    std::function<bool()> cancelled_cb,
    std::vector<ToolCall>& tool_calls)
{
    if (provider_ == "openai") {
        return stream_openai(messages, system_prompt, tool_defs,
                             token_cb, cancelled_cb, tool_calls);
    }
    return stream_anthropic(messages, system_prompt, tool_defs,
                            token_cb, cancelled_cb, tool_calls);
}

// ---------------------------------------------------------------------------
// Anthropic streaming
// ---------------------------------------------------------------------------

esp_err_t LlmApi::stream_anthropic(
    const std::vector<Message>& messages,
    const std::string& system_prompt,
    const std::string& tool_defs,
    std::function<void(const std::string&)> token_cb,
    std::function<bool()> cancelled_cb,
    std::vector<ToolCall>& tool_calls)
{
    // Build request body
    JsonDocument req;
    req["model"]      = model_;
    req["max_tokens"] = 1024;
    req["stream"]     = true;
    if (!system_prompt.empty()) req["system"] = system_prompt;

    JsonArray msgs = req["messages"].to<JsonArray>();
    for (const auto& m : messages) {
        JsonObject obj = msgs.add<JsonObject>();
        obj["role"]    = m.role;
        obj["content"] = m.content;
    }

    if (!tool_defs.empty()) {
        JsonDocument tools_doc;
        if (deserializeJson(tools_doc, tool_defs) == DeserializationError::Ok) {
            req["tools"] = tools_doc.as<JsonArray>();
        }
    }

    std::string body;
    serializeJson(req, body);

    StreamCtx ctx;
    ctx.token_cb     = &token_cb;
    ctx.cancelled_cb = &cancelled_cb;
    ctx.tool_calls   = &tool_calls;
    ctx.provider     = "anthropic";

    esp_http_client_config_t cfg = {};
    cfg.url                = "https://api.anthropic.com/v1/messages";
    cfg.method             = HTTP_METHOD_POST;
    cfg.timeout_ms         = HTTP_NETWORK_TIMEOUT_MS;
    cfg.buffer_size        = 4096;
    cfg.buffer_size_tx     = 4096;
    cfg.event_handler      = http_event_handler;
    cfg.user_data          = &ctx;
    cfg.crt_bundle_attach  = esp_crt_bundle_attach;
    cfg.keep_alive_enable  = true;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    std::string auth = "Bearer " + api_key_;
    esp_http_client_set_header(client, "Content-Type",      "application/json");
    esp_http_client_set_header(client, "x-api-key",         api_key_.c_str());
    esp_http_client_set_header(client, "anthropic-version", "2023-06-01");
    esp_http_client_set_header(client, "Authorization",     auth.c_str());
    esp_http_client_set_post_field(client, body.c_str(), body.size());

    esp_err_t ret = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ctx.cancelled) return ESP_ERR_NOT_FINISHED;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Anthropic API returned HTTP %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// OpenAI streaming
// ---------------------------------------------------------------------------

esp_err_t LlmApi::stream_openai(
    const std::vector<Message>& messages,
    const std::string& system_prompt,
    const std::string& tool_defs,
    std::function<void(const std::string&)> token_cb,
    std::function<bool()> cancelled_cb,
    std::vector<ToolCall>& tool_calls)
{
    JsonDocument req;
    req["model"]  = model_;
    req["stream"] = true;

    JsonArray msgs = req["messages"].to<JsonArray>();

    if (!system_prompt.empty()) {
        JsonObject sys = msgs.add<JsonObject>();
        sys["role"]    = "system";
        sys["content"] = system_prompt;
    }

    for (const auto& m : messages) {
        JsonObject obj = msgs.add<JsonObject>();
        obj["role"]    = m.role;
        obj["content"] = m.content;
    }

    if (!tool_defs.empty()) {
        JsonDocument tools_doc;
        if (deserializeJson(tools_doc, tool_defs) == DeserializationError::Ok) {
            req["tools"] = tools_doc.as<JsonArray>();
        }
    }

    std::string body;
    serializeJson(req, body);

    StreamCtx ctx;
    ctx.token_cb     = &token_cb;
    ctx.cancelled_cb = &cancelled_cb;
    ctx.tool_calls   = &tool_calls;
    ctx.provider     = "openai";

    esp_http_client_config_t cfg = {};
    cfg.url               = "https://api.openai.com/v1/chat/completions";
    cfg.method            = HTTP_METHOD_POST;
    cfg.timeout_ms        = HTTP_NETWORK_TIMEOUT_MS;
    cfg.buffer_size       = 4096;
    cfg.buffer_size_tx    = 4096;
    cfg.event_handler     = http_event_handler;
    cfg.user_data         = &ctx;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.keep_alive_enable = true;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    std::string auth = "Bearer " + api_key_;
    esp_http_client_set_header(client, "Content-Type",  "application/json");
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_post_field(client, body.c_str(), body.size());

    esp_err_t ret = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ctx.cancelled) return ESP_ERR_NOT_FINISHED;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "OpenAI API returned HTTP %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}
