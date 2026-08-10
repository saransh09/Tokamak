# ADR-011: HTTP frontend architecture for Milestone 2

## Status
Proposed

## Date
2026-08-10

## Context

Milestone 1 built a purely synchronous, single-threaded simulation: one
`FakeClock`, one `FifoScheduler::tick()` loop, one process, no networking.
Milestone 2 (project.md Section 19) requires an async HTTP frontend exposing
`/v1/completions` and `/v1/chat/completions` with SSE streaming, backed
initially by the same `MockBackend` and eventually by a real `llama.cpp`
backend, plus health/readiness/metrics endpoints and graceful shutdown
(Section 8.1, Section 14).

Six design questions need resolving before writing any networking code, none
answered by prior ADRs:

1. What HTTP/async library, and how is it brought into the build?
2. Callback-based or coroutine-based async code?
3. How does network I/O relate to the scheduler's tick loop, given
   `FifoScheduler::tick()` is synchronous and backend calls (especially a real
   model) block for tens to hundreds of milliseconds?
4. How do generated tokens cross from the scheduler side to a streaming HTTP
   response, given project.md Section 8.1 requires a *bounded* per-request
   output queue and backpressure accounting?
5. What admission policy gates `scheduler.submit()`, given Section 8.2 lists
   three policies (capacity-only, deadline-aware, reserved-capacity) and only
   one can be justified with the operational data available today?
6. What does graceful shutdown (Section 14) look like across two now-distinct
   execution domains (network executor, engine thread)?

## Decision

### 1. Boost.Asio + Beast, fetched header-only via `FetchContent`

Project.md Section 8.1 explicitly suggests Boost.Asio and Beast "for
portability and reviewability." Alternatives considered:

- **System-installed Boost** (`brew install boost` / `apt install
  libboost-all-dev`): rejected — makes the build depend on an environment
  fact outside the repository, works differently (or not at all) on a fresh
  clone or CI image, and breaks the project's existing pattern of pinning
  every dependency through `FetchContent` (Catch2, nlohmann_json — see root
  `CMakeLists.txt`).
- **Standalone (non-Boost) Asio + a separate HTTP library** (e.g.
  cpp-httplib, uWebSockets): rejected — Beast (the HTTP/WebSocket layer) is
  Boost-specific and is what project.md names directly; introducing a
  different HTTP library means losing Beast's SSE-friendly chunked-response
  and header-parsing primitives for no stated benefit.

Boost is fetched the same way Catch2/nlohmann_json already are — a
`FetchContent_Declare` block in the root `CMakeLists.txt`, pinned to a
specific tag. Only the header-only libraries actually used (`asio`, `beast`,
their direct dependencies — `system`, `config`, `assert`, `throw_exception`,
`core`, etc.) are targeted; Boost's compiled-library components (e.g.
`boost_thread`, `boost_filesystem`) are not needed since Asio/Beast's
networking and HTTP parsing are header-only. This keeps first-build fetch and
compile cost bounded instead of pulling in all of Boost.

### 2. C++20 coroutines (`co_await` / `asio::awaitable<T>`), not callbacks

Asio supports three async styles: raw callbacks, `std::future`-based
completion, and C++20 coroutines via `asio::awaitable<T>` /
`asio::co_spawn`. This project uses **coroutines** for every connection
handler.

Why not callbacks: a single request's lifecycle — accept connection, read
headers, read body, validate, submit to scheduler, await tokens in a loop,
write SSE frames, close — is a sequence of dependent async steps. Written as
callbacks, each step nests inside the previous one's completion handler,
producing the "callback pyramid" that makes control flow, error propagation,
and especially *cancellation* (a client disconnecting mid-stream) hard to
reason about — state that would naturally be a local variable in sequential
code (bytes read so far, current token index, whether the deadline fired)
instead has to be hoisted into a heap-allocated closure or object per
callback.

