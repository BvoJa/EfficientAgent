# Experiment plan — future work

Stage 1 provides the configuration loader, fixed fixtures, runtime instrumentation, repeated-trial harness, exports, and baseline comparison. It does not execute the following optimization campaign. Planned configurations are marked `planned` and cannot run until their dependencies and implementation are validated. See `configs/experiments/methods.json`.

| Group | Hypothesis and variables | Baseline and evidence needed |
| --- | --- | --- |
| A: Quantization | Q4_K_M may reduce model size/RSS at some loss of tool reliability; F16 only if feasible. Vary GGUF precision with the exact same checkpoint family. | Q8_0; model bytes, server peak RSS, prompt/generation tokens/s, task latency, verified success, invalid outputs |
| B: CPU | More threads/bigger batches may improve throughput until bandwidth or overhead dominates. Vary thread count, context, batch, microbatch, CPU settings, mmap, warm/cold state. | Fixed Q8_0 server flags; model load time, prompt/generation throughput, latency, CPU, RSS |
| C: Context | Less retained context may lower prompt cost while losing task evidence. Add sliding window, structured compression, selective retention, cache reuse individually. | Full context; input tokens, prompt latency, task latency, success, memory |
| D: Tool selection | Filtering descriptions may save tokens; retrieval overhead may dominate with six tools. Add rule/keyword filtering, embeddings only if justified. | All tools; correct tool/argument selection, input tokens, retrieval overhead, total latency, verified success |
| E: Reasoning | Smaller prompts and bounded reasoning may lower latency but affect recovery. Compare thinking/non-thinking, prompt detail, fixed/adaptive steps, single/multiple steps, correction budgets, verification ablation. | Non-thinking, full tool descriptions, fixed budget, bounded correction, evidence verification; success by difficulty, steps, tokens, latency |
| F: Advanced | Only address measured bottlenecks: speculative decoding with a compatible draft, KV quantization, prefix caching, concurrent batching, direct libllama, independent async tools. | Current HTTP backend; confirm hardware/runtime compatibility first, then measure task quality and latency/RSS |

Keep data, cases, seed, prompt, server version, checkpoint, CPU governor, affinity, and background load controlled except the variable under study. Record the effective server command, model SHA256, binary/source versions, and configuration per run. Separate cold model loading from warm inference, and agent memory from server memory. Include warmups and enough independent repetitions before reporting p50/p95 or confidence intervals. Four one-off smoke trials are correctness evidence, not a performance conclusion.

Expand to at least 100 frozen cases before full comparisons: single-tool, two-step, multi-step, numeric tool execution, recovery, long context, and invalid inputs. Label expected tool choices and arguments before claiming tool/argument accuracy. Maintain deterministic output oracles, independent of model prose. Analyze failures, not just successful-trial latency. Run ablations one at a time before combined strategies.

Stages after baseline: quantization → CPU settings → context/tool/reasoning optimization → compatible advanced inference → extensive comparisons and ablations → portfolio findings. ARM, Android, QNN/NPU, cross-compilation, and Docker deployment remain outside scope. No performance gains or numerical results are asserted for unexecuted methods.
