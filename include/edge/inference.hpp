#pragma once
#include "edge/common.hpp"
#include <memory>

namespace edge {
struct InferenceReply {
    std::string content;
    Json usage = nullptr;
    Json timings = nullptr;
    double latency_ms = 0;
};
class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;
    virtual Json health(Deadline deadline) = 0;
    virtual InferenceReply generate(const Json& messages, const Json& schema, Deadline deadline) = 0;
    virtual std::string name() const = 0;
};
struct InferenceConfig {
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string model = "Qwen3-0.6B";
    int max_tokens = 512;
    double temperature = 0;
    int seed = 42;
    bool thinking = false;
    int context_size = 8192;
};
class LlamaServerBackend final : public InferenceBackend {
    InferenceConfig config_;
public:
    explicit LlamaServerBackend(InferenceConfig config);
    Json health(Deadline deadline) override;
    InferenceReply generate(const Json& messages, const Json& schema, Deadline deadline) override;
    std::string name() const override { return "llama-server"; }
};
// A fixed response script tests orchestration only; it performs no model inference.
class ScriptedBackend final : public InferenceBackend {
    std::vector<Json> responses_;
    size_t next_ = 0;
public:
    explicit ScriptedBackend(std::vector<Json> responses) : responses_(std::move(responses)) {}
    Json health(Deadline) override { return {{"available",true},{"backend","scripted"},{"model_inference",false}}; }
    InferenceReply generate(const Json&, const Json&, Deadline deadline) override;
    std::string name() const override { return "scripted"; }
};
}
