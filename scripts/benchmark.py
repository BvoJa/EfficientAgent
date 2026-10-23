#!/usr/bin/env python3
"""Small correctness/measurement harness, not an optimization campaign."""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]

def digest(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()

def percentile(values, fraction):
    if not values:
        return None
    values = sorted(values)
    return values[max(0, math.ceil(fraction * len(values)) - 1)]

def verify(result, oracle, workspace):
    """Verify exact ordered tool rows and matching report evidence, not prose."""
    if not result.get("success"):
        return False, "agent did not finish successfully"
    matching = []
    for obs in result.get("observations", []):
        if obs["tool"] != oracle["tool"] or not obs["result"].get("ok"):
            continue
        rows = obs["result"]["data"].get("rows", [])
        if [row.get(oracle["field"]) for row in rows] == oracle["expected"]:
            matching.append(obs)
    if not matching:
        return False, "ordered query result differs from fixture oracle"
    report = workspace / "outputs" / oracle["report"]
    if not report.is_file():
        return False, "expected report is missing"
    writes = [o for o in result["observations"] if o["tool"] == "write_report" and o["result"].get("ok")
              and o["result"]["data"].get("path") == "outputs/" + oracle["report"]]
    if not any(set(w["arguments"]["observation_ids"]) & {o["id"] for o in matching} for w in writes):
        return False, "report does not cite the verified query"
    if report.suffix == ".json":
        sources = json.loads(report.read_text())["sources"]
        if not any(s["data"] == o["result"]["data"] and s["id"] == o["id"] for s in sources for o in matching):
            return False, "JSON report data does not match query"
    else:
        content = report.read_text()
        if not any(json.dumps(o["result"]["data"], indent=2, ensure_ascii=False, sort_keys=True) in content for o in matching):
            # C++ JSON formatting uses compact empty arrays; check the exact output data through parsing each fenced JSON block.
            import re
            blocks = []
            for block in re.findall(r"`{3,}json\n(.*?)\n`{3,}", content, re.S):
                try:
                    blocks.append(json.loads(block))
                except json.JSONDecodeError:
                    pass
            if not any(o["result"]["data"] in blocks for o in matching):
                return False, "Markdown report evidence differs from query"
    return True, "ordered query rows and report evidence verified"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/edge-agent")
    parser.add_argument("--config", default="configs/agent.json")
    parser.add_argument("--cases", default="benchmarks/cases/smoke.json")
    parser.add_argument("--backend", choices=["scripted", "llama-server"], default="scripted")
    parser.add_argument("--results", default="benchmarks/results")
    parser.add_argument("--baseline", help="Prior summary.json for a descriptive comparison")
    args = parser.parse_args()
    binary = Path(args.binary).resolve()
    config_path = Path(args.config).resolve()
    config = json.loads(config_path.read_text())
    if config["status"] != "implemented":
        parser.error("planned experiment configurations cannot execute")
    preflight = subprocess.run([str(binary),"--config",str(config_path),"--server-args"],capture_output=True,text=True)
    if preflight.returncode != 0:
        parser.error("invalid experiment configuration: " + preflight.stdout.strip())
    cases_path = Path(args.cases).resolve()
    cases = json.loads(cases_path.read_text())
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    destination = Path(args.results) / config["experiment_name"] / run_id
    destination.mkdir(parents=True, exist_ok=False)
    dataset_hashes = {p.name: digest(p) for p in sorted((ROOT / "benchmarks/datasets").iterdir()) if p.is_file()}
    manifest = {"run_id":run_id, "status":"running", "backend":args.backend, "model_inference":args.backend == "llama-server",
                "configuration":config, "config_sha256":digest(config_path), "binary_sha256":digest(binary),
                "cases_sha256":digest(cases_path), "datasets_sha256":dataset_hashes,
                "scripts_sha256":{c["id"]:digest(ROOT / c["script"]) for c in cases} if args.backend == "scripted" else None,
                "python":platform.python_version(), "platform":platform.platform()}
    revision = subprocess.run(["git","rev-parse","HEAD"],cwd=ROOT,capture_output=True,text=True)
    manifest["agent_git_revision"] = revision.stdout.strip() if revision.returncode == 0 else None
    runtime_file = ROOT / "models/runtime-manifest.json"
    manifest["installed_runtime"] = json.loads(runtime_file.read_text()) if runtime_file.is_file() else None
    (destination / "manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
    (destination / "configuration.json").write_text(json.dumps(config,indent=2)+"\n")
    trials = []
    for trial in range(-config["benchmark"]["warmup"], config["benchmark"]["repetitions"]):
        for case in cases:
            name = f"{case['id']}-{'warmup' if trial < 0 else 'trial'}-{abs(trial)}"
            with tempfile.TemporaryDirectory(prefix="edge-benchmark-") as temp:
                workspace = Path(temp)
                shutil.copytree(ROOT / "benchmarks/datasets",workspace / "benchmarks/datasets")
                cmd = [str(binary),"--config",str(config_path),"--workspace",str(workspace),"--output","outputs","--task",case["task"]]
                if args.backend == "scripted":
                    cmd += ["--script",str(ROOT / case["script"])]
                try:
                    process = subprocess.run(cmd,capture_output=True,text=True,timeout=config["agent"]["timeout_ms"]/1000+10)
                    (destination / f"{name}.stderr.jsonl").write_text(process.stderr)
                    try:
                        result = json.loads(process.stdout)
                    except json.JSONDecodeError:
                        result = {"success":False,"error":{"code":"invalid_process_output","message":process.stdout[:1000]}}
                    success, reason = verify(result,case["oracle"],workspace)
                    success = success and process.returncode == 0
                except subprocess.TimeoutExpired:
                    result = {"success":False,"state":"timed_out","error":{"code":"harness_timeout"}}
                    success, reason = False,"harness process timeout"
                artifact_dir = destination / (name + "-artifacts")
                if (workspace / "outputs").exists():
                    shutil.copytree(workspace / "outputs", artifact_dir)
            record = {"case_id":case["id"],"trial":trial,"warmup":trial < 0,"verified_success":success,"verification":reason,"run":result}
            (destination / f"{name}.json").write_text(json.dumps(record,indent=2)+"\n")
            trials.append(record)
            print(f"{'PASS' if success else 'FAIL'} {name}: {reason}",flush=True)
    measured = [r for r in trials if not r["warmup"]]
    latencies = [r["run"]["metrics"]["total_ms"] for r in measured if "metrics" in r["run"]]
    summary = {"run_id":run_id,"status":"executed","experiment_name":config["experiment_name"],"backend":args.backend,
               "trials":len(measured),"verified_successes":sum(r["verified_success"] for r in measured),
               "success_rate":sum(r["verified_success"] for r in measured)/len(measured),
               "p50_total_ms":percentile(latencies,0.5),"p95_total_ms":percentile(latencies,0.95),
               "timeout_rate":sum(r["run"].get("state") == "timed_out" for r in measured)/len(measured),
               "failure_rate":sum(not r["verified_success"] for r in measured)/len(measured),
               "cases_sha256":manifest["cases_sha256"],"datasets_sha256":dataset_hashes,
               "scripts_sha256":manifest["scripts_sha256"],
               "limitations":"Scripted runs measure deterministic runtime only. Agent RSS/CPU exclude llama-server. No TTFT, load-time, or server memory measurements."}
    if args.baseline:
        baseline = json.loads(Path(args.baseline).read_text())
        compatible = all(baseline.get(k) == summary.get(k) for k in ("backend","cases_sha256","datasets_sha256","scripts_sha256"))
        summary["baseline_comparison"] = {"compatible":compatible,"path":args.baseline}
        if compatible:
            summary["baseline_comparison"]["success_rate_delta"] = summary["success_rate"]-baseline["success_rate"]
            summary["baseline_comparison"]["p50_total_ms_delta"] = summary["p50_total_ms"]-baseline["p50_total_ms"] if summary["p50_total_ms"] is not None and baseline.get("p50_total_ms") is not None else None
    (destination / "summary.json").write_text(json.dumps(summary,indent=2)+"\n")
    with (destination / "trials.csv").open("w",newline="") as file:
        fields = ["case_id","trial","verified_success","total_ms","inference_ms","tool_ms","requests","tool_calls","invalid_outputs","prompt_tokens","completion_tokens","agent_peak_rss_kib"]
        writer = csv.DictWriter(file,fieldnames=fields); writer.writeheader()
        for r in measured:
            writer.writerow({**{k:r[k] for k in fields[:3]}, **{k:r["run"].get("metrics",{}).get(k) for k in fields[3:]}})
    manifest["status"] = "executed"
    (destination / "manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
    print(destination)
    return 0 if all(r["verified_success"] for r in measured) else 1

if __name__ == "__main__":
    raise SystemExit(main())
