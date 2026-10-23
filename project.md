# EfficientEdgeAgent - Project Description

## 1. What this project is

EfficientEdgeAgent is a small, local AI agent that runs on a Linux x86-64 CPU.
It is written primarily in C++20 and uses the Qwen3-0.6B language model in Q8_0
quantized GGUF format through a separate `llama-server` process.

The project is a **Stage 1 baseline**: a working, reproducible agent runtime plus
a smoke-evaluation harness. It is not a general-purpose assistant and it is not
yet an optimization campaign. The current goal is to demonstrate a clear,
bounded, measurable agent design before trying to make it faster or smaller.

Key properties:

- Runs fully offline after installation.
- Uses no external AI API.
- The model resides in a local server listening on loopback.
- C++ owns task orchestration, validation, tool execution, and verification.
- Python is only used for setup, fixtures, benchmarks, and optional tests.
- The model and file contents are treated as untrusted input.
- Every run produces structured JSON evidence and metrics.

---

## 2. The problem it explores

A language model cannot by itself read files, query data, or write reports. It
can only generate text. An **agent** adds a loop around the model:

1. Give the model the task, tool descriptions, and previous observations.
2. Let the model choose one action, usually a tool call.
3. Validate the action.
4. Execute the tool in a restricted workspace.
5. Feed the observation back to the model.
6. Repeat until the model produces a final answer with evidence, or a budget is
   exhausted.

The project studies how to do this with a very small model on ordinary CPU
hardware, while keeping the runtime safe, deterministic, and measurable. The
"efficient" part refers to the small checkpoint, bounded context, fixed token
budgets, grammar-constrained output, and CPU-only execution. No performance gain
is claimed for optimization methods that have not actually been run.

---

## 3. High-level architecture

```text
Task
  |
  v
C++ edge-agent
  |  builds prompt + constrained JSON schema
  v
Inference backend  --HTTP-->  llama-server  -->  Qwen3-0.6B Q8_0
  ^                                                   |
  |                                                   v
  |                                            JSON action
  |                                                   |
  +---------------- full context <--------------------+
                          |
                          v
                 validate action
                          |
                          v
                 C++ tool registry
              (6 bounded read/write tools)
                          |
                          v
                 observation (obs-N)
                          |
                          v
                 evidence verification
                          |
                          v
                    JSON result
```

Two processes exist at runtime:

1. **`edge-agent`** - the native C++ executable. It owns the loop, the prompt,
   the schema, validation, tools, and verification.
2. **`llama-server`** - a separate llama.cpp process. It owns model loading,
   tokenization, chat templating, sampling, and grammar-constrained generation.

The split is deliberate. llama.cpp is good at model execution; the C++ agent is
responsible for safe orchestration and tool work. The agent talks to the server
over plain HTTP on `127.0.0.1` only.

Python never participates in the core execution path. It exists for developer
and evaluation workflows.

---

## 4. Main components

### 4.1 `edge-agent`

The command-line executable. It:

- Loads and strictly validates a JSON configuration.
- Selects a real or scripted inference backend.
- Checks server health before running.
- Opens the workspace and output directory.
- Creates the six-tool registry.
- Runs a bounded agent loop.
- Prints a structured JSON result to stdout and state events to stderr.
- Returns an exit code describing startup, task, or success status.

### 4.2 Inference backends

Two implementations of the same interface exist:

- **`LlamaServerBackend`** - the real backend. Sends `/v1/chat/completions`
  requests to llama-server with a JSON schema in `response_format`. Also checks
  `/health` and optionally reads `/props` for model/context metadata.
- **`ScriptedBackend`** - replays a fixed list of actions. It performs no model
  inference. It exists to test orchestration, validation, recovery, and tool
  behavior deterministically.

Scripted mode is explicitly not evidence of model reasoning or model quality.
Results from scripted runs carry `model_inference=false`.

### 4.3 Tools

All tool execution happens in C++. There is no shell tool, no dynamic code
execution, and no general HTTP tool.

