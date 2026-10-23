#include "edge/config.hpp"
#include <iostream>
#include <sys/resource.h>

using namespace edge;
int main(int argc, char** argv) {
    try {
        std::map<std::string,std::string> options;
        bool health = false, server_args = false;
        for (int n = 1; n < argc; ++n) {
            std::string key = argv[n];
            if (key == "--help") {
                std::cout << "edge-agent --config FILE --task TEXT [--script FILE] [--workspace DIR] [--output NAME]\n"
                    "           --health | --server-args\nScript mode replays fixed responses and does not run an LLM. Output is structured JSON.\n";
                return 0;
            }
            if (key == "--health") { health = true; continue; }
            if (key == "--server-args") { server_args = true; continue; }
            if (key != "--config" && key != "--task" && key != "--script" && key != "--workspace" && key != "--output") throw Error("cli", "Unknown option: " + key);
            if (++n >= argc) throw Error("cli", "Missing value for " + key);
            if (!options.emplace(key,argv[n]).second) throw Error("cli", "Duplicate option: " + key);
        }
        auto config = load_config(options.contains("--config") ? options.at("--config") : "configs/agent.json");
        if (options.contains("--workspace")) config.workspace = options.at("--workspace");
        if (options.contains("--output")) config.output = options.at("--output");
        config.raw["workspace"] = config.workspace;
        config.raw["output"] = config.output;
        if (server_args) {
            Json args = {"llama-server","--model",config.model_path,"--alias","Qwen3-0.6B","--host",config.inference.host,
                "--port",std::to_string(config.inference.port),"--ctx-size",std::to_string(config.inference.context_size),
                "--threads",std::to_string(config.threads),"--batch-size",std::to_string(config.batch_size),
                "--ubatch-size",std::to_string(config.microbatch_size),"--n-gpu-layers","0","--parallel","1","--jinja",
                "--chat-template-kwargs","{\"enable_thinking\":false}"};
            std::cout << args.dump(2) << '\n'; return 0;
        }
        std::unique_ptr<InferenceBackend> backend;
        if (options.contains("--script")) {
            auto script = read_json(options.at("--script"));
            if (!script.is_array() || script.empty()) throw Error("script", "Expected a nonempty JSON response array");
            backend = std::make_unique<ScriptedBackend>(script.get<std::vector<Json>>());
        } else backend = std::make_unique<LlamaServerBackend>(config.inference);
        auto server = backend->health(Clock::now() + std::chrono::milliseconds(std::min(config.agent.timeout_ms,5000)));
        if (health) { std::cout << server.dump(2) << '\n'; return server.value("available",false) ? 0 : 1; }
        if (!server.value("available",false)) throw Error("backend", "Local server is not ready");
        if (!options.contains("--task")) throw Error("cli", "--task is required");
        Workspace workspace(config.workspace,config.output);
        ToolRegistry registry;
        FullContext context(config.agent.context_bytes);
        AllTools selection;
        Agent agent(*backend,registry,workspace,context,selection,config.agent);
        auto result = agent.run(options.at("--task"));
        result["experiment_name"] = config.id;
        result["metadata"] = runtime_metadata(config);
        result["metadata"]["server_version"] = server.value("build_info",Json());
        result["configuration"] = config.raw;
        result["server_health"] = server;
        struct rusage usage {};
        if (::getrusage(RUSAGE_SELF,&usage) == 0) {
            result["metrics"]["agent_peak_rss_kib"] = usage.ru_maxrss;
            result["metrics"]["agent_cpu_ms"] = 1000.0 * static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
                static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
        }
        for (const auto& event : result.at("events")) std::cerr << Json{{"event","state_transition"},{"detail",event}}.dump() << '\n';
        std::cout << result.dump(2) << '\n';
        return result.at("success").get<bool>() ? 0 : 1;
    } catch (const Error& e) {
        std::cout << error_json(e.code,e.what()).dump(2) << '\n'; return 2;
    } catch (const std::exception& e) {
        std::cout << error_json("fatal",e.what()).dump(2) << '\n'; return 2;
    }
}
