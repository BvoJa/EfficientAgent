# EfficientEdgeAgent

Efficient local agent powered by **Qwen3-0.6B Q8_0**, llama.cpp, and a native C++20 tool runtime. This portfolio project focuses on practical efficient AI, systems engineering, and agent orchestration. Current scope is Stage 1: a runnable baseline and reproducible smoke evaluation. Optimization campaigns are future work.

Supports **Linux x86-64 CPU only**, with Linux 5.6+ `openat2` support. Python 3.11+ is used for optional setup, fixture generation, and evaluation; the agent executable needs no Python. The project vendors pinned JSON, HTTP, and SQLite sources, so the C++ build performs no downloads. See [dependency pins](third_party/dependencies.json).

```text
Task → C++ Agent → Backend interface → local llama-server / Qwen3
          ↑                                  ↓
    Full context ← Observations ← Six C++ tools ← Validated JSON action
          └──────────────────────────────→ Evidence verification → Result
```

## Build and test

Install GCC 11+ or Clang with C++20 support, CMake 3.20+, Make or Ninja, and Git through your Linux package manager. Python is optional for core execution.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j 2
ctest --test-dir build --output-on-failure -L model-independent
python3 scripts/check_dependencies.py
```

The HTTP test binds an ephemeral loopback port. Restricted execution environments must permit local sockets. Unit tests require no network, server, or weights. The synthetic SQLite fixture is checked in; regenerate it with `python3 scripts/create_fixtures.py` when changing fixture data.

## Run immediately without a model

```sh
./build/edge-agent --config configs/agent.json \
  --script examples/select_best.script.json \
  --task 'Select the highest mAP below 50 ms and save a JSON report.'
python3 scripts/benchmark.py --backend scripted
```

The first command writes `outputs/best-model.json`. Reports never overwrite existing files: use a new filename, or run the harness, which creates an isolated workspace for every trial. Scripted mode replays predetermined actions; it verifies runtime behavior and **does not demonstrate language-model reasoning or model performance**.

## Install and run Qwen3

The setup helper downloads the exact official Qwen3-0.6B Q8_0 checkpoint and a pinned llama.cpp source archive, verifies SHA256 hashes, and builds only the CPU server target. It needs internet during installation, about 640 MB for the model, plus source/build storage and compiler memory. Review [runtime pins](configs/runtime.lock.json).

```sh
python3 scripts/setup_runtime.py --jobs 2
python3 scripts/start_server.py --config configs/agent.json
```

The server command occupies its terminal. In another terminal:

```sh
./build/edge-agent --config configs/agent.json --health
./build/edge-agent --config configs/agent.json \
  --task 'Inspect benchmarks/datasets/models.csv. Select the model with the highest mAP among rows with latency_ms below 50. Save a JSON report best-model-live.json using the query observation, then summarize the winner.'
python3 scripts/live_smoke.py
```

For a completely native startup/execution path after installation, start the server directly instead of using the Python launcher:

```sh
./build-llama/bin/llama-server --model models/Qwen3-0.6B-Q8_0.gguf \
  --alias Qwen3-0.6B --host 127.0.0.1 --port 8080 --ctx-size 8192 \
  --threads 4 --batch-size 512 --ubatch-size 128 --n-gpu-layers 0 \
  --parallel 1 --jinja --chat-template-kwargs '{"enable_thinking":false}'
```

To register the separate model test in CTest:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DEDGE_REAL_MODEL_TEST=ON
ctest --test-dir build --output-on-failure -L model-dependent
```

`./build/edge-agent --server-args` prints the exact argument array generated from configuration. Server startup uses the GGUF's embedded Qwen3 chat template (`--jinja`), disables thinking, uses one slot, and forces GPU layers to zero. Thread, context, batch, and microbatch settings are server startup parameters: restart the server when changing them. All execution is offline after installation. The C++ client only connects to a loopback host. No external AI API is used. See [inference details](docs/INFERENCE.md).

