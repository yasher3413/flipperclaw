#pragma once
#include <functional>
#include <string>
#include <vector>
#include "esp_err.h"
#include "esp_http_client.h"

/**
 * @file llm_api.h
 * @brief Streaming HTTPS client for Anthropic and OpenAI LLM APIs.
 *
 * Supports server-sent events (SSE) streaming. Tokens are delivered to the
 * caller via a token_cb as they arrive. Tool calls are collected and returned
 * after the stream ends.
 */

/// A single conversation message.
struct Message {
    std::string role;     ///< "user", "assistant", or "tool"
    std::string content;  ///< Raw text content
};

/// A tool call requested by the LLM.
struct ToolCall {
    std::string id;        ///< Tool call ID (Anthropic/OpenAI)
    std::string name;      ///< Tool name (e.g. "web_search")
    std::string arguments; ///< JSON-encoded argument object
};

/**
 * @brief Streaming LLM API client.
 *
 * Configured via NVS (provider, model, api_key). Call stream() to start a
 * streaming inference. Tokens arrive via token_cb; on completion, any tool
 * calls are available in the returned vector.
 */
class LlmApi {
public:
    LlmApi() = default;

    /**
     * @brief Load configuration from NVS (api_key, provider, model).
     *
     * Falls back to FC_SECRET_* defines if NVS keys are absent.
     * @return ESP_OK on success.
     */
    esp_err_t init();

    /**
     * @brief Perform a streaming LLM inference.
     *
     * Blocks until the stream completes or is cancelled. Tokens arrive via
     * token_cb synchronously from this call's context.
     *
     * @param messages     Conversation history (system prompt excluded here —
     *                     pass it as the first message with role "system", or
     *                     use the system_prompt parameter for Anthropic).
     * @param system_prompt System prompt string (used as Anthropic "system" field
     *                      or prepended as a system message for OpenAI).
     * @param tool_defs    JSON array string of tool definitions (may be empty).
     * @param token_cb     Called for each streamed text token.
     * @param cancelled_cb Returns true if the caller wants to abort streaming.
     * @param tool_calls   Output: any tool calls requested by the model.
     * @return ESP_OK, ESP_ERR_TIMEOUT, or ESP_FAIL on API error.
     */
    esp_err_t stream(
        const std::vector<Message>& messages,
        const std::string&          system_prompt,
        const std::string&          tool_defs,
        std::function<void(const std::string&)> token_cb,
        std::function<bool()>                   cancelled_cb,
        std::vector<ToolCall>&                  tool_calls
    );

    /**
     * @brief Update API key in NVS.
     */
    esp_err_t set_api_key(const std::string& key);

    /**
     * @brief Update model provider ("anthropic" or "openai") in NVS.
     */
    esp_err_t set_provider(const std::string& provider);

    /**
     * @brief Update model name in NVS.
     */
    esp_err_t set_model(const std::string& model);

    const std::string& provider() const { return provider_; }
    const std::string& model()    const { return model_; }

private:
    esp_err_t stream_anthropic(
        const std::vector<Message>& messages,
        const std::string& system_prompt,
        const std::string& tool_defs,
        std::function<void(const std::string&)> token_cb,
        std::function<bool()> cancelled_cb,
        std::vector<ToolCall>& tool_calls
    );

    esp_err_t stream_openai(
        const std::vector<Message>& messages,
        const std::string& system_prompt,
        const std::string& tool_defs,
        std::function<void(const std::string&)> token_cb,
        std::function<bool()> cancelled_cb,
        std::vector<ToolCall>& tool_calls
    );

    /// HTTP event handler trampoline (esp_http_client callback).
    static esp_err_t http_event_handler(esp_http_client_event_t* evt);

    std::string api_key_;
    std::string provider_{"anthropic"};
    std::string model_{"claude-haiku-4-5-20251001"};
};
