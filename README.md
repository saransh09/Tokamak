# Tokamak

> A high-performance, SLO-aware LLM inference server written in modern C++.

Tokamak is a systems-first personal project for serving generative language
models under concurrent load. It combines asynchronous networking, continuous
batching, deadline-aware scheduling, KV-cache management, speculative
decoding, observability, and reproducible performance evaluation behind an
OpenAI-compatible API.

The project's central claim is not merely that it can generate text. Tokamak
aims to demonstrate that a C++ runtime can **adapt its serving policy to the
workload**, increasing useful throughput while respecting a user-defined
latency objective.

Full design rationale, subsystem specs, and the milestone plan live in
[`project.md`](project.md). Individual design decisions are recorded as ADRs
in [`docs/adr/`](docs/adr/); the reasoning and mistakes behind them are
written up in [`learning/`](learning/).

## Status

**Milestone 1 (Deterministic Runtime Skeleton) — complete.** Request state
machine, mock backend, virtual clock, FIFO scheduler, in-process submission
API, deterministic simulation runner, and structured event trace are built,
tested, and passing. No real model or network is required to exercise any
of this.

**Milestone 2 (Streaming Model Server) — in progress.** Building an async
HTTP frontend (Boost.Asio + Beast, C++20 coroutines) that streams tokens via
SSE, backed initially by the same deterministic mock backend and eventually
by a real `llama.cpp` backend. See
[`docs/adr/011-http-frontend-architecture.md`](docs/adr/011-http-frontend-architecture.md)
for the architecture and [`rough/milestone2-plan.md`](rough/milestone2-plan.md)
for the implementation sequencing.

See [project.md Section 19](project.md#19-milestones) for the full
milestone list (Benchmark Laboratory, Continuous Batching and Scheduling
Policies, Cache Policy, Speculative Decoding, Adaptive Controller, and
beyond).

## Building

Requires a C++23 compiler, CMake ≥ 3.21, and Ninja. Dependencies (Catch2,
nlohmann_json, Boost header-only subset) are fetched automatically via
CMake's `FetchContent`.

```bash
cmake --preset dev
cmake --build build/dev
ctest --preset dev
```

Other presets: `sanitizer` (ASan/UBSan), `release`. See
[`CMakePresets.json`](CMakePresets.json).

## Running the Milestone 1 simulation

```bash
./build/dev/apps/tokamak/tokamak <workload.jsonl> [--trace-out <path>]
```

Replays a JSONL workload (one request per line: `id`, `arrival_ms`,
`prompt_token_ids`, `max_tokens`, `deadline_ms`) through the FIFO scheduler
against the deterministic mock backend, using a virtual clock — no wall-clock
time or real model involved. Prints per-request state-transition timings,
batch composition per scheduler tick, TTFT/E2E latency distributions,
throughput/goodput, and an invariant-check result. Example fixtures are in
[`tests/golden/fixtures/`](tests/golden/fixtures/).

## Repository layout

```text
tokamak/
├── apps/tokamak/       # Simulation runner (Milestone 1); HTTP server entrypoint (Milestone 2+)
├── include/tokamak/    # Public headers, one subdirectory per module
├── src/                # Implementation, mirrors include/tokamak/ layout
├── tests/
│   ├── unit/           # Catch2 unit tests
│   └── golden/         # Integration tests against checked-in workload fixtures
├── docs/adr/           # Architecture Decision Records
├── learning/           # Write-ups of design reasoning and mistakes, one per session
├── rough/              # Untracked scratch notes and session plans
└── project.md          # Full project spec: goals, architecture, milestones
```

See [project.md Section 18](project.md#18-repository-layout) for the
eventual full layout as later milestones (cache, speculation, kernels,
benchmarks) land.

## Engineering conventions

- C++23, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Werror` on project code (third-party dependencies are exempt).
- Every non-trivial design decision that could reasonably have gone another
  way gets an ADR before implementation, not after.
- Programmer/invariant-violation bugs call `panic()` (abort); recoverable,
  expected failures are reported through typed return values
  (`std::expected`) — see
  [`docs/adr/002-error-boundary-policy.md`](docs/adr/002-error-boundary-policy.md).
- Tests are layered: unit tests exercise pure logic against hand-built
  fixtures; golden/integration tests exercise real scheduler/backend
  mechanics against checked-in workload files.
