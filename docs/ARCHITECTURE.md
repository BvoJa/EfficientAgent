# Architecture

The native C++ executable owns orchestration and all tool execution. A separate CPU llama-server process owns model loading, tokenization, chat templating, sampling, and constrained generation. Python helpers never participate in the core execution path.

`Agent` receives references to `InferenceBackend`, `ToolRegistry`, `Workspace`, `ContextStrategy`, and `ToolSelectionStrategy`. These are the concrete extension points; there is no plugin loader or service framework. Direct `libllama` integration can implement the backend interface without changing the controller. Sliding windows/compression can implement the context interface, and retrieval can implement tool selection. Only `FullContext` and `AllTools` are supplied today.

The registry owns six named handlers and their schemas. It builds the constrained action schema from those same schemas, validates calls independently, and catches tool failures into structured observations. The schema validator deliberately supports only the emitted subset: object/array/scalar types, required fields, additional properties, enums, oneOf, numeric bounds, string byte bounds, and array bounds. It is not a general JSON Schema implementation.

The workspace holds directory descriptors. Reads use `openat2(RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS)` and reject non-regular files. Directory listing opens each directory securely and never follows symlinks. Hidden entries are excluded. SQLite consumes a bounded byte snapshot from the same read path; SQLite never opens a model-chosen database filename, journal, or extension. Reports are written through a pinned output-directory descriptor and atomically published without replacement.

Report arguments cite prior successful observations. Their data and source call arguments are rendered directly into JSON/Markdown, with dynamic Markdown fences for embedded backticks. A report is read back and compared before it is marked verified. Files must use a plain `.md` or `.json` filename. Existing destinations, including symlinks, are rejected.

Configuration is strict and fails on unknown fields or unsupported methods. Inference, tool, and total latency use a monotonic clock. Each result includes the configuration and metadata; the harness adds hashes, fixture versions, process artifacts, and execution manifests. A run's existence never changes a planned method into an implemented one.

Threat model: the model and file contents are untrusted. The user's configured workspace and local server are authorized. This is not a container or OS isolation boundary against a hostile process concurrently replacing the workspace or a malicious SQLite library exploit. Deadlines and byte/row/instruction limits bound normal tool work; blocking kernel I/O is not forcibly interrupted.
