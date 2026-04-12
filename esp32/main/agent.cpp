/**
 * @file agent.cpp
 * @brief ReAct agent loop — streaming inference, tool dispatch, UART output.
 *
 * Flow per user prompt:
 *   1. Append user message to history.
 *   2. Load system prompt (SOUL.md) from SPIFFS.
 *   3. Call LlmApi::stream() — tokens forwarded as CHUNK over UART.
 *   4. If model returns tool calls, dispatch each via Tools::dispatch().
 *   5. Append tool results to history, loop up to MAX_TOOL_LOOPS.
 *   6. Send DONE when the loop ends naturally; send ERROR:CANCELLED if cancelled.
 */

#include "agent.h"
#include "constants.h"
#include <algorithm>
#include "esp_log.h"
#include "ArduinoJson.h"

static const char* TAG = "agent";

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void Agent::init(UartProto* uart, LlmApi* llm, Tools* tools, MemoryStore* memory) {
    uart_   = uart;
    llm_    = llm;
    tools_  = tools;
    memory_ = memory;
}

// ---------------------------------------------------------------------------
// cancel() — thread-safe signal
// ---------------------------------------------------------------------------

void Agent::cancel() {
    ESP_LOGI(TAG, "cancel() called");
    cancelled_.store(true);
}

// ---------------------------------------------------------------------------
// History management
// ---------------------------------------------------------------------------

void Agent::push_history(const std::string& role, const std::string& content) {
    history_.push_back({role, content});
    // Roll off oldest pairs (user+assistant) when over limit
    while (history_.size() > MAX_HISTORY_TURNS * 2) {
        history_.erase(history_.begin(), history_.begin() + 2);
    }
}

// ---------------------------------------------------------------------------
// Tool definitions JSON
// ---------------------------------------------------------------------------

std::string Agent::build_tool_defs() const {
    if (!tools_) return "";
    if (llm_->provider() == "openai") return tools_->build_openai_tools_json();
    return tools_->build_anthropic_tools_json();
}

// ---------------------------------------------------------------------------
// SPIFFS session persistence
// ---------------------------------------------------------------------------

void Agent::persist_session() {
    if (!memory_) return;
    // Serialise history as JSONL — one JSON object per line
    std::string jsonl;
    for (const auto& msg : history_) {
        JsonDocument doc;
        doc["role"]    = msg.role;
        doc["content"] = msg.content;
        std::string line;
        serializeJson(doc, line);
        jsonl += line + "\n";
    }
    memory_->write("session.jsonl", jsonl);
    ESP_LOGD(TAG, "Session persisted (%zu messages)", history_.size());
}

// ---------------------------------------------------------------------------
// run()
// ---------------------------------------------------------------------------