## Tools

| Tool | Behavior |
| --- | --- |
| `list_files` | Bounded relative file listing; optional recursion/extension filter |
| `read_file` | Bounded `.txt`, `.md`, `.json` reads; parses JSON |
| `inspect_csv` | Header, row count, numeric counts/min/max/mean |
| `query_csv` | Merge matching headers, filter, group, aggregate, multi-key sort, top rows |
| `query_sqlite` | Authorized single read-only query over an in-memory standalone DB snapshot |
| `write_report` | Render observation evidence as Markdown/JSON in the output directory |

Every tool has a description, JSON schema, argument validation, structured success/error result, and C++ handler. No shell tool exists. Tools reject traversal, absolute paths, symlinks, special files, excessive input, and unauthorized SQL. Reports use atomic exclusive publication and readback verification. Detailed schemas are in [tools.cpp](src/tools.cpp); examples are in [examples/](examples/).

CSV query example:

```json
{"type":"tool","name":"query_csv","arguments":{"paths":["benchmarks/datasets/models.csv"],"filters":[{"column":"latency_ms","op":"lt","value":50}],"sort":[{"column":"mAP","descending":true}],"limit":1}}
```

Results include observations (`obs-1`, etc.), state transitions, errors, configuration, inference/tool/total latency, token counts when provided, and process RSS/CPU. Stdout is the JSON result; stderr is JSONL execution logging. Exit codes: 0 verified runtime completion, 1 task failure/timeout, 2 startup/configuration error.

## Experiments and evaluation

```sh
# Four fixture cases, one measured trial each, no warmup by default.
python3 scripts/benchmark.py --backend scripted
# Requires a running Qwen3 server; intentionally opt-in.
python3 scripts/benchmark.py --backend llama-server
```

Each run gets a unique directory under `benchmarks/results/<experiment>/<run>/`, containing immutable configuration, environment/dependency fingerprints, raw traces, report artifacts, CSV trials, and JSON summaries. The harness verifies exact ordered query results and report evidence. Optional `--baseline path/to/summary.json` compares compatible fixture runs. See [benchmark methodology](docs/BENCHMARK.md).

Only full context, all tools, non-thinking mode, fixed budgets, and bounded correction are implemented. [Method catalog](configs/experiments/methods.json) and planned Q4/F16 configurations distinguish future work from runnable methods; planned configurations are rejected. Executed status is recorded only by actual run manifests. No benchmark values are supplied for unexecuted methods. See [experiment plan](docs/EXPERIMENT_PLAN.md).

## Structure and limits

`include/edge/` defines backend, context, selection, registry, configuration, and controller interfaces. `src/` implements the C++ runtime. `tests/` covers tools, validation, safety, state transitions, recovery, and HTTP contracts. `configs/`, `examples/`, `benchmarks/`, `scripts/`, and `docs/` contain reproducible inputs, workflows, evaluation, and design notes. Models, builds, reports, and benchmark outputs are Git-ignored.

This is a bounded single-task baseline, not a general-purpose assistant. Reports are deterministic evidence documents rather than free-form model-written analysis. Final-response verification checks successful evidence references, while benchmark-specific oracles check semantic fixture correctness; arbitrary natural-language prose is not proven correct. Full context has a byte cap, not an exact token budget; the server remains authoritative about token limits. SQLite requires a standalone database in DELETE journal mode and rejects WAL snapshots. Cooperative tool deadlines do not preempt stalled kernel filesystem I/O. Agent RSS/CPU exclude the server; TTFT, model load time, server memory/CPU, tool-selection accuracy, and argument accuracy remain unmeasured unless separately instrumented. No ARM, Android, QNN, NPU, Docker, or cross-compilation support is included.

Validation outcomes and current real-model limitations are recorded in [VALIDATION.md](docs/VALIDATION.md).
