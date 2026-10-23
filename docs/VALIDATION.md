# Validation

Validated on 2026-10-20 with the implemented `baseline_q8` configuration,
Qwen3-0.6B Q8_0, and the pinned CPU-only llama.cpp server.

| Check | Result |
| --- | --- |
| Native C++ tool, safety, agent-state, and configuration tests | 82 passed, 0 failed |
| HTTP client contract test | passed |
| Dependency-pin check | passed |
| Scripted fixture harness | 4/4 verified |
| Local Qwen3 smoke workflow | passed |
| Local Qwen3 fixture harness | 4/4 verified |

The real-model harness output is preserved in
`benchmarks/results/baseline_q8/20261020T064354Z-d02a111b/`. It verified the
ordered query result and report evidence for the CSV selection, comparison,
top-three, and SQLite fixtures. The one-trial result reported 100% verified
fixture success, no timeouts, a p50 total agent latency of 27.07 seconds, and a
p95 of 42.25 seconds. Those timing values include end-to-end local inference
and tool execution; they are not an optimization claim or a throughput study.

The initial live suite, retained separately in
`benchmarks/results/baseline_q8/20261019T175255Z-5212393e/`, exposed path and
evidence-grounding failures. The current run follows fixes that constrain task
file paths, ranking/filter requirements, report provenance, and final evidence
to successful observations. It is a fixed synthetic-fixture regression check,
not evidence of general-purpose task reliability.

Agent process RSS and CPU exclude llama-server. TTFT, model load time, server
memory/CPU, tool-selection accuracy, argument accuracy, and statistical
repeatability have not been measured.
