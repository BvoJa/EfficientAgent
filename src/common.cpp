#include "edge/common.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <linux/openat2.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <atomic>

namespace edge {
void check_deadline(Deadline deadline) {
    if (Clock::now() >= deadline) throw Error("timeout", "Execution deadline exceeded");
}
double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
Json read_json(const fs::path& path) {
    std::ifstream in(path);
    if (!in) throw Error("config", "Cannot open JSON file: " + path.string());
    if (fs::file_size(path) > 4 * 1024 * 1024) throw Error("config", "JSON input too large");
    try { return Json::parse(in); } catch (const Json::exception& e) { throw Error("invalid_json", e.what()); }
}
Json error_json(const std::string& code, const std::string& message) {
    return {{"ok", false}, {"error", {{"code", code}, {"message", message}}}};
}
Json string_schema() { return {{"type", "string"}, {"maxLength", 65536}}; }
Json integer_schema(int lo, int hi) { return {{"type", "integer"}, {"minimum", lo}, {"maximum", hi}}; }
Json array_schema(Json items, int maximum, int minimum) {
    return {{"type", "array"}, {"items", std::move(items)}, {"maxItems", maximum}, {"minItems", minimum}};
}
Json object_schema(Json properties, Json required) {
    return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}};
}
void validate(const Json& v, const Json& s, const std::string& path) {
    auto bad = [&](const std::string& why) { throw Error("schema", path + ": " + why); };
    if (s.contains("oneOf")) {
        int matches = 0;
        for (const auto& branch : s.at("oneOf")) {
            try { validate(v, branch, path); ++matches; } catch (const Error&) {}
        }
        if (matches != 1) bad("must match exactly one action schema");
        return;
    }
    const auto type = s.value("type", "");
    if ((type == "object" && !v.is_object()) || (type == "array" && !v.is_array()) ||
        (type == "string" && !v.is_string()) || (type == "integer" && !v.is_number_integer()) ||
        (type == "number" && !v.is_number()) || (type == "boolean" && !v.is_boolean())) bad("expected " + type);
    if (s.contains("enum") && std::find(s["enum"].begin(), s["enum"].end(), v) == s["enum"].end()) bad("value not in enum");
    if (v.is_object()) {
        for (const auto& key : s.value("required", Json::array())) if (!v.contains(key.get<std::string>())) bad("missing " + key.get<std::string>());
        const auto props = s.value("properties", Json::object());
        for (const auto& [k, x] : v.items()) {
            if (props.contains(k)) validate(x, props.at(k), path + "." + k);
            else if (!s.value("additionalProperties", true)) bad("unknown property " + k);
        }
    }
    if (v.is_array()) {
        if (v.size() > s.value("maxItems", size_t(100000)) || v.size() < s.value("minItems", size_t(0))) bad("array size out of range");
        if (s.contains("items")) for (size_t i = 0; i < v.size(); ++i) validate(v[i], s["items"], path + "[" + std::to_string(i) + "]");
    }
    if (v.is_string() && v.get_ref<const std::string&>().size() > s.value("maxLength", size_t(65536))) bad("string too long");
    if (v.is_number()) {
        const double n = v.get<double>();
        if (!std::isfinite(n) || (s.contains("minimum") && n < s["minimum"].get<double>()) ||
            (s.contains("maximum") && n > s["maximum"].get<double>())) bad("number out of range");
    }
}
Fd::~Fd() { if (value_ >= 0) ::close(value_); }
Fd::Fd(Fd&& other) noexcept : value_(other.value_) { other.value_ = -1; }
Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) { if (value_ >= 0) ::close(value_); value_ = other.value_; other.value_ = -1; }
    return *this;
}
static void safe_relative(const std::string& value) {
    if (value.empty() || value.find('\0') != std::string::npos || fs::path(value).is_absolute()) throw Error("path", "Expected relative workspace path");
    for (const auto& part : fs::path(value)) if (part == "..") throw Error("path", "Parent traversal is prohibited");
}
Workspace::Workspace(fs::path root, std::string output) : root_(fs::canonical(root)), output_(std::move(output)),
    root_fd_(::open(root_.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)) {
    if (root_fd_.get() < 0) throw Error("path", "Cannot open workspace");
    safe_relative(output_);
    if (output_.find('/') != std::string::npos || output_ == ".") throw Error("path", "Output must be one directory name");
    if (::mkdirat(root_fd_.get(), output_.c_str(), 0700) < 0 && errno != EEXIST) throw Error("io", "Cannot create output directory");
    output_fd_ = open(output_, O_RDONLY | O_DIRECTORY);
}
Fd Workspace::open(const std::string& relative, int flags) const {
    safe_relative(relative);
    struct open_how how {};
    how.flags = static_cast<unsigned long long>(flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
    int fd = static_cast<int>(::syscall(SYS_openat2, root_fd_.get(), relative.c_str(), &how, sizeof(how)));
    if (fd < 0) throw Error("path", "Cannot securely open " + relative + ": " + std::strerror(errno));
    return Fd(fd);
}
std::string Workspace::read(const std::string& path, size_t maximum, Deadline deadline) const {
    auto fd = open(path, O_RDONLY);
    struct stat st {};
    if (::fstat(fd.get(), &st) < 0 || !S_ISREG(st.st_mode)) throw Error("path", "Only regular files are readable");
    if (st.st_size < 0 || static_cast<uint64_t>(st.st_size) > maximum) throw Error("limit", "File exceeds byte limit");
    std::string result;
    char buffer[8192];
    while (true) {
        check_deadline(deadline);
        auto n = ::read(fd.get(), buffer, sizeof(buffer));
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; throw Error("io", "File read failed"); }
        result.append(buffer, static_cast<size_t>(n));
        if (result.size() > maximum) throw Error("limit", "File grew beyond byte limit");
    }
    return result;
}
std::string Workspace::write_report(const std::string& name, const std::string& content, Deadline deadline) const {
    safe_relative(name);
    if (fs::path(name).filename() != name || name == "." || content.size() > 256 * 1024) throw Error("path", "Report requires a plain filename and bounded content");
    static std::atomic<unsigned> serial{0};
    std::string temp = ".edge-" + std::to_string(::getpid()) + "-" + std::to_string(serial++) + ".tmp";
    Fd fd(::openat(output_fd_.get(), temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (fd.get() < 0) throw Error("io", "Cannot create temporary report");
    try {
        size_t offset = 0;
        while (offset < content.size()) {
            check_deadline(deadline);
            auto n = ::write(fd.get(), content.data() + offset, content.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw Error("io", "Report write failed");
            offset += static_cast<size_t>(n);
        }
        if (::fsync(fd.get()) < 0) throw Error("io", "Report fsync failed");
        check_deadline(deadline);
        if (::syscall(SYS_renameat2, output_fd_.get(), temp.c_str(), output_fd_.get(), name.c_str(), RENAME_NOREPLACE) < 0)
            throw Error("io", "Report destination exists or rename failed");
    } catch (...) { ::unlinkat(output_fd_.get(), temp.c_str(), 0); throw; }
    return output_ + "/" + name;
}
Json Observation::json() const { return {{"id", id}, {"tool", tool}, {"arguments", arguments}, {"result", result}, {"latency_ms", latency_ms}}; }
}
