#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include "esp_err.h"
#include "llm_api.h"
#include "tools.h"
#include "memory_store.h"
#include "uart_proto.h"

/**
 * @file agent.h
 * @brief ReAct agent loop — manages conversation history, tool dispatch,
 *        streaming to Flipper, and SPIFFS persistence.
 */
class Agent {
public:
    Agent() = default;

    /**
     * @brief Inject dependencies.
     * @param uart    Pointer to the initialised UartProto instance.
     * @param llm     Pointer to the initialised LlmApi instance.
     * @param tools   Pointer to the initialised Tools instance.
     * @param memory  Pointer to the initialised MemoryStore instance.
     */
    void init(UartProto* uart, LlmApi* llm, Tools* tools, MemoryStore* memory);

    /**
     * @brief Run a full ReAct cycle for the given user prompt.
     *
     * Blocks until the response is complete (or cancelled). Sends CHUNK
     * messages over UART as tokens arrive and DONE when finished.
     * On error, sends ERROR:<code> over UART.
     *
     * @param user_prompt  Decoded user text (not Base64).
     */
    void run(const std::string& user_prompt);

    /**
     * @brief Signal the agent to cancel the in-progress inference.
     *
     * Thread-safe. Sets an atomic flag checked in the streaming callback.
     * The current run() call will return early and send ERROR:CANCELLED.
     */
    void cancel();

    /**
     * @brief Return true if an inference is currently in progress.
     */
    bool is_running() const { return running_.load(); }

private:
    /// Build the complete tool-definitions JSON for the current provider.
    std::string build_tool_defs() const;

    /// Append a message to history, rolling off oldest turns if at max.
    void push_history(const std::string& role, const std::string& content);

    /// Persist the current session to SPIFFS as JSONL.
    void persist_session();

    UartProto*   uart_{nullptr};
    LlmApi*      llm_{nullptr};
    Tools*       tools_{nullptr};
    MemoryStore* memory_{nullptr};

    std::vector<Message> history_;
    std::atomic<bool>    cancelled_{false};
    std::atomic<bool>    running_{false};
};
