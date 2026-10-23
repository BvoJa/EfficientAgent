#pragma once
#include "edge/inference.hpp"
#include "edge/tools.hpp"

namespace edge {
enum class State { idle, requesting, validating, executing, verifying, completed, failed, timed_out };
std::string state_name(State state);
class ContextStrategy {
public:
    virtual ~ContextStrategy() = default;
    virtual void reset(const std::string& task, const Json& definitions) = 0;
    virtual void append(const std::string& role, const std::string& content) = 0;
    virtual Json messages() const = 0;
};
class FullContext final : public ContextStrategy {
    Json messages_ = Json::array();
    size_t maximum_bytes_;
public:
    explicit FullContext(size_t maximum_bytes = 24000) : maximum_bytes_(maximum_bytes) {}
    void reset(const std::string& task, const Json& definitions) override;
    void append(const std::string& role, const std::string& content) override;
    Json messages() const override { return messages_; }
};
struct AgentConfig {
    int max_steps = 10;
    int invalid_retries = 2;
    int timeout_ms = 180000;
    int tool_timeout_ms = 2000;
    int max_identical_actions = 1;
    size_t context_bytes = 24000;
};
struct Metrics {
    double inference_ms = 0, tool_ms = 0, total_ms = 0;
    size_t requests = 0, tool_calls = 0, invalid_outputs = 0, tool_errors = 0;
    size_t prompt_tokens = 0, completion_tokens = 0;
    bool usage_available = true;
    Json server_timings = Json::array();
    Json json() const;
};
class Agent {
    InferenceBackend& backend_;
    const ToolRegistry& tools_;
    const Workspace& workspace_;
    ContextStrategy& context_;
    const ToolSelectionStrategy& selection_;
    AgentConfig config_;
public:
    Agent(InferenceBackend& backend, const ToolRegistry& tools, const Workspace& workspace,
          ContextStrategy& context, const ToolSelectionStrategy& selection, AgentConfig config)
      : backend_(backend), tools_(tools), workspace_(workspace), context_(context), selection_(selection), config_(config) {}
    Json run(const std::string& task);
};
}
