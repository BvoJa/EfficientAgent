# Benchmark harness

`python3 scripts/benchmark.py --backend scripted` runs four frozen synthetic workflows in fresh temporary workspaces. `--backend llama-server` uses real inference from an already-running server. Configuration controls warmup rounds and measured repetitions; warmups are saved but excluded from summaries. Each task is a fresh agent process and context. A warmup here does not measure cold startup or guarantee a particular KV-cache state.

Cases in `benchmarks/cases/smoke.json` cover filtered model selection, accuracy/latency comparison, FPS ranking with size tie breaking, and read-only SQLite ranking. Each oracle requires successful completion, an exact ordered sequence of identifiers from a successful query, a verified write to the expected report filename, and matching evidence in the report. Prose is not judged. Fixture numbers are synthetic and are never reported as Qwen3 measurements.

Every execution creates a unique `<experiment>/<UTC timestamp>-<random suffix>/` directory with:

- `manifest.json`: backend identity, run status, configuration, binary/config/case/dataset/script hashes, Python/platform, repository revision when available, and installed-runtime manifest when available.
- `configuration.json`: the unchanged input configuration.
- Per-trial JSON results, stderr JSONL logs, and copied report artifacts.
- `trials.csv`: measured per-task success and available metrics.
- `summary.json`: verified success/failure/timeout rates and nearest-rank p50/p95 task latency.

All trials, including failures, stay visible. Configuration load errors and malformed child output fail verification. Missing observables remain null. CLI configuration overrides appear in each agent result. Configuration status `implemented` describes code support; execution status `running`/`executed` belongs to the run manifest. Interrupted runs retain `running` and never acquire a completed summary.

`--baseline previous/summary.json` emits descriptive success and p50 deltas only when backend, case hash, and dataset hashes match. A comparison still needs controlled hardware/software and sufficient repeats before interpretation. Scripted runtime latency is not comparable to real-model inference latency. The default four measured cases intentionally avoid an extensive campaign.

Agent metrics separate wall-clock inference, tool, and total duration; runtime overhead is their nonnegative difference. Input/output tokens and raw server timings are retained when available. Invalid-output/tool-error counts, request count, and tool-call count are measured. RSS and CPU describe only the agent process and include startup; they exclude llama-server. Prompt/generation tokens/s can be analyzed from server timings when present, but the harness does not invent them from HTTP wall time. TTFT, model load time, server peak memory/CPU, and labeled tool/argument accuracy need additional collectors. Scripted runs explicitly carry `model_inference=false` and null token usage.

Unit and local HTTP tests are model independent. `scripts/live_smoke.py` and the opt-in `model-dependent` CTest label separately test the real Qwen3 checkpoint. A failed live test remains a failure even if scripted cases pass. See `VALIDATION.md` for observed outcomes.