| Tool | Purpose |
| --- | --- |
| `list_files` | Bounded listing of files relative to the workspace; optional recursion and extension filter. |
| `read_file` | Bounded reads of `.txt`, `.md`, and `.json` files; JSON files are parsed. |
| `inspect_csv` | Reports header, row count, and per-column numeric statistics. |
| `query_csv` | Merges compatible CSVs, filters rows, groups and aggregates, sorts, and returns top rows. |
| `query_sqlite` | Runs one authorized read-only SQL query over an in-memory snapshot of a standalone SQLite database. |
| `write_report` | Renders cited observations as a Markdown or JSON report in the output directory. |

Important limits include: 2 MiB CSV files, 20,000 merged rows, 128 columns,
16 KiB fields, 16 input paths, 200 returned rows, 48 KiB observations, 24,000-byte
text files, 16 MiB SQLite snapshots, and 256 KiB reports.

### 4.4 Workspace and filesystem safety

`Workspace` holds directory file descriptors rather than trusting path strings.
Reads use `openat2` with:

- `RESOLVE_BENEATH` - paths cannot escape the workspace root.
- `RESOLVE_NO_SYMLINKS` - symlinks are rejected.
- `RESOLVE_NO_MAGICLINKS` - magic links are rejected.

Only regular files are accepted. FIFOs, devices, and other special files are
rejected without blocking. Directory listings never follow symlinks, and hidden
entries are excluded.

Reports are written only with a plain `.md` or `.json` filename into the output
directory. Existing destinations and symlinks are rejected. Publication is
atomic and followed by a readback check before the report is marked verified.

### 4.5 SQLite safety

The agent does not let the model open arbitrary database paths. It reads a
bounded byte snapshot of a configured database file and opens that snapshot in
memory.

The query must be one read-only prepared statement. An allowlist authorizer
rejects writes, PRAGMAs, `ATTACH`, unknown functions, extensions, BLOBs,
duplicate output column names, and multiple statements. CTEs are allowed.
A progress handler enforces a deadline and a 10-million-VM-instruction budget.
WAL-mode databases are rejected because the snapshot would be inconsistent;
the database owner must checkpoint and convert to DELETE journal mode first.

### 4.6 Configuration

`configs/agent.json` describes the baseline. Configuration is strict: unknown
fields, unsupported methods, or inconsistent values are rejected instead of
silently ignored.

Main configuration groups:

- **Model** - name, path, revision, quantization.
- **Inference** - host, port, threads, context size, batch sizes, max tokens,
  temperature, seed.
- **Agent** - maximum steps, invalid retries, task timeout, tool timeout,
  identical-action limit, context byte budget, context strategy, tool selection,
  thinking mode.
- **Benchmark** - warmup count and measured repetitions.

The baseline configuration implements only:

- full context,
- all tools,
- non-thinking mode,
- fixed step/token/time budgets,
- bounded invalid-output correction.

Planned methods such as Q4/F16 quantization, context compression, filtered tool
selection, and adaptive reasoning are marked `planned` and are rejected until
implemented and validated. A run never changes a planned method into an
implemented one.

---

## 5. The agent loop in detail

The state machine is:

```text
idle -> requesting -> validating -> executing -> requesting -> ...
                                          |
                                          v
                                      verifying -> completed
```

Any uncaught failure ends in `failed`; a deadline ends in `timed_out`.

### 5.1 Initialization

The agent receives the task (1-8192 bytes) and builds the message history:

- A system prompt explaining the one-JSON-action protocol.
- The full tool definitions.
- The user task.
- Optional completion requirements derived from simple task cues.

The full-context strategy keeps the original task, tool descriptions, accepted
actions, observations, and corrective errors. It has a byte cap, not an exact
token budget; the server remains authoritative about token limits.

### 5.2 Request

Before each request, the agent narrows the allowed action schema based on state:

- Final action is unavailable until successful evidence exists.
- If the task asks for a report, `write_report` is forced after a successful
  data/query observation and before further actions.
- Evidence IDs are restricted to successful observations.
- File arguments are restricted to paths mentioned in the task or seen in a
  successful listing, by file type.
