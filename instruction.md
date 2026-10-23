# Build and Test Instructions

This document explains **part 1 of the run workflow**: turning the source code
in this repository into a working executable, then running the tests that do
not need the language model.

You do not need the Qwen model, the llama.cpp server, or a downloaded model
file for this part.

---

## 1. What "building" means

This project is written in C++20. A C++ program cannot be run directly from its
source files. It must first be translated into machine code and linked into an
executable.

The process has three stages:

1. **Configure** - CMake reads `CMakeLists.txt`, checks the platform and
   compiler, and generates build files for the machine.
2. **Compile and link** - the compiler turns each `.cpp` file into an object
   file, and the linker combines those objects and libraries into the
   `edge-agent` executable.
3. **Test** - CTest runs the compiled test programs and reports pass/fail.

CMake is a build-system generator, not the compiler. It normally generates
Makefiles here, then `cmake --build` invokes `make` and the C++ compiler.

The build is **out of source**: all generated files go into `build/`, not into
`src/` or `include/`. Deleting `build/` is safe; the source tree is unchanged.

---

## 2. Requirements

Required for this repository:

- Linux on x86-64 (AMD64). The build rejects other operating systems and CPU
  architectures.
- Linux kernel 5.6 or newer at runtime, because the file tools use the
  `openat2` system call. Building does not require it, but running file tools
  does.
- A C++20 compiler: GCC 11 or newer, or a recent Clang.
- CMake 3.20 or newer.
- Make or Ninja.
- Git, if you are cloning or inspecting the repository.
- A filesystem that supports normal Unix permissions and temporary files.

Optional but recommended:

- Python 3.11 or newer. The agent itself does not use Python at runtime. Python
  runs the setup, fixture, benchmark, live-test, and dependency-check scripts.
  The HTTP contract test is only registered if CMake finds Python.

Not required for build and model-independent tests:

- Internet access.
- The Qwen model file.
- `llama-server`.
- A GPU or NPU.

---

## 3. Install the tools

Use the commands for your Linux distribution.

### Debian / Ubuntu

```sh
sudo apt update
sudo apt install build-essential cmake git ninja-build python3
```

`build-essential` includes GCC, `make`, and the standard C/C++ headers.

### Fedora

```sh
sudo dnf install gcc-c++ make cmake git ninja-build python3
```

### Arch Linux

```sh
sudo pacman -S base-devel cmake git ninja python
```

`ninja-build`/`ninja` is optional. Without it, CMake uses Make by default.

Check the versions:

```sh
gcc --version
cmake --version
python3 --version
```

`cmake --version` must report 3.20 or newer. This checkout may also contain a
local Python-packaged CMake under `.tools/cmake/`. That copy is not a standalone
binary on `PATH`; use the `PYTHONPATH` form shown in the troubleshooting section
if the system CMake is missing or too old.

---

## 4. Configure the build

Run from the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
```

Meaning of each part:

- `cmake` - run the configuration tool.
- `-S .` - source directory is the current directory.
- `-B build` - put all generated files in `build/`. Create it if missing.
- `-DCMAKE_BUILD_TYPE=Release` - build optimized code. This is what the project
  documents use. A `Debug` build is also possible and is easier to inspect in a
  debugger, but it is slower.
- `-DBUILD_TESTING=ON` - compile the unit tests and register them with CTest.
  This is a CMake/CTest standard option.

Configuration performs these checks:

- The platform must be Linux x86-64, otherwise CMake stops with
  `Only Linux x86-64 is supported`.
- A working C++20 compiler must be present.
- Python is searched for. If found, the HTTP contract test is added.

Configuration does **not** download anything. The JSON, HTTP, and SQLite
libraries are already committed under `third_party/`.

Expected result: CMake prints compiler and Python detection information and
finishes with `Configuring done`, `Generating done`, and a line telling you the
build files were written to `build/`.

---

## 5. Compile and link

```sh
cmake --build build -j 2
```

Meaning:

- `cmake --build build` - build the project already configured in `build/`.
  This works regardless of whether CMake generated Makefiles or Ninja files.
- `-j 2` - compile up to two files in parallel. The project documentation uses
  two jobs because this small machine-friendly baseline avoids high memory
  pressure. If you have plenty of RAM, `-j "$(nproc)"` is usually faster.

Targets produced:

- `build/edge-agent` - the main executable.
- `build/edge-tests` - the unit-test executable, when `BUILD_TESTING=ON`.
- `build/libedge_core.a` - a static library containing the agent, tools,
  inference client, configuration, and common code.
- `build/libedge_sqlite.a` - a static library built from the vendored SQLite
  source.

A **static library** is a collection of compiled object code linked into an
executable. It is not a separate process and does not need to be installed.

Expected result: the final line is similar to:

```text
[100%] Built target edge-agent
```

Warnings from `-Wall -Wextra -Wpedantic -Wconversion` are allowed. The project
does not use `-Werror`, so warnings do not stop the build. Compiler errors do.

### Incremental rebuilds

After editing a source file, run the same build command again. CMake and the
compiler rebuild only what changed.

After editing `CMakeLists.txt`, changing compiler options, or adding a new
source file, run the configure command again before building.

### Clean rebuild

If the build directory is corrupted or was created with incompatible options:

```sh
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j 2
```

`build/` is generated and Git-ignored. Removing it does not remove source code,
fixtures, or documentation.

---

## 6. Run the model-independent tests

```sh
ctest --test-dir build --output-on-failure -L model-independent
```

Meaning:

- `ctest` - CTest test runner.
- `--test-dir build` - run the tests registered in the `build/` directory.
- `--output-on-failure` - hide normal test output, but print full logs when a
  test fails.
- `-L model-independent` - run only tests carrying the `model-independent`
  label. These tests do not need Qwen or a running server.

Expected result:

```text
100% tests passed, 0 tests failed out of 2
```

The two tests are:

1. **`unit`** - runs `build/edge-tests`. It creates temporary fixtures under
   `/tmp`, then checks tool behavior, JSON schema validation, path security,
   CSV and SQLite limits, report writing and verification, agent state
   transitions, recovery, and scripted backend behavior. It reads the checked-in
   fixtures from `benchmarks/datasets/`, but does not modify them.
2. **`http-contract`** - runs `tests/http_contract.py`. It starts a fake local
   HTTP server on an ephemeral loopback port and checks the real HTTP backend
   against it: health checks, request shape, JSON actions, native tool calls,
   server errors, malformed output, timeouts, and wrong server context. This
   test requires Python and permission to bind a local socket.

The test executables enforce a 30-second timeout. They require no internet, no
model weights, and no external server.

### Useful CTest variations

List registered tests without running them:

```sh
ctest --test-dir build -N
```

Run one test verbosely:

```sh
ctest --test-dir build -R unit --output-on-failure -V
```

Run the tests directly:

```sh
./build/edge-tests "$PWD"
python3 tests/http_contract.py ./build/edge-agent "$PWD"
```

The unit test expects one argument: the repository root. The HTTP test expects
the agent binary path and the repository root.

### Optional live-model test

A separate test uses the real Qwen3 server. It is intentionally off by default.
Do not enable it for part 1 unless the model is installed and `llama-server` is
already running:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DEDGE_REAL_MODEL_TEST=ON
cmake --build build -j 2
ctest --test-dir build --output-on-failure -L model-dependent
```

