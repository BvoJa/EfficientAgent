#pragma once
#include "edge/common.hpp"
#include <functional>

namespace edge {
struct ToolContext {
    const Workspace& workspace;
    const std::vector<Observation>& observations;
    Deadline deadline;
};
struct Tool {
    std::string name;
    std::string description;
    Json schema;
    std::function<Json(const Json&, const ToolContext&)> run;
};
class ToolRegistry {
    std::map<std::string, Tool> tools_;
public:
    ToolRegistry();
    void add(Tool tool);
    Json definitions() const;
    Json action_schema() const;
    Json execute(const std::string& name, const Json& arguments, const ToolContext& context) const;
    void validate_call(const std::string& name, const Json& arguments) const;
};
class ToolSelectionStrategy {
public:
    virtual ~ToolSelectionStrategy() = default;
    virtual Json select(const ToolRegistry& registry, const std::string& task) const = 0;
};
class AllTools final : public ToolSelectionStrategy {
public:
    Json select(const ToolRegistry& registry, const std::string&) const override { return registry.definitions(); }
};
}
