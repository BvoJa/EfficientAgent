#!/usr/bin/env python3
"""Create a synthetic, standalone SQLite test fixture using only Python stdlib."""
import csv
from pathlib import Path
import sqlite3

root = Path(__file__).resolve().parents[1]
destination = root / "benchmarks/datasets/benchmark.sqlite"
if destination.exists():
    destination.unlink()
with sqlite3.connect(destination) as db:
    db.execute("CREATE TABLE benchmarks(model TEXT PRIMARY KEY, mAP REAL, accuracy REAL, latency_ms REAL, fps REAL, size_mb REAL)")
    with (root / "benchmarks/datasets/models.csv").open(newline="") as source:
        rows = csv.DictReader(source)
        db.executemany("INSERT INTO benchmarks VALUES (?,?,?,?,?,?)", [(r["model"], *[float(r[k]) for k in ("mAP", "accuracy", "latency_ms", "fps", "size_mb")]) for r in rows])
print(destination)
