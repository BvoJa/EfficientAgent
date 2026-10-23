#!/usr/bin/env python3
"""Check vendored source hashes without downloading anything."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1] / "third_party"
for dependency, spec in json.loads((root / "dependencies.json").read_text()).items():
    for filename, expected in spec["files"].items():
        with (root / filename).open("rb") as file:
            actual = hashlib.file_digest(file,"sha256").hexdigest()
        if actual != expected:
            raise SystemExit("Hash mismatch: " + filename)
    print("PASS", dependency, spec["version"])
