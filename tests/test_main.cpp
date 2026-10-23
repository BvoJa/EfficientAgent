#include "edge/config.hpp"
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace edge;
namespace {
int passed = 0, failed = 0;
void require(bool value, const std::string& label) {
    if (!value) { ++failed; std::cerr << "FAIL " << label << '\n'; }
    else { ++passed; std::cout << "PASS " << label << '\n'; }
}
void put(const fs::path& path, const std::string& text) { std::ofstream out(path); out << text; }
Json call(const std::string& name, Json args) { return {{"type","tool"},{"name",name},{"arguments",args}}; }
Json finish(Json evidence = {"obs-1"}) { return {{"type","final"},{"success",true},{"answer","Verified tool result"},{"evidence",evidence}}; }
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    char pattern[] = "/tmp/edge-tests-XXXXXX";
    auto* temp = ::mkdtemp(pattern);
    if (!temp) return 2;
    fs::path root(temp), repo(argv[1]);
    try {
        fs::create_directory(root / "data");
        fs::copy_file(repo / "benchmarks/datasets/models.csv",root / "data/models.csv");
        fs::copy_file(repo / "benchmarks/datasets/benchmark.sqlite",root / "data/benchmark.sqlite");
        put(root / "data/note.md","Fixture note");
        put(root / "data/a.json","{\"valid\":true}");
        put(root / "data/broken.json","{");
        put(root / "data/quoted.csv","name,score\r\n\"a,b\",2\r\n\"multi\nline\",3\r\n\"double\"\"quote\",4\r\n");
        put(root / "data/bad.csv","a,b\n1\n");
        put(root / "data/unclosed.csv","a\n\"x\n");
        put(root / "data/duplicate.csv","a,a\n1,2\n");
        put(root / "data/groups.csv","group,value\na,1\nb,2\na,5\n");
        put(root / "data/large.txt",std::string(25000,'x'));
        fs::create_symlink("/etc/passwd",root / "data/escape.txt");
        fs::create_directory_symlink("/etc",root / "outside");
        ::mkfifo((root / "data/pipe.txt").c_str(),0600);
        Workspace workspace(root,"outputs");
        ToolRegistry tools;
        std::vector<Observation> observations;
        auto execute = [&](const std::string& name, Json args) {
            return tools.execute(name,args,ToolContext{workspace,observations,Clock::now()+std::chrono::seconds(2)});
        };
        auto ok = [&](const std::string& name, Json args, const std::string& label) {
            auto result = execute(name,args); require(result.value("ok",false),label); return result;
        };
        auto reject = [&](const std::string& name, Json args, const std::string& label) { require(!execute(name,args).value("ok",true),label); };
        require(tools.definitions().size() == 6,"six registered tools");
        try { tools.add({"read_file","duplicate",Json::object(),{}}); require(false,"duplicate tool rejected"); } catch (const Error&) { require(true,"duplicate tool rejected"); }
        reject("shell",Json::object(),"unknown tool rejected");
        reject("read_file",Json::object(),"required arguments checked");
        reject("read_file",{{"path",7}},"argument types checked");
        reject("read_file",{{"path","data/note.md"},{"extra",true}},"unknown arguments rejected");
        reject("list_files",{{"limit",0}},"numeric schema range checked");
        auto listing = ok("list_files",{{"path","data"},{"recursive",true},{"extension",".csv"}},"list CSV files");
        require(listing["data"]["files"].size() == 6,"listing filters extension and excludes symlinks");
        auto limited = execute("list_files",{{"path","data"},{"limit",1}});
        require(limited["data"]["truncated"] == true,"listing exposes truncation");
        require(ok("read_file",{{"path","data/note.md"}},"read markdown")["data"]["text"] == "Fixture note","text roundtrip");
        require(ok("read_file",{{"path","data/a.json"}},"read JSON")["data"]["json"]["valid"] == true,"JSON parsed");
        reject("read_file",{{"path","data/broken.json"}},"invalid JSON file rejected");
        reject("read_file",{{"path","../etc/passwd"}},"parent traversal rejected");
        reject("read_file",{{"path","/etc/passwd"}},"absolute path rejected");
        reject("read_file",{{"path","data/escape.txt"}},"symlink file rejected");
        reject("read_file",{{"path","outside/passwd.txt"}},"symlink directory rejected");
        reject("read_file",{{"path",std::string("data/note.md\0suffix",19)}},"NUL path rejected");
        reject("read_file",{{"path","data/pipe.txt"}},"FIFO rejected without blocking");
        reject("read_file",{{"path","data/large.txt"}},"file byte limit enforced");
        reject("read_file",{{"path","data/models.csv"}},"text extension allowlist");
        auto stats = ok("inspect_csv",{{"path","data/models.csv"}},"inspect CSV");
        require(stats["data"]["row_count"] == 4 && stats["data"]["statistics"]["latency_ms"]["min"] == 22,"CSV stats correct");
        auto quoted = ok("query_csv",{{"paths",{"data/quoted.csv"}}},"quoted CSV parser");
        require(quoted["data"]["rows"][0]["name"] == "a,b" && quoted["data"]["rows"][1]["name"] == "multi\nline" && quoted["data"]["rows"][2]["name"] == "double\"quote","quoted commas, newlines and quotes");
        reject("inspect_csv",{{"path","data/bad.csv"}},"ragged CSV rejected");
        reject("inspect_csv",{{"path","data/unclosed.csv"}},"unclosed CSV quoting rejected");
        reject("inspect_csv",{{"path","data/duplicate.csv"}},"duplicate CSV headers rejected");
        Json best_args = {{"paths",{"data/models.csv"}},{"filters",Json::array({Json{{"column","latency_ms"},{"op","lt"},{"value",50}}})},
            {"sort",Json::array({Json{{"column","mAP"},{"descending",true}}})},{"limit",1}};
        auto best = ok("query_csv",best_args,"filter and numeric sort");
        require(best["data"]["rows"][0]["model"] == "tiny-b" && best["data"]["matched_rows"] == 3,"best model oracle");
        auto merged = execute("query_csv",{{"paths",{"data/models.csv","data/models.csv"}}});
        require(merged["data"]["matched_rows"] == 8,"CSV merge across files");
        auto aggregate = ok("query_csv",{{"paths",{"data/groups.csv"}},{"group_by","group"},{"aggregates",Json::array({Json{{"op","sum"},{"column","value"},{"as","total"}},Json{{"op","count"},{"as","n"}}})},{"sort",Json::array({Json{{"column","total"},{"descending",true}}})}},"grouped aggregation");
        require(aggregate["data"]["rows"][0]["total"] == 6 && aggregate["data"]["rows"][0]["n"] == 2,"aggregate oracle");
        auto empty_args = best_args; empty_args["filters"][0]["value"] = 0;
        auto empty = execute("query_csv",empty_args);
        require(empty["ok"] == true && empty["data"]["rows"].empty(),"zero-match query");
        reject("query_csv",{{"paths",{"data/models.csv"}},{"sort",Json::array({Json{{"column","missing"}}})}},"unknown CSV column rejected");
        reject("query_csv",{{"paths",{"data/models.csv"}},{"filters",Json::array({Json{{"column","latency_ms"},{"op","lt"},{"value","fifty"}}})}},"nonnumeric filter on numeric column rejected");
        auto numeric_text = best_args; numeric_text["filters"][0]["value"] = "50";
        require(execute("query_csv",numeric_text)["data"]["rows"] == best["data"]["rows"],"numeric text filters normalize deterministically");
        reject("query_csv",{{"paths",{"data/models.csv","data/groups.csv"}}},"incompatible CSV merge rejected");
        const std::string dbpath = "data/benchmark.sqlite";
        auto sql = ok("query_sqlite",{{"path",dbpath},{"sql","SELECT model, latency_ms FROM benchmarks ORDER BY latency_ms LIMIT 1"}},"read-only SQLite SELECT");
        require(sql["data"]["rows"][0]["model"] == "tiny-a","SQLite ordering oracle");
        ok("query_sqlite",{{"path",dbpath},{"sql","WITH x AS (SELECT model FROM benchmarks) SELECT count(*) AS n FROM x"}},"read-only CTE allowed");
        for (const auto& statement : {"DELETE FROM benchmarks", "DROP TABLE benchmarks", "PRAGMA user_version=1", "PRAGMA table_info(benchmarks)", "ATTACH DATABASE '/tmp/other.db' AS x", "SELECT load_extension('x')", "SELECT randomblob(1000000)", "SELECT * FROM benchmarks; SELECT 1"})
            reject("query_sqlite",{{"path",dbpath},{"sql",statement}},"SQL restriction: " + std::string(statement));
        reject("query_sqlite",{{"path",dbpath},{"sql","SELECT 1 AS a, 2 AS a"}},"duplicate SQL result columns rejected");
        reject("query_sqlite",{{"path",dbpath},{"sql","WITH RECURSIVE x(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM x) SELECT sum(n) FROM x"}},"recursive SQL instruction budget enforced");
        auto wal = workspace.read(dbpath,16384,Clock::now()+std::chrono::seconds(1));
        wal[18] = 2; wal[19] = 2; put(root / "data/wal.sqlite",wal);
        reject("query_sqlite",{{"path","data/wal.sqlite"},{"sql","SELECT 1"}},"WAL snapshot rejected");
        auto expired = tools.execute("query_sqlite",{{"path",dbpath},{"sql","SELECT 1"}},ToolContext{workspace,observations,Clock::now()-std::chrono::seconds(1)});
        require(expired["error"]["code"] == "timeout","tool deadline enforced");
        observations.push_back({"obs-1","query_csv",best_args,best,0});
        Json report_args = {{"filename","result.json"},{"title","Verified fixture"},{"observation_ids",{"obs-1"}}};
        auto report = ok("write_report",report_args,"JSON report written");
        require(report["data"]["verified"] == true && read_json(root / "outputs/result.json")["sources"][0]["data"]["rows"][0]["model"] == "tiny-b","report evidence readback");
        reject("write_report",report_args,"report overwrite rejected");
        report_args["filename"] = "result.md"; ok("write_report",report_args,"Markdown report written");
        report_args["filename"] = "../escape.json"; reject("write_report",report_args,"report traversal rejected");
        report_args["filename"] = "missing.json"; report_args["observation_ids"] = {"obs-missing"}; reject("write_report",report_args,"unknown report evidence rejected");
        fs::create_directory_symlink("/tmp",root / "linked-output");
        try { Workspace bad(root,"linked-output"); require(false,"symlink output directory rejected"); } catch (const Error&) { require(true,"symlink output directory rejected"); }
        AllTools selection;
        auto run = [&](std::vector<Json> responses, AgentConfig config = {}, const std::string& task = "Inspect fixture data") {
            ScriptedBackend backend(std::move(responses)); FullContext context(config.context_bytes);
            Agent agent(backend,tools,workspace,context,selection,config); return agent.run(task);
        };
        auto good_action = call("inspect_csv",{{"path","data/models.csv"}});
        auto result = run({good_action,finish()});
        require(result["success"] == true && result["state"] == "completed","agent multi-step workflow");
        require(result["events"][0]["state"] == "requesting" && result["events"][3]["state"] == "requesting","agent explicit state transitions");
        require(result["metrics"]["prompt_tokens"].is_null(),"mock tokens remain unmeasured");
        require(result["inference_responses"].size() == 2,"inference content retained for debugging");
        require(run({call("list_files",{{"path","data"}}),finish()})["success"] == true,"single-tool directory listing can finish");
        AgentConfig small_context; small_context.context_bytes = 64;
        require(run({good_action},small_context)["error"]["code"] == "context_limit","context overflow reported honestly");
        auto corrected = run({"not JSON",good_action,finish()});
        require(corrected["success"] == true && corrected["metrics"]["invalid_outputs"] == 1,"invalid JSON bounded recovery");
        auto invalid = run({"broken","broken","broken"});
        require(invalid["success"] == false && invalid["error"]["code"] == "invalid_output","invalid output retry exhaustion");
        auto repeated = run({good_action,good_action,good_action,good_action});
        require(repeated["success"] == false && repeated["metrics"]["tool_calls"] == 1,"repeated identical calls bounded");
        require(run({finish()})["success"] == false,"unsupported final answer rejected");
        require(run({call("read_file",{{"path","missing.md"}}),finish()})["success"] == false,"failed evidence cannot prove success");
        AgentConfig one; one.max_steps = 1;
        auto limited_run = run({good_action,finish()},one);
        require(limited_run["error"]["code"] == "step_limit","agent maximum steps");
        auto recovery = run({call("read_file",{{"path","missing.md"}}),good_action,finish({"obs-2"})});
        require(recovery["success"] == true && recovery["metrics"]["tool_errors"] == 1,"tool failure recovery with new evidence");
        auto contract = run({good_action,finish()}, {}, "Select the best row in data/models.csv and save a JSON report");
        require(contract["success"] == false,"inspection alone cannot satisfy query and report task");
        auto report_action = call("write_report",{{"filename","agent-contract.json"},{"title","Contract test"},{"observation_ids",{"obs-2"}}});
        auto complete_contract = run({good_action,finish(),call("query_csv",best_args),report_action,finish({"obs-1","obs-2","obs-3"})}, {}, "Select the best row in data/models.csv and save a JSON report");
        require(complete_contract["success"] == true && !complete_contract["diagnostics"].empty(),"premature completion receives bounded correction");
        auto no_filter = best_args; no_filter.erase("filters");
        auto filtered_report = call("write_report",{{"filename","filter-contract.json"},{"title","Filter contract"},{"observation_ids",{"obs-1"}}});
        auto filtered_contract = run({call("query_csv",no_filter),call("query_csv",best_args),filtered_report,finish({"obs-1","obs-2"})}, {},
            "Select the highest mAP below 50 ms in data/models.csv and save a JSON report");
        require(filtered_contract["success"] == true && filtered_contract["metrics"]["invalid_outputs"] == 1,"explicit threshold requires filters before execution");
        auto truncated_path = best_args; truncated_path["paths"] = {"models.csv"};
        auto grounded_path = run({call("query_csv",truncated_path),call("query_csv",best_args),finish()}, {}, "Find the highest mAP below 50 in data/models.csv");
        require(grounded_path["success"] == true && grounded_path["metrics"]["invalid_outputs"] == 1,"task file references constrain generated paths");
        auto cfg = load_config(repo / "configs/agent.json"); require(cfg.id == "baseline_q8" && cfg.agent.max_steps > 0,"experiment configuration loads");
        auto bad_config = cfg.raw; bad_config["agent"]["context_strategy"] = "compression"; put(root / "bad-config.json",bad_config.dump());
        try { load_config(root / "bad-config.json"); require(false,"unimplemented strategy rejected"); } catch (const Error&) { require(true,"unimplemented strategy rejected"); }
        bad_config = cfg.raw; bad_config["status"] = "planned"; put(root / "planned.json",bad_config.dump());
        try { load_config(root / "planned.json"); require(false,"planned experiments cannot execute"); } catch (const Error&) { require(true,"planned experiments cannot execute"); }
    } catch (const std::exception& e) { require(false,std::string("unexpected exception: ") + e.what()); }
    fs::remove_all(root);
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