This test is labeled `model-dependent`, so the `-L model-independent` command
above never runs it. A failed live test is a real failure; scripted tests
passing does not prove model quality.

---

## 7. Verify the vendored dependencies

```sh
python3 scripts/check_dependencies.py
```

This computes SHA256 hashes of the committed third-party source files and
compares them with `third_party/dependencies.json`. It does not download
anything. Expected output:

```text
PASS nlohmann_json 3.12.0
PASS cpp_httplib 0.20.0
PASS sqlite 3.50.2
```

This step is not required for the build, but it confirms that the vendored
libraries were not accidentally modified.

The pinned versions are:

- nlohmann/json 3.12.0 (MIT)
- cpp-httplib 0.20.0 (MIT)
- SQLite 3.50.2 (public domain)

The separate llama.cpp source and Qwen model are downloaded only by
`scripts/setup_runtime.py`, which verifies their hashes from
`configs/runtime.lock.json`. They are not part of the C++ build.

---

## 8. What part 1 proves and does not prove

After the build and model-independent tests pass, you know:

- The source compiles with the intended platform and language version.
- The six tools behave as expected on synthetic fixtures.
- Path traversal, symlinks, special files, oversized inputs, and unauthorized
  SQL are rejected.
- Reports are written, read back, and verified.
- The agent's bounded state machine, retries, deadlines, and scripted mode work.
- The HTTP client contract works against deterministic fake responses.

You do **not** know:

- Whether Qwen3-0.6B solves real tasks.
- Whether the model chooses the correct tool or arguments.
- Whether inference is fast, accurate, or memory-efficient.
- Whether benchmark success rates are good.

Those questions require part 2 (runtime setup and server), part 3 (running the
agent), and the evaluation harness.

---

## 9. Troubleshooting

### `CMake 3.20 or higher is required`

The system CMake is missing or too old. Install a newer CMake, or use the local
copy in this checkout. The local copy is a Python package, so its directory must
be added to `PYTHONPATH`:

```sh
export PYTHONPATH="$PWD/.tools/cmake"
./.tools/cmake/bin/cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
./.tools/cmake/bin/cmake --build build -j 2
./.tools/cmake/bin/ctest --test-dir build --output-on-failure -L model-independent
```

### `Only Linux x86-64 is supported`

The repository deliberately supports only Linux on x86-64. Cross-compilation,
ARM, Android, macOS, and Windows are outside current scope.

### `No CMAKE_CXX_COMPILER could be found`

No C++ compiler is installed, or it is not on `PATH`. Install GCC or Clang and
run configuration again. On Debian/Ubuntu, `build-essential` provides it.

### `Python3` is not found during configuration

The agent still builds. The `http-contract` test is simply not registered. Install
Python 3.11+ and reconfigure if you want it:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
```

### `http-contract` fails to bind a socket

The test needs loopback networking. Restricted containers or sandboxes may deny
socket creation or binding. Run on a normal host, or allow local sockets. The
unit test itself does not need networking.

### A test fails after changing fixture data

The checked-in SQLite fixture and CSV data are expected by the tests. Regenerate
the synthetic fixtures only when intentionally changing them:

```sh
python3 scripts/create_fixtures.py
```

Then rebuild and rerun the tests. Do not edit generated fixtures by hand.

### The compiler runs out of memory

Reduce parallelism:

```sh
cmake --build build -j 1
```

### `ctest` finds zero model-independent tests

Configuration was probably done without `-DBUILD_TESTING=ON`, or the build
directory belongs to another source tree. Reconfigure with testing enabled:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j 2
ctest --test-dir build -N
```

### The build fails after switching branches or compilers

Use a clean build:

```sh
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j 2
```

### Local HTTP build warnings about OpenSSL

TLS is not used. The agent connects only to a loopback llama-server over plain
HTTP. Missing OpenSSL does not prevent this project's build.

---

## 10. Short version

```sh
sudo apt install build-essential cmake git python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j 2
ctest --test-dir build --output-on-failure -L model-independent
python3 scripts/check_dependencies.py
```

Part 1 is complete when both model-independent tests pass and all dependency
hashes report `PASS`. No model or server is involved yet.
