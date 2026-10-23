#include "edge/tools.hpp"
#include "sqlite3.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace edge {
namespace {
constexpr size_t file_limit = 2 * 1024 * 1024;
constexpr size_t row_limit = 20000;
constexpr size_t observation_limit = 48 * 1024;
Json enumeration(std::initializer_list<const char*> values) {
    Json s = string_schema(); s["enum"] = Json::array();
    for (const char* x : values) s["enum"].push_back(x);
    return s;
}
std::optional<double> number(const std::string& text) {
    double value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || !std::isfinite(value)) return {};
    return value;
}
Json cell(const std::string& text) { auto n = number(text); return n ? Json(*n) : Json(text); }
struct Csv { std::vector<std::string> columns; std::vector<Json> rows; };
Csv csv(const std::string& path, const ToolContext& ctx) {
    if (fs::path(path).extension() != ".csv") throw Error("csv", "Expected .csv file");
    auto text = ctx.workspace.read(path, file_limit, ctx.deadline);
    if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
    if (text.find('\0') != std::string::npos) throw Error("csv", "NUL in CSV");
    std::vector<std::vector<std::string>> records;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false, closed = false, started = false;
    auto finish_field = [&] { row.push_back(field); field.clear(); closed = false; started = false; };
    auto finish_row = [&] {
        finish_field();
        if (row.size() > 128) throw Error("limit", "Too many CSV columns");
        records.push_back(std::move(row)); row.clear();
        if (records.size() > row_limit + 1) throw Error("limit", "Too many CSV rows");
    };
    for (size_t i = 0; i < text.size(); ++i) {
        if ((i % 4096) == 0) check_deadline(ctx.deadline);
        char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { field += '"'; ++i; }
                else { quoted = false; closed = true; }
            } else field += c;
        } else if (c == ',') finish_field();
        else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            finish_row();
        } else if (c == '"' && !started && field.empty() && !closed) { quoted = true; started = true; }
        else {
            if (closed || c == '"') throw Error("csv", "Malformed CSV quoting");
            field += c; started = true;
        }
        if (field.size() > 16384 || row.size() > 128) throw Error("limit", "CSV field or column limit exceeded");
    }
    if (quoted) throw Error("csv", "Unterminated quoted field");
    if (started || closed || !field.empty() || !row.empty()) finish_row();
    if (records.empty()) throw Error("csv", "CSV has no header");
    Csv result; result.columns = std::move(records[0]);
    std::set<std::string> seen;
    for (const auto& col : result.columns) {
        if (col.empty() || col.size() > 128 || col == "_source" || !seen.insert(col).second) throw Error("csv", "Invalid, reserved, or duplicate CSV header");
    }
    for (size_t i = 1; i < records.size(); ++i) {
        if (records[i].size() != result.columns.size()) throw Error("csv", "CSV row width differs from header");
        Json r = Json::object();
        for (size_t j = 0; j < result.columns.size(); ++j) r[result.columns[j]] = cell(records[i][j]);
        r["_source"] = path;
        result.rows.push_back(std::move(r));
    }
    return result;
}
void column(const Csv& table, const std::string& key) {
    if (key != "_source" && std::find(table.columns.begin(), table.columns.end(), key) == table.columns.end()) throw Error("csv", "Unknown column: " + key);
}
int compare(const Json& a, const Json& b) {
    // CSV columns determine numeric type. Accept finite numeric text on the filter RHS.
    if (a.is_number() && b.is_string()) {
        auto parsed = number(b.get<std::string>());
        if (parsed) return compare(a,Json(*parsed));
    }
    if (a.is_number() && b.is_number()) { const auto x = a.get<double>(), y = b.get<double>(); return x < y ? -1 : x > y ? 1 : 0; }
    if (a.is_string() && b.is_string()) { const auto& x = a.get_ref<const std::string&>(); const auto& y = b.get_ref<const std::string&>(); return x < y ? -1 : x > y ? 1 : 0; }
    throw Error("csv", "Comparison requires matching numeric/string types; numeric filters require a finite number");
}
Json inspect(const Json& args, const ToolContext& ctx) {
    auto table = csv(args.at("path"), ctx);
    Json stats = Json::object();
    for (const auto& col : table.columns) {
        double sum = 0, minimum = INFINITY, maximum = -INFINITY;
        size_t numeric = 0, empty = 0;
        for (const auto& row : table.rows) {
            check_deadline(ctx.deadline);
            const auto& v = row.at(col);
            if (v.is_number()) { double n = v.get<double>(); ++numeric; sum += n; minimum = std::min(minimum, n); maximum = std::max(maximum, n); }
            else if (v == "") ++empty;
        }
        if (!std::isfinite(sum)) throw Error("csv", "Numeric aggregate overflow");
        stats[col] = {{"numeric_count", numeric}, {"empty_count", empty}, {"min", numeric ? Json(minimum) : Json()},
                      {"max", numeric ? Json(maximum) : Json()}, {"mean", numeric ? Json(sum / static_cast<double>(numeric)) : Json()}};
    }
    return {{"path", args.at("path")}, {"columns", table.columns}, {"row_count", table.rows.size()}, {"statistics", stats}};
}
Json query(const Json& args, const ToolContext& ctx) {
    Csv table;
    for (const auto& path : args.at("paths")) {
        auto part = csv(path.get<std::string>(), ctx);
        if (table.columns.empty()) table.columns = part.columns;
        else if (table.columns != part.columns) throw Error("csv", "Merged CSV headers must match");
        if (table.rows.size() + part.rows.size() > row_limit) throw Error("limit", "Merged row limit exceeded");
        std::move(part.rows.begin(), part.rows.end(), std::back_inserter(table.rows));
    }
    for (const auto& f : args.value("filters", Json::array())) column(table, f.at("column"));
    std::vector<Json> selected;
    for (auto& row : table.rows) {
        check_deadline(ctx.deadline);
        bool keep = true;
        for (const auto& f : args.value("filters", Json::array())) {
            int c = compare(row.at(f.at("column").get<std::string>()), f.at("value"));
            auto op = f.at("op").get<std::string>();
            keep &= op == "eq" ? c == 0 : op == "ne" ? c != 0 : op == "lt" ? c < 0 : op == "lte" ? c <= 0 : op == "gt" ? c > 0 : c >= 0;
        }
        if (keep) selected.push_back(std::move(row));
    }
    const size_t matched = selected.size();
    if (args.contains("group_by") && !args.contains("aggregates")) throw Error("csv", "group_by requires aggregates");
    if (args.contains("aggregates")) {
        std::map<std::string, std::vector<const Json*>> groups;
        auto group_by = args.value("group_by", std::string());
        if (!group_by.empty()) column(table, group_by);
        for (const auto& a : args.at("aggregates")) {
            if (a.at("op") != "count" && !a.contains("column")) throw Error("csv", "Aggregate requires column");
            if (a.contains("column")) column(table, a.at("column"));
        }
        if (group_by.empty()) groups[""] = {};
        for (const auto& row : selected) {
            groups[group_by.empty() ? "" : row.at(group_by).dump()].push_back(&row);
            if (groups.size() > 200) throw Error("limit", "Too many aggregate groups");
        }
        std::vector<Json> aggregated;
        std::set<std::string> aliases;
        for (const auto& a : args.at("aggregates")) {
            auto alias = a.at("as").get<std::string>();
            if (alias.empty() || alias == group_by || !aliases.insert(alias).second) throw Error("csv", "Invalid or duplicate aggregate alias");
        }
        for (const auto& [key, rows] : groups) {
            Json row = Json::object();
            if (!group_by.empty()) row[group_by] = Json::parse(key);
            for (const auto& a : args.at("aggregates")) {
                auto op = a.at("op").get<std::string>(), alias = a.at("as").get<std::string>();
                if (op == "count") { row[alias] = rows.size(); continue; }
                double sum = 0, lo = INFINITY, hi = -INFINITY;
                for (const auto* source : rows) {
                    check_deadline(ctx.deadline);
                    const auto& v = source->at(a.at("column").get<std::string>());
                    if (!v.is_number()) throw Error("csv", "Aggregate encountered nonnumeric value");
                    double n = v.get<double>(); sum += n; lo = std::min(lo, n); hi = std::max(hi, n);
                }
                if (!std::isfinite(sum)) throw Error("csv", "Numeric aggregate overflow");
                row[alias] = rows.empty() ? Json() : Json(op == "sum" ? sum : op == "mean" ? sum / static_cast<double>(rows.size()) : op == "min" ? lo : hi);
            }
            aggregated.push_back(std::move(row));
        }
        selected = std::move(aggregated);
        table.columns.clear();
        if (!group_by.empty()) table.columns.push_back(group_by);
        for (const auto& a : args.at("aggregates")) table.columns.push_back(a.at("as"));
    }
    for (const auto& s : args.value("sort", Json::array())) column(table, s.at("column"));
    size_t comparisons = 0;
    std::stable_sort(selected.begin(), selected.end(), [&](const Json& a, const Json& b) {
        if (++comparisons % 1024 == 0) check_deadline(ctx.deadline);
        for (const auto& s : args.value("sort", Json::array())) {
            int c = compare(a.at(s.at("column").get<std::string>()), b.at(s.at("column").get<std::string>()));
            if (c != 0) return s.value("descending", false) ? c > 0 : c < 0;
        }
        return false;
    });
    size_t total = selected.size();
    if (selected.size() > args.value("limit", size_t(20))) selected.resize(args.value("limit", size_t(20)));
    return {{"matched_rows", matched}, {"total_result_rows", total}, {"returned_rows", selected.size()}, {"truncated", selected.size() < total}, {"rows", selected}};
}
// The authorizer runs during prepare; read-only status alone permits unsafe PRAGMAs.
int authorize(void*, int action, const char* p1, const char* p2, const char*, const char*) {
    switch (action) {
    case SQLITE_SELECT: case SQLITE_READ: case SQLITE_RECURSIVE: return SQLITE_OK;
    case SQLITE_FUNCTION: {
        static const std::set<std::string> allowed = {"count","sum","avg","min","max","total","round","abs","coalesce","ifnull","nullif","lower","upper","length","substr","substring","trim","ltrim","rtrim","typeof","like","glob"};
        std::string fn = p2 ? p2 : p1 ? p1 : "";
        std::transform(fn.begin(), fn.end(), fn.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return allowed.contains(fn) ? SQLITE_OK : SQLITE_DENY;
    }
    default: return SQLITE_DENY;
    }
}
struct SqlBudget { Deadline deadline; int callbacks = 10000; };
int progress(void* ptr) {
    auto& budget = *static_cast<SqlBudget*>(ptr);
    return Clock::now() >= budget.deadline || --budget.callbacks <= 0 ? 1 : 0;
}
Json sqlite_query(const Json& args, const ToolContext& ctx) {
    // Snapshot through the safe workspace FD. No SQLite path re-opening or -wal symlink escapes.
    auto data = ctx.workspace.read(args.at("path"), 16 * 1024 * 1024, ctx.deadline);
    if (data.size() < 100 || data.compare(0,16,std::string("SQLite format 3\0",16)) != 0)
        throw Error("sqlite", "Expected a valid standalone SQLite database");
    if (data[18] != 1 || data[19] != 1) throw Error("sqlite", "WAL databases require checkpointing and journal_mode=DELETE before querying");
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(":memory:", &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        if (raw) sqlite3_close(raw);
        throw Error("sqlite", "Cannot create database snapshot");
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    auto* buffer = static_cast<unsigned char*>(sqlite3_malloc64(data.size()));
    if (!buffer) throw Error("sqlite", "Cannot allocate bounded database snapshot");
    std::memcpy(buffer, data.data(), data.size());
    int rc = sqlite3_deserialize(db.get(), "main", buffer, static_cast<sqlite3_int64>(data.size()), static_cast<sqlite3_int64>(data.size()), SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_READONLY);
    if (rc != SQLITE_OK) { sqlite3_free(buffer); throw Error("sqlite", "Invalid SQLite snapshot"); }
    sqlite3_db_config(db.get(), SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    sqlite3_db_config(db.get(), SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
    if (sqlite3_exec(db.get(), "PRAGMA temp_store=MEMORY; PRAGMA query_only=ON;",nullptr,nullptr,nullptr) != SQLITE_OK)
        throw Error("sqlite", "Cannot configure read-only in-memory execution");
    sqlite3_limit(db.get(), SQLITE_LIMIT_LENGTH, 65536);
    sqlite3_limit(db.get(), SQLITE_LIMIT_SQL_LENGTH, 8192);
    sqlite3_limit(db.get(), SQLITE_LIMIT_COLUMN, 128);
    sqlite3_limit(db.get(), SQLITE_LIMIT_EXPR_DEPTH, 32);
    sqlite3_limit(db.get(), SQLITE_LIMIT_VDBE_OP, 20000);
    sqlite3_limit(db.get(), SQLITE_LIMIT_WORKER_THREADS, 0);
    sqlite3_set_authorizer(db.get(), authorize, nullptr);
    Deadline deadline = ctx.deadline;
    SqlBudget budget{deadline};
    sqlite3_progress_handler(db.get(), 1000, progress, &budget);
    std::string sql = args.at("sql");
    if (sql.find('\0') != std::string::npos) throw Error("sqlite", "NUL in SQL");
    sqlite3_stmt* stmt_raw = nullptr;
    const char* tail = nullptr;
    rc = sqlite3_prepare_v2(db.get(), sql.c_str(), static_cast<int>(sql.size()), &stmt_raw, &tail);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(stmt_raw, sqlite3_finalize);
    if (rc != SQLITE_OK || !stmt || !sqlite3_stmt_readonly(stmt.get())) throw Error("sqlite", "Only authorized read-only SELECT queries are allowed: " + std::string(sqlite3_errmsg(db.get())));
    for (; tail && *tail; ++tail) if (!std::isspace(static_cast<unsigned char>(*tail))) throw Error("sqlite", "Multiple statements or trailing SQL are prohibited");
    Json rows = Json::array(), columns = Json::array();
    std::set<std::string> seen;
    for (int c = 0; c < sqlite3_column_count(stmt.get()); ++c) {
        std::string key = sqlite3_column_name(stmt.get(), c);
        if (!seen.insert(key).second) throw Error("sqlite", "Duplicate result column; use aliases");
        columns.push_back(key);
    }
    bool truncated = false;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        check_deadline(deadline);
        if (rows.size() >= args.value("limit", size_t(100))) { truncated = true; break; }
        Json row = Json::object();
        for (int c = 0; c < sqlite3_column_count(stmt.get()); ++c) {
            Json v;
            switch (sqlite3_column_type(stmt.get(), c)) {
            case SQLITE_NULL: break;
            case SQLITE_INTEGER: v = sqlite3_column_int64(stmt.get(), c); break;
            case SQLITE_FLOAT: {
                double n = sqlite3_column_double(stmt.get(), c);
                if (!std::isfinite(n)) throw Error("sqlite", "Nonfinite SQL result");
                v = n; break;
            }
            case SQLITE_TEXT: v = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), c)), static_cast<size_t>(sqlite3_column_bytes(stmt.get(), c))); break;
            default: throw Error("sqlite", "BLOB results are not supported");
            }
            row[columns[static_cast<size_t>(c)].get<std::string>()] = v;
        }
        rows.push_back(row);
        if (rows.dump().size() > observation_limit) throw Error("limit", "SQLite output too large");
    }
    if (rc != SQLITE_DONE && !truncated) { check_deadline(deadline); throw Error("sqlite", sqlite3_errmsg(db.get())); }
    return {{"columns", columns}, {"rows", rows}, {"truncated", truncated}, {"snapshot", true}};
}
Json list_files(const Json& args, const ToolContext& ctx) {
    std::string path = args.value("path", ".");
    auto top = ctx.workspace.open(path, O_RDONLY | O_DIRECTORY);
    Json found = Json::array();
    size_t visited = 0;
    bool truncated = false;
    std::function<void(const std::string&, int)> walk = [&](const std::string& dirpath, int depth) {
        check_deadline(ctx.deadline);
        auto fd = ctx.workspace.open(dirpath, O_RDONLY | O_DIRECTORY);
        DIR* raw = ::fdopendir(::dup(fd.get()));
        if (!raw) throw Error("io", "Cannot enumerate directory");
        struct DirClose { void operator()(DIR* d) const { ::closedir(d); } };
        std::unique_ptr<DIR, DirClose> dir(raw);
        while (auto* entry = ::readdir(dir.get())) {
            check_deadline(ctx.deadline);
            std::string name(entry->d_name);
            if (name == "." || name == ".." || name.starts_with('.')) continue;
            if (++visited > 5000 || found.size() >= args.value("limit", size_t(100))) { truncated = true; return; }
            struct stat st {};
            if (::fstatat(fd.get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) throw Error("io", "Directory changed while listing");
            auto relative = (fs::path(dirpath) / name).lexically_normal().generic_string();
            if (S_ISREG(st.st_mode) && (!args.contains("extension") || fs::path(name).extension() == args.at("extension").get<std::string>())) found.push_back(relative);
            if (S_ISDIR(st.st_mode) && args.value("recursive", false)) {
                if (depth >= 8) { truncated = true; continue; }
                walk(relative, depth + 1);
                if (truncated && found.size() >= args.value("limit", size_t(100))) return;
            }
        }
    };
    walk(path, 0);
    std::sort(found.begin(), found.end());
    return {{"files", found}, {"truncated", truncated}};
}
Json report(const Json& args, const ToolContext& ctx) {
    std::string name = args.at("filename"), title = args.at("title");
    auto extension = fs::path(name).extension();
    if (extension != ".md" && extension != ".json") throw Error("report", "Report extension must be .md or .json");
    if (title.find_first_of("\r\n") != std::string::npos || title.size() > 200) throw Error("report", "Title must be one line up to 200 bytes");
    Json sources = Json::array();
    for (const auto& id : args.at("observation_ids")) {
        auto it = std::find_if(ctx.observations.begin(), ctx.observations.end(), [&](const Observation& o) { return o.id == id.get<std::string>(); });
        if (it == ctx.observations.end() || !it->result.value("ok", false) || it->tool == "write_report") throw Error("report", "Report sources must refer to successful data observations");
        sources.push_back({{"id", it->id}, {"tool", it->tool}, {"arguments", it->arguments}, {"data", it->result.at("data")}});
    }
    Json document = {{"title", title}, {"sources", sources}};
    std::string content;
    if (extension == ".json") content = document.dump(2) + "\n";
    else {
        content = "# " + title + "\n\nGenerated from deterministic tool observations.\n";
        for (const auto& source : sources) {
            std::string block = source.at("data").dump(2);
            size_t fence_size = 3, run = 0;
            for (char c : block) { run = c == '`' ? run + 1 : 0; fence_size = std::max(fence_size, run + 1); }
            std::string fence(fence_size, '`');
            content += "\n## " + source.at("id").get<std::string>() + " — " + source.at("tool").get<std::string>() + "\n\n" + fence + "json\n" + block + "\n" + fence + "\n";
        }
    }
    auto path = ctx.workspace.write_report(name, content, ctx.deadline);
    auto stored = ctx.workspace.read(path, 256 * 1024, ctx.deadline);
    if (stored != content || (extension == ".json" && Json::parse(stored) != document)) throw Error("verification", "Report readback failed");
    return {{"path", path}, {"bytes", content.size()}, {"verified", true}, {"observation_ids", args.at("observation_ids")}};
}
}
void ToolRegistry::add(Tool tool) {
    if (tool.name.empty() || tools_.contains(tool.name)) throw Error("registry", "Empty or duplicate tool name");
    auto name = tool.name; tools_.emplace(name, std::move(tool));
}
ToolRegistry::ToolRegistry() {
    const auto str = string_schema();
    add({"list_files", "List relative workspace files. Symlinks and hidden entries are excluded; check truncated.", object_schema({{"path", str}, {"extension", str}, {"recursive", {{"type","boolean"}}}, {"limit", integer_schema(1,200)}}, Json::array()), list_files});
    add({"read_file", "Read a bounded UTF-8 text, Markdown, or JSON file.", object_schema({{"path", str}}, {"path"}), [](const Json& a, const ToolContext& c) {
        auto ext = fs::path(a.at("path").get<std::string>()).extension();
        if (ext != ".txt" && ext != ".md" && ext != ".json") throw Error("file", "Only .txt, .md and .json are supported");
        auto text = c.workspace.read(a.at("path"), 24000, c.deadline);
        if (text.find('\0') != std::string::npos) throw Error("file", "Binary text is prohibited");
        Json data = {{"path", a.at("path")}, {"text", text}};
        if (ext == ".json") data["json"] = Json::parse(text);
        return data;
    }});
    add({"inspect_csv", "Get columns, row count and numeric min/max/mean for a CSV.", object_schema({{"path", str}}, {"path"}), inspect});
    auto filter = object_schema({{"column", str}, {"op", enumeration({"eq","ne","lt","lte","gt","gte"})}, {"value", {{"oneOf", Json::array({str, Json{{"type","number"}}})}}}}, {"column","op","value"});
    auto sort = object_schema({{"column", str}, {"descending", {{"type","boolean"}}}}, {"column"});
    auto aggregate = object_schema({{"column", str}, {"op", enumeration({"count","sum","mean","min","max"})}, {"as", str}}, {"op","as"});
    add({"query_csv", "Merge same-header CSVs; filter; optionally group/aggregate; sort by ordered keys; return top rows. Numeric operations run in C++.",
        object_schema({{"paths", array_schema(str,16,1)}, {"filters",array_schema(filter,16)}, {"sort",array_schema(sort,8)}, {"aggregates",array_schema(aggregate,16,1)}, {"group_by",str}, {"limit",integer_schema(1,200)}}, {"paths"}), query});
    add({"query_sqlite", "Run a single authorized read-only SELECT against a bounded standalone SQLite snapshot. No PRAGMA, ATTACH, extension, or writes.",
        object_schema({{"path",str}, {"sql",{{"type","string"},{"maxLength",8192}}}, {"limit",integer_schema(1,200)}}, {"path","sql"}), sqlite_query});
    add({"write_report", "Create a new .md or .json report in the output directory from successful observation IDs (e.g. obs-2). Content is rendered deterministically; existing files are never overwritten.",
        object_schema({{"filename",str}, {"title",str}, {"observation_ids",array_schema(str,16,1)}}, {"filename","title","observation_ids"}), report});
}
Json ToolRegistry::definitions() const {
    Json result = Json::array();
    for (const auto& [name, t] : tools_) result.push_back({{"name",name},{"description",t.description},{"parameters",t.schema}});
    return result;
}
Json ToolRegistry::action_schema() const {
    Json choices = Json::array();
    for (const auto& [name, t] : tools_) choices.push_back(object_schema({{"type",{{"type","string"},{"enum",{"tool"}}}}, {"name",{{"type","string"},{"enum",{name}}}}, {"arguments",t.schema}}, {"type","name","arguments"}));
    choices.push_back(object_schema({{"type",{{"type","string"},{"enum",{"final"}}}}, {"success",{{"type","boolean"}}}, {"answer",string_schema()}, {"evidence",array_schema(string_schema(),32)}}, {"type","success","answer","evidence"}));
    return {{"oneOf",choices}};
}
void ToolRegistry::validate_call(const std::string& name, const Json& args) const {
    auto it = tools_.find(name);
    if (it == tools_.end()) throw Error("unknown_tool", "Unknown tool: " + name);
    validate(args, it->second.schema);
}
Json ToolRegistry::execute(const std::string& name, const Json& args, const ToolContext& ctx) const {
    try {
        check_deadline(ctx.deadline);
        validate_call(name,args);
        Json data = tools_.at(name).run(args,ctx);
        check_deadline(ctx.deadline);
        if (data.dump().size() > observation_limit) throw Error("limit", "Tool result exceeds observation byte limit; narrow query");
        return {{"ok",true},{"data",data}};
    } catch (const Error& e) { return error_json(e.code,e.what()); }
      catch (const std::exception& e) { return error_json("tool_error",e.what()); }
}
}