void Agent::run(const std::string& user_prompt) {
    if (!uart_ || !llm_) {
        ESP_LOGE(TAG, "run() called before init()");
        return;
    }

    running_.store(true);
    cancelled_.store(false);

    ESP_LOGI(TAG, "Agent run: prompt='%s'", user_prompt.substr(0, 60).c_str());

    // Append user message
    push_history("user", user_prompt);

    // Load system prompt
    std::string system_prompt;
    if (memory_) {
        memory_->read("SOUL.md", system_prompt);
    }
    if (system_prompt.empty()) {
        system_prompt =
            "You are FlipperClaw, a pocket AI assistant running on a Flipper Zero. "
            "Keep responses under 200 words.";
    }

    // Optional: append user profile to system prompt
    if (memory_) {
        std::string user_profile;
        if (memory_->read("USER.md", user_profile) == ESP_OK && !user_profile.empty()) {
            system_prompt += "\n\nUser profile:\n" + user_profile;
        }
    }

    // Optional: append long-term memory to system prompt
    if (memory_) {
        std::string mem;
        if (memory_->read("MEMORY.md", mem) == ESP_OK && !mem.empty()) {
            system_prompt += "\n\nLong-term memory:\n" + mem;
        }
    }

    // Optional: append today's daily note to system prompt
    if (memory_) {
        std::string today = memory_->today_filename();
        if (!today.empty()) {
            std::string daily;
            if (memory_->read(today, daily) == ESP_OK && !daily.empty()) {
                system_prompt += "\n\nToday's notes (" + today + "):\n" + daily;
            }
        }
    }

    std::string tool_defs = build_tool_defs();

    uart_->send("STATUS", "Thinking...");

    int tool_loops = 0;
    std::string full_response;

    while (tool_loops <= MAX_TOOL_LOOPS) {
        if (cancelled_.load()) break;

        full_response.clear();
        std::vector<ToolCall> tool_calls;

        // Streaming callback — forward each token to Flipper via UART
        auto token_cb = [this, &full_response](const std::string& token) {
            full_response += token;
            uart_->send("CHUNK", token);
        };

        // Cancellation check — polled from inside LlmApi on each SSE event
        auto cancelled_cb = [this]() -> bool {
            return cancelled_.load();
        };

        esp_err_t err = llm_->stream(
            history_, system_prompt, tool_defs,
            token_cb, cancelled_cb, tool_calls
        );

        if (cancelled_.load()) {
            ESP_LOGI(TAG, "Inference cancelled");
            uart_->send_raw("ERROR:CANCELLED\n");
            running_.store(false);
            return;
        }

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LLM stream error: %s", esp_err_to_name(err));
            const char* code = "API_ERR";
            if (err == ESP_ERR_TIMEOUT) code = "TIMEOUT";
            uart_->send_raw(std::string("ERROR:") + code + "\n");
            running_.store(false);
            return;
        }

        // Append assistant response to history
        if (!full_response.empty()) {
            push_history("assistant", full_response);
        }

        // If no tool calls, we're done
        if (tool_calls.empty()) break;
        if (tool_loops >= MAX_TOOL_LOOPS) {
            ESP_LOGW(TAG, "Max tool loops (%d) reached", MAX_TOOL_LOOPS);
            break;
        }

        // Dispatch tool calls and build tool-result messages
        for (const auto& tc : tool_calls) {
            if (cancelled_.load()) break;
            ESP_LOGI(TAG, "Tool call: %s args=%s", tc.name.c_str(), tc.arguments.substr(0, 80).c_str());
            uart_->send("STATUS", std::string("Using tool: ") + tc.name + "...");

            std::string result = tools_->dispatch(tc.name, tc.arguments);
            ESP_LOGI(TAG, "Tool result (%zu bytes)", result.size());

            // Append tool result to history
            // For Anthropic we encode as a tool_result message
            if (llm_->provider() == "anthropic") {
                JsonDocument doc;
                doc["type"]        = "tool_result";
                doc["tool_use_id"] = tc.id;
                doc["content"]     = result;
                std::string tool_msg;
                serializeJson(doc, tool_msg);
                push_history("user", tool_msg);
            } else {
                // OpenAI tool message
                JsonDocument doc;
                doc["role"]         = "tool";
                doc["tool_call_id"] = tc.id;
                doc["content"]      = result;
                std::string tool_msg;
                serializeJson(doc, tool_msg);
                push_history("tool", tool_msg);
            }
        }

        ++tool_loops;
    }

    // Persist conversation to SPIFFS
    persist_session();

    // Append a brief entry to today's daily note
    if (memory_) {
        std::string today = memory_->today_filename();
        if (!today.empty()) {
            std::string entry = "**Q:** " + user_prompt.substr(0, 120);
            if (user_prompt.size() > 120) entry += "...";
            if (!full_response.empty()) {
                entry += "\n**A:** " + full_response.substr(0, 200);
                if (full_response.size() > 200) entry += "...";
            }
            memory_->append(today, entry);
        }
    }

    uart_->send_raw("DONE\n");
    running_.store(false);
    ESP_LOGI(TAG, "Agent run complete (%d tool loops)", tool_loops);
}
