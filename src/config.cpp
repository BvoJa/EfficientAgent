#include "edge/config.hpp"
#include "sqlite3.h"
#include <fstream>
#include <sys/utsname.h>

namespace edge {
ExperimentConfig load_config(const fs::path& path) {
    ExperimentConfig c; c.raw = read_json(path);
    auto positive = integer_schema(1,1000000);
    auto model = object_schema({{"name",{{"type","string"},{"enum",{"Qwen3-0.6B"}}}}, {"path",string_schema()}, {"revision",string_schema()}, {"quantization",string_schema()}}, {"name","path","revision","quantization"});
    auto inference = object_schema({{"host",string_schema()},{"port",integer_schema(1,65535)}, {"max_tokens",integer_schema(16,8192)},
        {"temperature",{{"type","number"},{"minimum",0},{"maximum",2}}}, {"seed",integer_schema(0,2147483647)}, {"context_size",integer_schema(512,131072)},
        {"threads",integer_schema(1,1024)}, {"batch_size",integer_schema(1,8192)}, {"microbatch_size",integer_schema(1,8192)}},
        {"host","port","max_tokens","temperature","seed","context_size","threads","batch_size","microbatch_size"});
    auto agent = object_schema({{"max_steps",integer_schema(1,100)}, {"invalid_retries",integer_schema(0,10)}, {"timeout_ms",positive}, {"tool_timeout_ms",integer_schema(1,60000)},
        {"max_identical_actions",integer_schema(1,3)}, {"context_bytes",integer_schema(4096,1048576)},
        {"context_strategy",{{"type","string"},{"enum",{"full"}}}}, {"tool_selection",{{"type","string"},{"enum",{"all"}}}}, {"thinking",{{"type","boolean"},{"enum",{false}}}}},
        {"max_steps","invalid_retries","timeout_ms","tool_timeout_ms","max_identical_actions","context_bytes","context_strategy","tool_selection","thinking"});
    validate(c.raw, object_schema({{"experiment_name",string_schema()}, {"status",{{"type","string"},{"enum",{"implemented","planned"}}}},
        {"workspace",string_schema()},{"output",string_schema()},{"model",model},{"inference",inference},{"agent",agent},
        {"benchmark",object_schema({{"warmup",integer_schema(0,100)},{"repetitions",integer_schema(1,1000)}},{"warmup","repetitions"})}},
        {"experiment_name","status","workspace","output","model","inference","agent","benchmark"}));
    c.id = c.raw.at("experiment_name"); c.status = c.raw.at("status");
    if (c.id.empty() || c.id.size() > 80 || c.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
        throw Error("config", "Experiment identifier must use letters, digits, hyphens or underscores");
    if (c.status != "implemented") throw Error("unimplemented", "Planned configuration cannot be executed");
    c.workspace = c.raw.at("workspace"); c.output = c.raw.at("output");
    c.model_path = c.raw["model"]["path"]; c.model_revision = c.raw["model"]["revision"];
    const auto& i = c.raw["inference"];
    c.inference.host = i["host"]; c.inference.port = i["port"]; c.inference.max_tokens = i["max_tokens"];
    c.inference.temperature = i["temperature"]; c.inference.seed = i["seed"]; c.inference.context_size = i["context_size"];
    c.threads = i["threads"]; c.batch_size = i["batch_size"]; c.microbatch_size = i["microbatch_size"];
    if (c.microbatch_size > c.batch_size) throw Error("config", "microbatch_size must not exceed batch_size");
    const auto& a = c.raw["agent"];
    c.agent.max_steps = a["max_steps"]; c.agent.invalid_retries = a["invalid_retries"]; c.agent.timeout_ms = a["timeout_ms"];
    c.agent.tool_timeout_ms = a["tool_timeout_ms"]; c.agent.max_identical_actions = a["max_identical_actions"]; c.agent.context_bytes = a["context_bytes"];
    c.inference.thinking = a["thinking"];
    c.warmup = c.raw["benchmark"]["warmup"]; c.repetitions = c.raw["benchmark"]["repetitions"];
    return c;
}
Json runtime_metadata(const ExperimentConfig& config) {
    struct utsname system {}; ::uname(&system);
    Json result = {{"agent_version",EDGE_VERSION}, {"compiler",__VERSION__}, {"sqlite_version",sqlite3_libversion()},
        {"kernel",system.release},{"architecture",system.machine}, {"model_name","Qwen3-0.6B"}, {"model_revision",config.model_revision},
        {"model_path",config.model_path}, {"server_version",nullptr}, {"model_size_bytes",nullptr}};
    const fs::path path = config.model_path;
    if (fs::is_regular_file(path)) result["model_size_bytes"] = fs::file_size(path);
    return result;
}
}