- Ranking cues require CSV sort keys and a limit.
- Filter cues require nonempty CSV filters.
- Report source IDs are restricted to successful non-report observations.

The schema is serialized in a deliberate order: action type and tool name before
arguments, required properties before optional ones. This matters because
llama.cpp's grammar follows schema property order, and a tiny model can otherwise
choose arguments before deciding which tool it is using.

### 5.3 Validate

The response must be valid JSON matching the schema. Tool names and arguments are
validated again independently by the registry. Repeating the same canonical
tool-name/arguments pair beyond the configured limit is rejected.

Invalid output does not immediately kill the run. It consumes a retry from a
run-wide correction budget, and the model receives a structured error asking for
a corrected action.

### 5.4 Execute

Tools run under the earlier of the task deadline and the tool deadline. A tool
error becomes a structured observation, not a crash, so the model can try a
different valid call.

### 5.5 Observe

Each successful or failed tool run gets a stable run-local ID such as `obs-1`.
The observation records the tool name, validated arguments, structured result,
and execution latency. Tool output is labeled as untrusted data, never as
instructions.

### 5.6 Final verification

A successful final action requires:

- A nonempty answer.
- At least one successful observation ID.
- A successful data-reading or data-querying observation.
- If the task cues require a query, a successful `query_csv`/`query_sqlite`
  observation cited in the evidence.
- If the task cues require a report, a verified `write_report` observation cited
  in the evidence.

This is an evidence/provenance check, not a semantic proof that the answer is
correct. The model could cite real observations and still misinterpret them.
The benchmark harness therefore has separate task-specific oracles.

Failures produce a nonzero exit code. The run never silently reports success.

---

## 6. Result format and metrics

Stdout is one JSON object. It contains:

- `success` - whether the agent completed verification.
- `state` - final state name.
- `backend` - `llama-server` or `scripted`.
- `task` - the input task.
- `final` - the accepted final action, if any.
- `error` - failure code and message, if any.
- `observations` - all tool observations.
- `events` - ordered state transitions with elapsed times.
- `inference_responses` - raw model responses for debugging.
- `diagnostics` - invalid-output and verification corrections.
- `metrics` - inference/tool/total latency, overhead, request and tool-call
  counts, invalid outputs, tool errors, token counts when available, and server
  timings.
- `experiment_name`, `metadata`, `configuration`, and `server_health` added by
  the entry point.
- Agent process peak RSS and CPU time when available.

Stderr is JSONL: one event object per line.

Exit codes:

- `0` - verified task completion.
- `1` - task failure or timeout.
- `2` - startup, configuration, or fatal error.

Metric boundaries matter:

- Agent RSS/CPU exclude llama-server.
- Input context has a byte cap, not a proven token cap.
- TTFT, model load time, server memory/CPU, tool-selection accuracy, and argument
  accuracy are not measured by default.
- Missing observables are `null`, not invented.

---

## 7. Runtime setup and startup

The C++ build itself downloads nothing. The optional runtime setup script:

1. Downloads a pinned llama.cpp source archive.
2. Verifies its SHA256 hash.
3. Builds only the CPU `llama-server` target.
4. Downloads the pinned official Qwen3-0.6B Q8_0 GGUF file.
5. Verifies the model SHA256 hash.

Pins live in `configs/runtime.lock.json`.

To start the server:

```sh
python3 scripts/start_server.py --config configs/agent.json
```

The launcher asks `edge-agent --server-args` for the exact argument array derived
from configuration, then replaces itself with `llama-server`. The direct native
equivalent is documented in `README.md`.

Server startup settings such as threads, context size, batch size, and microbatch
size are startup parameters. Changing them requires restarting the server. The
agent does not smuggle them into unsupported chat-completion fields.

The model is Qwen3-0.6B:

- 0.6 billion parameters.
- Q8_0 quantization: roughly 8 bits per weight, smaller and faster than F16 with
  some possible quality loss.
- GGUF container format used by llama.cpp.
- Qwen3 thinking mode is disabled for this baseline.
- GPU layers are forced to zero; execution is CPU-only.

