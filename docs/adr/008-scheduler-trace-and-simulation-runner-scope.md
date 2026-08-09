# ADR-008: Structured scheduler trace, TickReport, and simulation runner scope

## Status
Accepted

## Date
2026-08-08

## Context

Milestone 1's three remaining deliverables — in-process request submission API,
deterministic simulation runner, structured event trace — are not independent
pieces; project.md Section 29 ("Immediate Next Step") describes them as one
cohesive pipeline:

```text
JSONL workload
      │
      ▼
virtual clock → admission → FIFO scheduler → mock backend
      │                                      │
      └──────────── scheduler trace ◀────────┘
```

Before writing this, five design questions needed resolving, none of which are
answered by ADR-007 or any prior ADR:

1. How does `FifoScheduler` expose per-tick decision data (batch composition,
   token counts) to anything outside itself, without coupling the scheduler to
   how that data gets logged, serialized, or displayed?
2. How closely should the emitted trace match project.md Section 8.3's full
   scheduler-trace JSON schema, given several of its fields (`policy` beyond
   FIFO, `kv_pages_free`, `earliest_slack_ms`, `decision_us`) describe
   subsystems that don't exist yet (KV-cache model, deadline-aware policy,
   scheduling-time instrumentation)?
3. What JSON library, if any, should back serialization?
4. What should the Milestone 1 workload JSONL schema look like, given
   project.md Section 11's full record (`messages`, `temperature`, `tenant`)
   describes HTTP/chat-completion concepts that don't exist until Milestone 2?
5. What shape should the runner's entrypoint take, given project.md Section 10
   describes a multi-subcommand `tokamak` CLI (`serve`/`benchmark`/`replay`/...)
   of which none exist yet except this one simulation mode?

## Decision

### 1. `tick()` returns a `TickReport`; the scheduler stays trace-format-agnostic

