#pragma once
#include "json.hpp"
#include <chrono>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace edge {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
namespace fs = std::filesystem;
struct Error : std::runtime_error {
    std::string code;
    Error(std::string c, std::string message) : std::runtime_error(std::move(message)), code(std::move(c)) {}
};
void check_deadline(Deadline deadline);
double elapsed_ms(Clock::time_point start);
Json read_json(const fs::path& path);
void validate(const Json& value, const Json& schema, const std::string& path = "$");
Json object_schema(Json properties, Json required);
Json string_schema();
Json integer_schema(int minimum, int maximum);
Json array_schema(Json items, int maximum = 200, int minimum = 0);
Json error_json(const std::string& code, const std::string& message);
class Fd {
    int value_ = -1;
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd();
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;
    int get() const { return value_; }
};
class Workspace {
    fs::path root_;
    std::string output_;
    Fd root_fd_;
    Fd output_fd_;
public:
    Workspace(fs::path root, std::string output);
    const fs::path& root() const { return root_; }
    const std::string& output() const { return output_; }
    Fd open(const std::string& relative, int flags) const;
    std::string read(const std::string& path, size_t maximum, Deadline deadline) const;
    std::string write_report(const std::string& name, const std::string& content, Deadline deadline) const;
};
struct Observation {
    std::string id;
    std::string tool;
    Json arguments;
    Json result;
    double latency_ms = 0;
    Json json() const;
};
}
