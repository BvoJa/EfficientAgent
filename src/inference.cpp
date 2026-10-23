#include "edge/inference.hpp"
#include "httplib.h"

namespace edge {
namespace {
using OrderedJson = nlohmann::ordered_json;
OrderedJson decoding_schema(const Json& input) {
    if (input.is_array()) {
        auto out = OrderedJson::array();
        for (const auto& item : input) out.push_back(decoding_schema(item));
        return out;
    }
    if (!input.is_object()) return OrderedJson::parse(input.dump());
    auto out = OrderedJson::object();
    for (const auto& [key,value] : input.items()) {
        if (key == "properties" && value.is_object()) {
            auto properties = OrderedJson::object();
            for (const auto& required : input.value("required",Json::array())) {
                auto name = required.get<std::string>();
                if (value.contains(name)) properties[name] = decoding_schema(value.at(name));
            }
            for (const std::string name : {"path","paths","filters","sort","limit","group_by","aggregates","recursive","extension"})
                if (value.contains(name) && !properties.contains(name)) properties[name] = decoding_schema(value.at(name));
            for (const auto& [name,property] : value.items())
                if (!properties.contains(name)) properties[name] = decoding_schema(property);
            out[key] = std::move(properties);
        } else out[key] = decoding_schema(value);
    }
    return out;
}
std::unique_ptr<httplib::Client> client(const InferenceConfig& config, Deadline deadline) {
    check_deadline(deadline);
    auto result = std::make_unique<httplib::Client>(config.host, config.port);
    auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now());
    result->set_connection_timeout(std::min(remaining, std::chrono::microseconds(2000000)));
    result->set_read_timeout(remaining);
    result->set_write_timeout(remaining);
    result->set_max_timeout(remaining);
    result->set_follow_location(false);
    return result;
}
Json response_json(const httplib::Result& response) {
    if (!response) throw Error("backend", "Local server request failed: " + httplib::to_string(response.error()));
    if (response->status != 200) throw Error("backend", "Local server returned HTTP " + std::to_string(response->status) + ": " + response->body.substr(0,512));
    try { return Json::parse(response->body); } catch (const Json::exception& e) { throw Error("backend_json",e.what()); }
}
}
LlamaServerBackend::LlamaServerBackend(InferenceConfig config) : config_(std::move(config)) {
    if (config_.host != "127.0.0.1" && config_.host != "localhost" && config_.host != "::1") throw Error("config", "Inference host must be loopback");
}
Json LlamaServerBackend::health(Deadline deadline) {
    auto http = client(config_, deadline);
    std::string body;
    auto response = http->Get("/health", [&](const char* ptr, size_t count) {
        if (body.size() + count > 65536 || Clock::now() >= deadline) return false;
        body.append(ptr,count); return true;
    });
    if (response) response->body = body;
    auto data = response_json(response);
    check_deadline(deadline);
    Json result = {{"available",data.value("status", "") == "ok"},{"backend","llama-server"},{"server",data}};
    // Optional server metadata. Older servers may not expose /props.
    body.clear();
    auto props = http->Get("/props", [&](const char* ptr, size_t count) {
        if (body.size() + count > 512 * 1024 || Clock::now() >= deadline) return false;
        body.append(ptr,count); return true;
    });
    if (props && props->status == 200) {
        try {
            auto p = Json::parse(body);
            result["build_info"] = p.value("build_info",Json());
            result["model_path"] = p.value("model_path",Json());
            result["model_alias"] = p.value("model_alias",Json());
            if (p.contains("model_alias") && p["model_alias"].is_string() && p["model_alias"] != config_.model)
                throw Error("server_config", "Running server alias must be Qwen3-0.6B");
            auto settings = p.value("default_generation_settings",Json::object());
            auto params = settings.value("params",settings);
            auto server_context = settings.value("n_ctx",params.value("n_ctx",Json()));
            if (server_context.is_number_integer()) {
                result["context_size"] = server_context;
                if (server_context.get<int>() != config_.context_size) throw Error("server_config", "Running server context_size differs from experiment configuration; restart with scripts/start_server.py");
            }
        } catch (const Json::exception&) { result["metadata_error"] = "Invalid /props metadata"; }
    }
    check_deadline(deadline);
    return result;
}
InferenceReply LlamaServerBackend::generate(const Json& messages, const Json& schema, Deadline deadline) {
    auto start = Clock::now();
    auto http = client(config_, deadline);
    OrderedJson body = {{"model",config_.model},{"messages",messages},{"stream",false}, {"max_tokens",config_.max_tokens},
        {"temperature",config_.temperature},{"seed",config_.seed}, {"chat_template_kwargs",{{"enable_thinking",config_.thinking}}},
        {"response_format",{{"type","json_schema"},{"json_schema",{{"name","agent_action"},{"strict",true},{"schema",decoding_schema(schema)}}}}}};
    std::string received;
    // The progress callback enforces the task deadline even while a peer sends small chunks.
    httplib::Request req;
    req.method = "POST"; req.path = "/v1/chat/completions";
    req.headers.emplace("Content-Type","application/json"); req.body = body.dump();
    req.content_receiver = [&](const char* ptr, size_t count, uint64_t, uint64_t) {
        if (received.size() + count > 1024 * 1024 || Clock::now() >= deadline) return false;
        received.append(ptr,count); return true;
    };
    req.progress = [&](uint64_t, uint64_t) { return Clock::now() < deadline; };
    auto response = http->send(req);
    check_deadline(deadline);
    if (response) response->body = received;
    const auto data = response_json(response);
    if (!data.contains("choices") || !data["choices"].is_array() || data["choices"].size() != 1) throw Error("backend", "Expected exactly one completion choice");
    const auto& choice = data["choices"][0];
    if (choice.value("finish_reason", "") == "length") throw Error("incomplete_output", "Generation exhausted max_tokens");
    const auto& message = choice.at("message");
    InferenceReply reply;
    if (message.contains("tool_calls") && message["tool_calls"].is_array() && !message["tool_calls"].empty()) {
        if (message["tool_calls"].size() != 1) throw Error("invalid_output", "Only one tool call per step is supported");
        const auto& fn = message["tool_calls"][0].at("function");
        Json args = fn.at("arguments").is_string() ? Json::parse(fn.at("arguments").get<std::string>()) : fn.at("arguments");
        reply.content = Json{{"type","tool"},{"name",fn.at("name")},{"arguments",args}}.dump();
    } else {
        if (!message.contains("content") || !message["content"].is_string()) throw Error("invalid_output", "Missing textual JSON action");
        reply.content = message["content"].get<std::string>();
    }
    reply.usage = data.value("usage", Json());
    reply.timings = data.value("timings", Json());
    reply.latency_ms = elapsed_ms(start);
    return reply;
}
InferenceReply ScriptedBackend::generate(const Json&, const Json&, Deadline deadline) {
    check_deadline(deadline);
    if (next_ >= responses_.size()) throw Error("script_exhausted", "Scripted response sequence exhausted");
    auto start = Clock::now();
    const auto& item = responses_[next_++];
    return {item.is_string() ? item.get<std::string>() : item.dump(), nullptr, nullptr, elapsed_ms(start)};
}
}
