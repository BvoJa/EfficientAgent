#!/usr/bin/env python3
"""Separate model-dependent integration test; requires the pinned Qwen3 server."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from benchmark import ROOT, verify

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--binary", default="build/edge-agent")
parser.add_argument("--config", default="configs/agent.json")
args = parser.parse_args()
case = json.loads((ROOT / "benchmarks/cases/smoke.json").read_text())[0]
with tempfile.TemporaryDirectory(prefix="edge-live-") as temp:
    workspace = Path(temp)
    shutil.copytree(ROOT / "benchmarks/datasets", workspace / "benchmarks/datasets")
    process = subprocess.run([str(Path(args.binary).resolve()),"--config",str(Path(args.config).resolve()),"--workspace",str(workspace),"--task",case["task"]],capture_output=True,text=True,timeout=220)
    result = json.loads(process.stdout)
    passed, reason = verify(result,case["oracle"],workspace)
    print(json.dumps({"passed":passed,"verification":reason,"run":result},indent=2))
    raise SystemExit(0 if passed and process.returncode == 0 else 1)
