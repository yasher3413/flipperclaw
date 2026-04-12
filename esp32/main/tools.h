#pragma once
#include <functional>
#include <map>
#include <string>
#include "esp_err.h"
#include "ArduinoJson.h"

/**
 * @file tools.h
 * @brief Tool registry and dispatcher for the FlipperClaw ReAct agent.
 *
 * Each tool has a name, description, JSON schema for parameters, and a handler
 * function. The registry builds the tool-definitions JSON blob sent to the LLM
 * and dispatches tool calls received in the model's response.
 */

/// Definition of a single tool.
struct ToolDef {
    std::string name;
    std::string description;
    std::string parameters_schema; ///< JSON Schema string for the parameters object
    std::function<std::string(JsonObjectConst)> handler;
};

/**
 * @brief Callback used by tools that need to exchange UART messages with the Flipper.
 *
 * send_fn(type, payload) — sends a message to the Flipper.
 * wait_fn(type, timeout_ms, out_payload) — blocks until a matching message arrives
 * or timeout elapses. Returns true on success.
 */
struct UartBridge {
    std::function<void(const std::string&, const std::string&)> send_fn;
    std::function<bool(const std::string&, uint32_t, std::string&)> wait_fn;
};

/**
 * @brief Tool registry and dispatcher.
 */
class Tools {
public:
    Tools() = default;

    /**
     * @brief Register all built-in tools and initialise HTTP clients.
     *
     * @param bridge  UART bridge callbacks for hardware tools.
     * @return ESP_OK on success.
     */
    esp_err_t init(UartBridge bridge);

    /**
     * @brief Dispatch a tool call by name.
     *
     * @param tool_name  Name of the tool to call.
     * @param args_json  JSON string containing the tool arguments.
     * @return String result to feed back to the LLM.
     */
    std::string dispatch(const std::string& tool_name, const std::string& args_json);

    /**
     * @brief Build the Anthropic tool_definitions JSON array string.
     */
    std::string build_anthropic_tools_json() const;

    /**
     * @brief Build the OpenAI function_definitions JSON array string.
     */
    std::string build_openai_tools_json() const;

private:
    void register_tool(ToolDef def);

    // Built-in tool implementations
    std::string tool_web_search(JsonObjectConst params);
    std::string tool_get_current_time(JsonObjectConst params);
    std::string tool_remember(JsonObjectConst params);
    std::string tool_flipper_nfc_read(JsonObjectConst params);
    std::string tool_flipper_subghz_replay(JsonObjectConst params);
    std::string tool_flipper_ir_send(JsonObjectConst params);

    std::map<std::string, ToolDef> registry_;
    UartBridge bridge_;
};