---

## 8. Evaluation harness

`scripts/benchmark.py` runs frozen synthetic workflows in fresh temporary
workspaces. Two backends are available:

```sh
python3 scripts/benchmark.py --backend scripted
python3 scripts/benchmark.py --backend llama-server
```

Scripted runs test the runtime only. Real runs require an already-running server
and are intentionally opt-in.

`benchmarks/cases/smoke.json` currently defines four cases:

1. Filtered model selection.
2. Accuracy/latency comparison.
3. FPS ranking with size as a tie breaker.
4. Read-only SQLite ranking.

Each case has a deterministic oracle requiring:

- Successful agent completion.
- An exact ordered sequence of identifiers from a successful query.
- A verified report written under the expected filename.
- Report contents matching the evidence.

The harness does not judge prose. It judges observable data and files.

Every run gets a unique directory:

```text
benchmarks/results/<experiment>/<UTC timestamp>-<random suffix>/
```

That directory contains:

- `manifest.json` - backend, run status, configuration, source/config/case/
  dataset/script hashes, Python/platform details, repository revision, runtime
  manifest when available.
- `configuration.json` - unchanged input configuration.
- Per-trial JSON results and stderr JSONL logs.
- Copied report artifacts.
- `trials.csv` - measured per-task outcomes and available metrics.
- `summary.json` - verified success/failure/timeout rates and nearest-rank
  p50/p95 task latency.

Failures remain visible. Warmups are saved but excluded from summaries. A
baseline comparison only reports deltas when backend, case hashes, and dataset
hashes match. A handful of synthetic trials is correctness evidence, not a
performance conclusion.

---

## 9. Security model

The threat model is explicit:

- The model is untrusted.
- File contents are untrusted.
- The user's configured workspace and local server are authorized.
- This is not a container or OS isolation boundary against a hostile process
  concurrently modifying the workspace.
- Deadlines and byte/row/instruction limits bound normal tool work, but blocking
  kernel I/O is not forcibly interrupted.

Design choices that follow from this:

- No shell tool and no dynamic code execution.
- Paths resolved beneath the workspace with symlink rejection.
- Read-only SQL with an authorizer, progress budget, and bounded snapshot.
- Reports cannot overwrite existing files and are read back.
- Only loopback HTTP is allowed; redirects are disabled; response bytes and time
  are bounded.
- Tool results are labeled as data, not instructions.

These measures reduce accidental damage and common prompt-injection paths. They
do not make the agent a sandbox for hostile local processes.

---

## 10. Current scope and limitations

Implemented now:

- One local model family: Qwen3-0.6B Q8_0.
- Linux x86-64 CPU only.
- Full context with a byte cap.
- All six tools always offered.
- Non-thinking mode.
- Fixed maximum steps, timeouts, token limits, and correction budget.
- Bounded correction after invalid output or premature final action.
- Evidence verification and deterministic benchmark oracles.
- Scripted mode for runtime testing.

Not implemented or not measured:

- Q4/F16 quantization experiments.
- CPU thread/batch/mmap tuning experiments.
- Sliding-window, compressed, or selective context.
- Retrieval or rule-based tool selection.
- Thinking-mode experiments.
- Speculative decoding, KV quantization, prefix caching, concurrent batching,
  direct `libllama` integration, or asynchronous tools.
- ARM, Android, QNN/NPU, cross-compilation, or Docker support.
- TTFT, model load time, server memory/CPU, or labeled tool/argument accuracy.
- General natural-language task satisfaction proof.

The project deliberately refuses to report numbers for methods it has not run.
Planned configurations are rejected by the configuration validator.

---

## 11. Repository map