Why coroutines: `asio::awaitable<T>` lets each connection be written as
sequential-looking code — `co_await socket.async_read(...)`, `co_await
channel.async_receive(...)` — where each `co_await` is an explicit
suspension point, and a thrown exception (e.g. from a malformed request)
propagates up through ordinary `try`/`catch` the same way it would in
non-async code, matching this project's documented exception-boundary policy
(project.md Section 16, "Error handling"). Cancellation becomes a first-class
Asio concept (`asio::cancellation_signal`) rather than a hand-rolled flag
checked in every callback.

C++20 coroutine support is confirmed available (project.md Section 16
requires "C++20 minimum," and this project already builds with `-std=c++23`
under Apple Clang 21).

### 3. Two execution domains: network executor and engine thread, bridged by explicit queues

`FifoScheduler::tick()` is synchronous, and a real backend's `prefill()`/
`decode()` calls (Section 8.4) are blocking CPU/GPU work that can take tens
to hundreds of milliseconds. Running the tick loop directly inside an Asio
coroutine on the network `io_context` would stall every other connection on
that executor for the duration of every batch — violating Section 8.1's
1,000-idle-connections acceptance criterion the moment any real work is in
flight, and directly contradicting Section 16's explicit engineering
guideline: "Never perform blocking backend work while holding scheduler
locks" / "Separate network executors from model-execution threads."

The decision: **one dedicated engine thread** owns `FifoScheduler` and runs
the tick loop continuously (replacing the simulation's discrete-event loop
with a real-time loop driven by `SystemClock`); Asio's `io_context` (with a
small thread pool) owns all network I/O and never touches the scheduler
directly. Two explicit, thread-safe boundaries connect them:

- **Submission queue** (network → engine): an HTTP handler coroutine that has
  validated and admitted a request pushes it onto a bounded, mutex-guarded
  queue; the engine thread drains this queue once per tick before calling
  `scheduler.submit()`. This is the only place a `Request` crosses from a
  network thread to the engine thread.
- **Per-request output channel** (engine → network): see Decision 4.

No other shared mutable state exists between the two domains. This satisfies
Section 16's requirement to document every lock and the state it protects:
exactly one lock, protecting exactly one queue.

### 4. Per-request bounded SPSC channel for token delivery, with explicit backpressure timing

Each admitted request gets one single-producer single-consumer channel
(bounded capacity, e.g. 32 slots — the "bounded per-request output queue"
Section 8.1 requires) created at submission time and owned jointly by the
engine-side request state and the network-side connection coroutine:

