#!/usr/bin/env python3
"""Translate validated agent configuration to llama-server startup arguments."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--binary",default="build/edge-agent")
parser.add_argument("--server",default="build-llama/bin/llama-server")
parser.add_argument("--config",default="configs/agent.json")
args = parser.parse_args()
os.chdir(ROOT)
reply = subprocess.run([args.binary,"--config",args.config,"--server-args"],capture_output=True,text=True,check=True)
command = json.loads(reply.stdout)
command[0] = args.server
print(json.dumps(command),flush=True)
os.execv(str(Path(args.server).resolve()),command)
