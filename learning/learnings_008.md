# Learnings 008 — Building the Simulation Runner and Test Suite: Boundaries, Panics, and Real Timing

## Questions

1. Why does `RequestSummary` exist as its own struct in `simulation.h`, rather than having `analysis.cpp` just take `Request*`/`FifoScheduler` directly?
2. Why does `run_simulation()` take an injected `TraceWriter*` instead of a file path?
3. Why couldn't `workload_test.cpp` test `load_workload()`'s error paths, even though they're the most bug-prone part of the parser?
4. `tokamak_app`'s missing `target_include_directories` call compiled fine for `main.cpp` but silently would have broken `golden_trace_test.cpp` — why did one succeed and the other fail, from the same root cause?
5. The deadline-miss golden test's timing didn't match a naive per-request cost estimate — why, and what does that reveal about `FifoScheduler`'s actual batching semantics?

---

## 1. A flat snapshot struct as the seam between the scheduler and analysis

`RequestSummary` isn't just a convenience DTO — it exists because `Request` cannot outlive the scheduler that owns it. `Request` holds a `const Clock&` (making it non-assignable) and lives inside `FifoScheduler`'s `std::vector<std::unique_ptr<Request>>`; `run_simulation()` constructs `FifoScheduler` as a local variable, so once the function returns, every `Request` it created is destroyed. Any `SimulationResult` field that held a `Request*` or `shared_ptr<Request>` would be a dangling reference the instant the caller looked at it.

The fix — snapshotting each request's relevant fields into `RequestSummary` *before* `run_simulation()` returns — does more than avoid a dangling pointer. It also decouples `analysis.cpp` from `Request`/`RequestLifecycle`/`FifoScheduler` entirely: `compute_latency_stats()`, `compute_throughput_stats()`, and `check_invariants()` only need to know about a flat struct of optional timestamps and counts. That's what made `analysis_test.cpp` possible as pure unit tests — every fixture in that file is a hand-built `RequestSummary`, with no `FakeClock`, no `MockBackend`, no `FifoScheduler` in sight. If `analysis.cpp` had taken `Request` objects directly, testing its percentile math or invariant logic would have required standing up a real scheduler for every test case, even ones with nothing to do with scheduling.

**Takeaway**: a lifetime constraint (owned data about to go out of scope) and a testability goal (decoupling pure logic from stateful machinery) can point to the same fix — snapshot into a flat, self-contained value type at the boundary where ownership would otherwise end. The struct's existence is justified twice over, not just once.

---

## 2. Dependency injection at the outermost layer, not the library layer

`run_simulation(config, TraceWriter* trace_writer = nullptr)` takes a pointer to an already-constructed `TraceWriter`, rather than a file path it would open itself. `main.cpp` — the outermost, least-reusable layer — is the one that calls `std::ofstream`, checks whether `--trace-out` was given, and constructs the `TraceWriter` around it. `run_simulation()` stays agnostic about *where* trace output goes.

This is the same pattern `TraceWriter` itself already used one layer down (wrapping `std::ostream&` instead of a path, so tests can point it at an `ostringstream`). Piece D's golden tests benefited from `run_simulation()` following the same discipline one level up: `golden_trace_test.cpp` never had to touch the filesystem or construct a `TraceWriter` at all — it just calls `run_simulation(config)` with the trace-writer argument omitted, and inspects `SimulationResult` directly. Had `run_simulation()` instead taken a path string and opened the file internally, every golden test would have needed to manage a real (or temp) file on disk just to get at data it was never going to read from that file anyway.

**Takeaway**: "who owns the side-effecting resource" should be decided by asking which caller actually needs to control it — here, only `main.cpp` (a human running the binary) cares about trace-file placement; every other caller (tests, and hypothetically a future server loop) just wants the computed result. Pushing the resource ownership outward, to the layer that has an actual opinion about it, kept every inner layer testable without extra setup.

---

## 3. `panic()` calling `std::abort()` sets a hard boundary on what Catch2 can test

`workload.cpp`'s `load_workload()` panics on three input errors: an unopenable file, malformed JSON, and a missing/wrong-typed field. Those are exactly the paths most worth testing — parsers are where off-by-one and wrong-type bugs like to hide. But `panic()` calls `std::fprintf` followed by `std::abort()` unconditionally, in every build type. `std::abort()` terminates the process; it cannot be caught by a `try`/`catch`, a Catch2 `REQUIRE_THROWS`, or any other in-process mechanism — the entire test binary (all other `TEST_CASE`s included) would go down with it.