- **Producer (engine thread)**: after each `tick()`, for every request that
  emitted a token this tick, push the token onto that request's channel. If
  the channel is full, do not block the engine thread (that would stall
  every other request's scheduling) — instead, start (or continue) a
  per-request backpressure timer, distinct from the request's model-time
  accounting. If the timer exceeds a configured timeout, the engine cancels
  the request (`scheduler.cancel(id)`) per Section 8.1: "The server may
  cancel a request if the output queue remains full past a configured
  timeout."
- **Consumer (connection coroutine)**: `co_await channel.async_receive(...)`
  pops a token and writes one SSE `data:` frame. On the client disconnecting,
  the coroutine's `co_await` on the socket write throws/returns an error;
  the coroutine's cleanup path calls `scheduler.cancel(id)` and closes its
  end of the channel, satisfying Section 8.1's disconnect-propagation
  requirement and Section 14's "fail affected requests explicitly."

Backpressure time being tracked separately from model time (Section 8.1)
means the channel records, per request, cumulative time spent full — a
number that folds into telemetry (Section 8.8) but never contaminates the
TTFT/ITL/E2E measurements ADR-010 already defined, which describe scheduling
and backend time only.

### 5. Admission control: capacity-only policy for Milestone 2; deadline-aware deferred

Of Section 8.2's three admission policies, Milestone 2 implements only
**capacity-only**: reject with HTTP `429` once a configured hard limit on
in-flight requests (or queued prompt tokens) is reached, with a
machine-readable rejection reason and a counter labeled by that reason
(Section 8.2's "Overload behavior" requirements).

Deadline-aware admission ("reject when estimated earliest service time
exceeds deadline") is deferred, not because it is unimportant, but because
computing a defensible "estimated earliest service time" requires calibrated
per-token cost data from whichever backend is actually serving traffic —
data this project does not have yet for the mock backend (its costs are
synthetic, chosen for scheduler-testing convenience, not representative of
real serving) and won't have for llama.cpp until Milestone 2's own
integration work produces it. Building an estimator on invented numbers would
produce a policy that looks complete but rejects or admits requests for the
wrong reasons — worse than the honest simplicity of a hard limit. This
follows the same "don't stub a real-looking number for a subsystem that
doesn't exist yet" discipline ADR-008 established for the scheduler trace's
omitted fields.

The admission controller is still its own class/interface — invoked between
"HTTP handler finished parsing/validating a request body" and
"`scheduler.submit()`" — so that adding the deadline-aware policy later
(planned for Milestone 4, alongside the deadline-aware scheduling policy in
Section 8.3) is a policy swap behind an existing seam, not a rewrite of the
HTTP handler.

### 6. Graceful shutdown sequence matches Section 14 exactly, spanning both domains

On `SIGINT`/`SIGTERM`, an Asio signal handler sets a shared atomic
`shutting_down` flag and begins the documented sequence:

1. Readiness endpoint (`/readyz`) starts returning `503` immediately (reads
   the atomic flag).
2. The listening acceptor stops accepting new connections.
3. The engine thread, checking the flag once per tick, stops draining the
   submission queue (already-admitted requests continue to completion) for
   up to a configured grace period.
4. Requests still active after the grace period are cancelled via the same
   `scheduler.cancel(id)` path a client disconnect uses.
5. Telemetry/trace output is flushed (same `TraceWriter` from ADR-008,
   reused unchanged).
6. Backend resources are released (`InferenceBackend::release()` per
   sequence, already required by the interface).
7. Process exits with a meaningful status code.

This sequence is deliberately the same six-step list Section 14 specifies
verbatim — no new shutdown policy is being invented, only mapped onto the
two-thread architecture from Decision 3.

## Consequences

- Boost (header-only subset) becomes the project's second non-test
  third-party dependency (after nlohmann_json), fetched and pinned the same
  way. First-build cost increases; runtime cost is negligible since Asio and
  Beast are header-only.
- The codebase gains a genuinely concurrent domain for the first time —
  everything through Milestone 1 was single-threaded by construction. Every
  new lock (there is exactly one specified above: the submission queue's)
  must be documented at the point it's implemented, per Section 16's
  requirement, and this ADR's "exactly one lock" claim should be revisited if
  implementation reveals a second one is actually needed.
- `FifoScheduler` itself does not change: it still exposes a synchronous
  `submit()`/`tick()`/`cancel()` API. The engine thread is a new caller of
  that existing API in a real-time loop, not a redesign of the scheduler.
  This preserves every existing Milestone 1 test unchanged.
- The per-request output channel is a new primitive with no precedent in the
  codebase; it needs its own unit tests (bounded capacity, backpressure
  timing, cancellation on producer/consumer-side close) before being wired
  into the HTTP layer.
- The simulation runner (`apps/tokamak/main.cpp`, `simulation.cpp`) is
  unaffected — it keeps using `FakeClock` and the existing discrete-event
  loop. The HTTP server is a new, separate entrypoint; how it and the
  simulation runner coexist under one `tokamak` binary (Section 10's
  multi-subcommand CLI, deferred by ADR-008) is an open question this ADR
  does not resolve — likely `tokamak serve` vs. the current bare invocation,
  to be decided when the CLI dispatcher question ADR-008 flagged is actually
  addressed.
- Deadline-aware admission remains unimplemented until Milestone 4; the
  capacity-only policy alone does not fully satisfy Section 8.2's
  responsibilities ("estimate whether a request can meet its deadline") —
  this is a known, documented gap, not an oversight.
- No `io_uring` experiment exists yet; the `HttpTransport`-style boundary
  this ADR implies (handlers programmed against an abstraction, Beast being
  the only implementation) needs to actually be drawn as a real interface
  during implementation, not just asserted here — if the first
  implementation inlines Beast types directly into handler code, that
  boundary will need a follow-up refactor before a second transport could be
  added.