| Path | Contents |
| --- | --- |
| `CMakeLists.txt` | Linux/x86-64 check, C++20 setup, library/executable/test targets. |
| `include/edge/` | Public interfaces: agent, backend, tools, configuration, common types. |
| `src/` | Implementation: `main.cpp`, `agent.cpp`, `tools.cpp`, `inference.cpp`, `config.cpp`, `common.cpp`. |
| `tests/` | C++ unit tests and Python HTTP contract test. |
| `configs/` | Baseline agent config, runtime pins, experiment method catalog. |
| `examples/` | Scripted response files for deterministic runs. |
| `benchmarks/` | Synthetic datasets, cases, and generated results. |
| `scripts/` | Dependency check, fixture generation, runtime setup, server launcher, benchmark, live smoke test. |
| `docs/` | Architecture, runtime, inference, benchmark, validation, and experiment plan. |
| `third_party/` | Vendored pinned JSON, HTTP, and SQLite sources. llama.cpp is downloaded and ignored. |
| `models/` | Downloaded model weights; only `README.md` is tracked. |
| `outputs/` | Agent reports; ignored by Git. |
| `build/`, `build-llama/` | Generated build trees; ignored by Git. |
| `instruction.md` | How to build and run model-independent tests. |
| `project.md` | This project description. |

---

## 12. How the workflows fit together

Developer path:

1. Build and run tests. See `instruction.md`.
2. Optionally install the runtime: `python3 scripts/setup_runtime.py`.
3. Start the server: `python3 scripts/start_server.py`.
4. Run one task with `edge-agent`.
5. Run the benchmark harness for scripted or live evaluation.
6. Compare only compatible runs, then investigate failures.

Execution path:

1. `main.cpp` loads configuration.
2. Chooses scripted or HTTP backend.
3. Checks server health.
4. Builds workspace, tool registry, full context, and tool selection.
5. `Agent::run` loops: request, validate, execute, observe.
6. Final action is verified against successful evidence.
7. Result JSON is emitted; metrics and configuration are attached.

Evaluation path:

1. Benchmark harness selects frozen cases.
2. Each trial gets a fresh workspace and a fresh agent process.
3. The oracle checks exact query rows and report contents.
4. Raw results and artifacts are saved.
5. Summary reports verified success and latency percentiles.
6. Baseline comparison is allowed only for matching configurations and hashes.

---

## 13. Glossary

- **LLM** - large language model. Here, Qwen3-0.6B.
- **Inference** - running the model to generate output.
- **Token** - a model's unit of text; roughly a word fragment.
- **Context** - the conversation and data sent to the model for one generation.
- **GGUF** - file format used by llama.cpp for model weights and metadata.
- **Quantization** - storing weights at lower precision to reduce size and cost.
  Q8_0 uses roughly 8 bits per weight.
- **llama.cpp** - C/C++ inference engine used through its HTTP server.
- **llama-server** - the local HTTP process that runs the model.
- **Agent loop** - repeated model action -> tool execution -> observation cycle.
- **Tool call** - a structured action asking the runtime to run a tool.
- **Observation** - the structured result of a tool run, with an ID like `obs-1`.
- **Constrained generation** - forcing output to follow a JSON schema grammar.
- **Schema validation** - checking that an action has the right shape and types.
- **Evidence** - successful observation IDs cited by the final answer.
- **Verification** - checking that cited evidence exists and satisfies guards.
- **Oracle** - benchmark's independent expected result.
- **Scripted backend** - replay of fixed model actions; no actual inference.
- **Q8_0** - 8-bit quantization variant used by the baseline model.
- **Q4_K_M / F16** - other quantization levels; planned, not implemented.
- **Full context** - keep all prior messages until the byte budget is reached.
- **Tool selection** - choosing which tool definitions to expose to the model.
- **Openat2** - Linux syscall used to resolve paths safely under a directory.
- **WAL** - SQLite write-ahead log mode; rejected for snapshots.
- **DELETE journal mode** - SQLite rollback-journal mode required for snapshots.
- **Atomic publication** - making a completed file visible without partial
  content being observed.
- **RSS** - resident set size, the process's physical memory in use.
- **TTFT** - time to first token; not measured in this baseline.
- **p50 / p95** - median and 95th-percentile latency.
- **Deterministic oracle** - expected result defined by data, not by prose.
- **Stage 1** - the current runnable baseline and reproducible smoke evaluation.