`mock_backend_test.cpp` had already run into this and documented it inline (a double-release test explicitly avoids exercising `release()`'s panic path for this exact reason). `workload_test.cpp` followed the same precedent: its only path to testing `load_workload()`'s error handling would be a separate death-test harness (spawning a subprocess and checking its exit status) — infrastructure that doesn't exist anywhere in this codebase yet, and wasn't worth introducing for three error paths in one file. Instead, `workload_test.cpp` scoped itself to the success paths only, with an explicit comment naming which paths are excluded and why, rather than silently having lower coverage with no trace of the decision.

**Takeaway**: a project-wide choice about how to signal invariant violations (`panic()` → hard abort, everywhere, no exceptions) has a direct, permanent consequence for what a chosen test framework can verify. Once that choice is made, "the most bug-prone code path is untestable in-process" isn't a testing gap to feel bad about — it's a predictable, named consequence of the panic design, and documenting the exclusion where the test file lives (not just once, in one file, and hoping the next author remembers) is what keeps that consequence from being mistaken for an oversight later.

---

## 4. A missing `target_include_directories` call is invisible until a second consumer exists

`apps/tokamak/CMakeLists.txt` defined `tokamak_app` without ever calling `target_include_directories` on it — meaning nothing about the library's public interface told CMake (or a linking target) where `simulation.h`/`analysis.h`/`workload.h` actually live. `main.cpp` compiled and ran correctly anyway, because `#include "analysis.h"` uses quoted-include lookup, which every major compiler resolves relative to the *including file's own directory* first — and `main.cpp` lives in the same directory as `analysis.h`. The bug was real but had zero symptoms, because there was exactly one consumer, and it happened to be co-located with the headers it needed.

The gap only became visible once `tests/golden/golden_trace_test.cpp` — a file in a *different* directory — tried to `#include "simulation.h"` and `"analysis.h"`. Quoted-include same-directory fallback doesn't reach across directories; without an explicit `PUBLIC` include path exported from `tokamak_app`, that second consumer would have failed to compile with a "file not found" error, for a cause completely unrelated to anything in the golden test itself.

**Takeaway**: an omitted `target_include_directories` (or any interface-boundary declaration) can silently "work" for arbitrarily long if the only consumer happens to satisfy the missing requirement by accident of file layout. This is a general shape worth watching for beyond CMake specifically: a boundary that's underspecified but happens to work for consumer #1 is not evidence the boundary is correctly specified — it's evidence no one has tried consumer #2 yet. The fix (adding one line, `target_include_directories(tokamak_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})`) was trivial once the real second consumer forced the question; the risk was entirely in not noticing before then.

---

## 5. Batch-level backend calls mean co-batched requests share a batch's total cost, not their own

The deadline-miss golden test needed real numbers for two requests — `big` (20 prompt tokens) and `tight` (1 prompt token, 25ms deadline) — submitted in the same tick. A first, informal estimate assumed each request's prefill cost applied only to that request individually, i.e., `tight` would finish prefill almost immediately (1ms) while `big` was still working, and transition to decode at a different, earlier timestamp than `big`.

That estimate was wrong, because it didn't match how `FifoScheduler::prefill_phase()` and `decode_phase()` are actually structured: both build the *entire* batch's `PrefillBatch`/`DecodeBatch` up front, make **one** call into the backend for the whole batch, and only afterward loop back over every request in that batch to run its state transitions — using whatever `clock.now()` is by the time the single batch call returns. Inside `MockBackend::prefill()`, the clock does advance once per request in the batch (accounting for `tight`'s 1ms and `big`'s 20ms separately, summing to 21ms total) — but that's an implementation detail of *how the mock backend spends time*, not something `FifoScheduler` exposes per-request. Every request in the batch gets the same `prefilling_at`/`decoding_at`/`first_token_at` timestamp: the batch's cumulative end time, not their own individual slice of it. That's what let `tight` — despite needing only 1ms of "its own" prefill work — still be well past its 25ms deadline by the time its first token arrived, purely from being batched alongside a request 20x its size.

**Takeaway**: when a scheduler batches heterogeneous-cost work into single backend calls (a real, load-bearing design choice per ADR-007's same-tick prefill-to-decode promotion), per-request latency is a function of the *whole batch's* cost, not the individual request's cost — a genuinely different (and worse, for small requests sharing a batch with large ones) latency profile than if requests were processed one at a time. This is exactly the kind of emergent behavior a golden/integration test is suited to catching and pinning down precisely, where a unit test operating on hand-built `RequestSummary` data (as in `analysis_test.cpp`) never would, since it doesn't exercise the scheduler's actual batching mechanics at all — the two test layers in this session's suite each cover a real gap the other one structurally cannot.

---

## Summary: what this session's test-suite work added, beyond the design work in ADR-008/ADR-009/ADR-010

| Concern | What we learned |
|---|---|
| Snapshot structs at ownership boundaries | `RequestSummary` solves a lifetime problem (dangling pointers into a soon-destroyed scheduler) and a testability problem (decoupling pure analysis logic from stateful machinery) at once — same fix, two independent justifications |
| Where to inject a side-effecting resource | Push resource ownership (e.g. an open file) to the outermost layer that has an actual opinion about it (`main.cpp`), so every inner layer (including tests) can operate on the resource's abstraction (`TraceWriter`/`ostream&`) without needing the real thing |
| `panic()`'s hard boundary on testability | A process-wide `std::abort()`-based invariant-violation policy makes certain code paths permanently untestable in-process without new subprocess-based test infrastructure — a predictable, nameable consequence, not a gap to silently accept |
| Silent interface-boundary gaps | A missing `target_include_directories` (or similar) can compile cleanly for years if the only consumer happens to satisfy it by accident (same-directory quoted includes) — a single working consumer is not evidence a boundary is correctly specified |
| Batch-level cost vs. per-request cost | A scheduler that calls the backend once per batch gives every request in that batch the batch's *total* elapsed cost, not its own — small requests inherit the latency of whatever they're co-batched with, a real emergent property only an integration-level (golden) test exercises |