`FifoScheduler::tick()` changes from `void` to returning a `TickReport` value
struct — plain counts and sums describing what that tick actually did
(`expired_count`, `prefill_attempted/succeeded/failed`, `prefill_tokens`,
`decode_attempted/completed/failed`, and the tick's `TimePoint`). The scheduler
does not know about JSON, logging, or file I/O; it only reports facts about its
own decision-making, the same way `PrefillResult`/`DecodeResult` report backend
facts without knowing how a caller will use them.

This mirrors vLLM's `Scheduler.schedule()` returning a plain `SchedulerOutputs`
data structure, consumed by a separate stats/logging layer — not an
observer/callback threaded into the scheduler's hot path. Two concrete reasons
this is the better shape here, not just "how vLLM happens to do it":

- **Testability**: a test asserts on `TickReport` fields directly, in the same
  style as every existing scheduler test — no JSON parsing, no callback
  wiring, no new test infrastructure.
- **Decoupling for free**: today's consumer is a JSONL trace writer; tomorrow's
  might be Prometheus counters or an OpenTelemetry span (project.md Section 15
  names both as eventual telemetry backends). Neither requires touching
  `FifoScheduler` — only the thing that turns a `TickReport` into an event
  changes. This is the same "narrow boundary, decisions never straddle it"
  philosophy ADR-004 established for the backend boundary, applied here to the
  scheduler-to-telemetry boundary.

### 2. Trace schema is a deliberate, honest subset — not a stubbed full schema

The emitted trace event includes only fields that describe something real
today: `iteration`, `timestamp_ns`, `policy` (literal `"fifo"` — a fact, since
that's the only policy that exists, not a placeholder), `runnable_prefill`,
`runnable_decode`, `selected_sequences`, `prefill_tokens`, `decode_tokens`.

`kv_pages_free`, `earliest_slack_ms`, and `decision_us` are omitted entirely,
**not** stubbed with placeholder values (e.g. `kv_pages_free: 0`). This follows
the same precedent `BackendCapabilities` already set: "queried, never guessed."
A stubbed numeric field looks like a real measurement and invites a future
reader (or a downstream analysis script) to treat it as one; an absent field
honestly says "this subsystem doesn't exist yet." Each omitted field will be
added exactly when its subsystem lands: `kv_pages_free` with the KV-cache
policy layer (project.md Section 8.5), `earliest_slack_ms` with the
deadline-aware policy (Section 8.3), `decision_us` once scheduling itself is
instrumented for timing (not simulated backend cost, which is already
measured, but the scheduler's own decision-making overhead).

### 3. `nlohmann::json`, added via `FetchContent`

Serialization is backed by `nlohmann::json`, fetched the same way Catch2
already is in the root `CMakeLists.txt`. A hand-rolled writer was considered
and rejected: our trace/workload schemas are simple today, but string
escaping, number formatting, and nested-object correctness are solved problems
that a well-tested library handles better than a purpose-built one would, for
no learning benefit — unlike the kernels track (project.md Section 17), JSON
serialization is explicitly not one of this project's areas of technical
exploration.

### 4. Milestone 1 workload schema is a documented, versioned reduction

The Milestone 1 JSONL workload record includes only fields `Request` and
`FifoScheduler` can actually use: `id`, `arrival_ms`, `prompt_token_ids`,
`max_tokens`, `deadline_ms`. Section 11's `messages`, `temperature`, and
`tenant` fields are omitted — they describe chat-completion request shape and
multi-tenant weighting, neither of which exists until Milestone 2 (HTTP
frontend) and the weighted-fair policy (Section 8.3), respectively. This
reduced schema is not a new, permanent format; it is explicitly the Milestone 1
subset of Section 11's eventual full record, and will grow toward it as those
fields gain real meaning, the same way the trace schema will.

### 5. Simulation runner is a minimal single-purpose executable, not a CLI dispatcher

The runner (`apps/tokamak/main.cpp` — path per project.md Section 18's
reserved layout) takes a workload file and runs the simulation; it does not
implement `tokamak serve/benchmark/replay/validate/version` subcommand
dispatch (Section 10). Building a dispatcher now would mean designing it
around exactly one real subcommand, with every design question a dispatcher
actually needs to answer (how subcommands share `--config`, how they're
registered, what `tokamak version` reports) unanswerable in the abstract with
only one data point. This follows the same deferral discipline already applied
elsewhere in this project — `verify()` deferred to Milestone 6 (ADR-004),
preemption deferred until real memory pressure exists to trigger it
(learnings_005 Section 4): build the abstraction when a second real consumer
exists to generalize from, not before.

## Consequences

- `FifoScheduler::tick()`'s signature changes from `void` to `TickReport`;
  existing call sites (tests) are unaffected since the return value is
  ignorable, and Catch2 test cases calling `tick()` do not need to change
  unless they want to assert on the new report.
- A new `telemetry` module (`include/tokamak/telemetry/`,
  `src/telemetry/`) is introduced, matching project.md Section 18's reserved
  layout — its only responsibility is turning a `TickReport` (plus iteration
  number and policy name) into a trace-event JSON object. It has no knowledge
  of `FifoScheduler`, `Request`, or `InferenceBackend` internals beyond what
  `TickReport` already exposes.
- `nlohmann::json` becomes the project's first non-test third-party dependency
  (Catch2 is test-only). This is a real, if small, addition to the dependency
  surface and build time; acceptable since it directly serves a Milestone 1
  deliverable rather than speculative future need.
- The Milestone 1 workload schema will need a second, explicit revision once
  Milestone 2's HTTP frontend needs `messages`/`temperature`, and again when
  the weighted-fair policy needs `tenant` — each revision should be a small
  follow-up ADR note or amendment, not a silent schema drift.
- "Golden scheduler traces pass" (Milestone 1 exit criterion) is satisfied by
  a `tests/golden/` fixture: one checked-in workload plus its expected trace
  output, diffed by a test — not merely informal multi-request ordering
  assertions in `fifo_scheduler_test.cpp` (which remain useful as unit-level
  coverage, but were never a substitute for this).
- A future ADR (or an amendment to this one) will be needed once a CLI
  dispatcher is actually built (Milestone 2+), to decide how `apps/tokamak/`
  grows from "one entrypoint" into "one entrypoint with subcommands" without
  breaking the Milestone 1 simulation invocation.
