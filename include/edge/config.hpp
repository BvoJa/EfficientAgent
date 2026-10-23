#pragma once
#include "edge/agent.hpp"

namespace edge {
struct ExperimentConfig {
    Json raw;
    std::string id;
    std::string status;
    std::string model_path;
    std::string model_revision;
    int threads = 4, batch_size = 512, microbatch_size = 128;
    int warmup = 0, repetitions = 1;
    std::string workspace = ".", output = "outputs";
    InferenceConfig inference;
    AgentConfig agent;
};
ExperimentConfig load_config(const fs::path& path);
Json runtime_metadata(const ExperimentConfig& config);
}
