# Tokamak

> A high-performance, SLO-aware LLM inference server written in modern C++.

Tokamak is a systems-first personal project for serving generative language models under concurrent load. It combines asynchronous networking, continuous batching, deadline-aware scheduling, KV-cache management, speculative decoding, observability, and reproducible performance evaluation behind an OpenAI-compatible API.

The project's central claim is not merely that it can generate text. Tokamak should demonstrate that a C++ runtime can **adapt its serving policy to the workload**, increasing useful throughput while respecting a user-defined latency objective.

---

## 1. Project Thesis

Modern LLM inference is a scheduling and memory-management problem wrapped around expensive tensor computation. A useful serving system must decide:

- when to admit or reject work;
- which requests to execute together;
- how to balance prompt prefill against token decoding;
- how to allocate and reclaim KV-cache memory;
- when speculative decoding is likely to help;
- how aggressively to optimize throughput without destroying tail latency;
- and how to expose enough evidence to explain those decisions.

Tokamak explores these decisions in a compact, inspectable C++ codebase.

### Primary hypothesis

Under mixed concurrent workloads, an adaptive policy that chooses batching and speculative-decoding parameters from recent runtime observations can achieve higher SLO-compliant throughput than a fixed configuration.

### Project-level success statement

> Tokamak automatically selects serving policies that improve goodput over static baselines while keeping p95 time-to-first-token within a configured latency target.

This claim must be supported by reproducible experiments, confidence intervals, workload traces, versioned configurations, and raw benchmark artifacts.

---

## 2. Goals

### 2.1 Systems goals

- Build the request lifecycle in C++20 or newer.
- Implement asynchronous HTTP serving and token streaming.
- Implement cancellation, deadlines, backpressure, and graceful shutdown.
- Implement a continuous-batching scheduler with interchangeable policies.
- Build a cache-block abstraction and a Tokamak-owned KV-cache policy layer.
- Isolate model execution behind a narrow backend interface.
- Keep ownership, lifetimes, and concurrency behavior explicit.
- Produce actionable telemetry without materially distorting performance.
- Run correctly under sanitizers, fault injection, and high concurrency.

### 2.2 AI-infrastructure goals

- Serve at least one open-weight causal language model.
- Support ordinary autoregressive decoding.
- Support draft-model speculative decoding.
- Adapt speculative draft length or enablement from observed acceptance behavior.
- Measure quality-preserving performance rather than tokens per second alone.
- Add a learned or online decision policy only after deterministic baselines exist.

### 2.3 Portfolio goals

The repository should make the following abilities obvious to a reviewer:

- modern C++ design and ownership discipline;
- concurrent and asynchronous systems programming;
- memory-aware runtime design;
- practical understanding of transformer inference;
- rigorous benchmarking and statistical reasoning;
- profiling-led optimization;
- production-minded reliability and observability;
- the ability to explain trade-offs, not merely implement features.

---

## 3. Non-goals

Tokamak is deliberately not all of the following:

- a new foundation model;
- a general-purpose deep-learning framework;
- a replacement for every feature in llama.cpp, vLLM, or TensorRT-LLM;
- a distributed multi-node runtime in its first release;
- a GUI-first chat application;
- a thin API wrapper around an existing model server;
- an attempt to implement an entire optimized transformer stack from scratch;
- a benchmark whose configurations differ between competitors;
- a project that claims speedups without publishing raw data.

The initial target is **one process, one host, one model family, and one production-quality request path**. GPU support may be supplied by the model backend. Tokamak owns orchestration, scheduling, policy, lifecycle, and measurement.

---

## 4. Target User and Core Use Case

### Target user

An inference engineer running an open-weight model on a workstation or single server who wants to explore latency-throughput trade-offs under realistic concurrent traffic.

### Core use case

The operator starts Tokamak with a service-level objective:

```bash
tokamak serve \
  --model ./models/target.gguf \
  --draft-model ./models/draft.gguf \
  --max-kv-memory 10GiB \
  --ttft-slo-ms 300 \
  --policy adaptive
```

Clients send OpenAI-compatible streaming requests. Tokamak schedules them, streams generated tokens, and exports metrics and traces explaining:

- queueing delay;
- prefill latency;
- time to first token;
- inter-token latency;
- batch composition;
- cache usage;
- speculative acceptance;
- policy changes;
- completed, rejected, cancelled, and SLO-compliant requests.

---

## 5. Definitions and Performance Vocabulary

Tokamak uses precise performance terminology.

### Time to first token

For request $$r$$:

$$
TTFT_r = t_{\text{first token},r} - t_{\text{accepted},r}
$$

### Inter-token latency

For generated tokens $$i > 1$$:

$$
ITL_{r,i} = t_{r,i} - t_{r,i-1}
$$

Report the distribution across tokens and requests. Do not hide stalls behind a single average.

### End-to-end latency

$$
E2E_r = t_{\text{completed},r} - t_{\text{accepted},r}
$$

### Throughput

Report both:

- output tokens per wall-clock second;
- completed requests per wall-clock second.

Prompt-processing throughput must be reported separately from decode throughput.

### Goodput

Goodput counts only requests satisfying the declared SLO:

$$
\text{Goodput} =
\frac{
  \left|\{r \in R_{\text{completed}} : TTFT_r \le SLO_{TTFT}\}\right|
}{
  \text{benchmark duration}
}
$$

A later version may define a compound SLO including maximum inter-token latency and end-to-end latency.

### Speculative acceptance rate

$$
A = \frac{\text{accepted draft tokens}}{\text{proposed draft tokens}}
$$

Also report accepted tokens per verification step and speedup relative to non-speculative decoding.

### SLO attainment

$$
\text{SLO attainment} =
\frac{\text{requests satisfying the SLO}}
{\text{eligible completed requests}}
$$

Cancelled requests are categorized separately and never silently excluded.

---

## 6. High-Level Architecture

```text
Clients
  │
  │ HTTP + Server-Sent Events
  ▼
┌─────────────────────────────────────────────────────────┐
│ Async Frontend                                          │
│ parse · validate · authenticate hook · stream · cancel  │
└───────────────────────┬─────────────────────────────────┘
                        ▼
┌─────────────────────────────────────────────────────────┐
│ Admission Controller                                    │
│ capacity · deadlines · priorities · overload behavior   │
└───────────────────────┬─────────────────────────────────┘
                        ▼
┌─────────────────────────────────────────────────────────┐
│ Request State Store                                     │
│ lifecycle · token buffers · deadlines · cancellation    │
└───────────────────────┬─────────────────────────────────┘
                        ▼
┌─────────────────────────────────────────────────────────┐
│ Scheduler                                               │
│ prefill queue · decode queue · continuous batch builder │
│ FIFO · fair · deadline-aware · adaptive policies        │
└──────────────┬────────────────────────────┬─────────────┘
               │                            │
               ▼                            ▼
┌───────────────────────────┐  ┌───────────────────────────┐
│ Cache Policy Layer        │  │ Speculation Controller    │
│ pages · prefixes · evict  │  │ enable · draft length     │
│ reserve · account · spill │  │ acceptance prediction     │
└──────────────┬────────────┘  └─────────────┬─────────────┘
               └──────────────┬──────────────┘
                              ▼
┌─────────────────────────────────────────────────────────┐
│ Backend Interface                                       │
│ tokenize · prefill · decode · verify · release sequence │
├───────────────────────────────┬─────────────────────────┤
│ llama.cpp backend             │ deterministic mock      │
│ optional ONNX Runtime backend │ optional tiny backend   │
└───────────────────────┬───────┴─────────────────────────┘
                        ▼
┌─────────────────────────────────────────────────────────┐
│ Telemetry                                               │
│ metrics · traces · structured logs · benchmark records  │
└─────────────────────────────────────────────────────────┘
```

### Architectural rule

Network I/O, scheduling, cache policy, model execution, and telemetry must not collapse into a single event loop or manager class. Each subsystem has a narrow contract and can be tested with deterministic substitutes.

---

## 7. Request Lifecycle

Every request moves through an explicit state machine.

```text
RECEIVED
   │ validate
   ▼
ADMITTED ───────────────▶ REJECTED
   │ enqueue                capacity / invalid / deadline
   ▼
WAITING_PREFILL ────────▶ CANCELLED
   │ scheduled
   ▼
PREFILLING ─────────────▶ FAILED
   │ KV ready
   ▼
WAITING_DECODE
   │ scheduled repeatedly
   ▼
DECODING ◀──────────────┐
   │ token emitted      │ more tokens
   ├────────────────────┘
   │ EOS / stop / limit
   ▼
COMPLETED
```

### Lifecycle invariants

- A request reaches exactly one terminal state.
- Cancellation is idempotent.
- No token is emitted after terminal state publication.
- Cache resources are released exactly once.
- Backend failures become explicit request failures.
- A disconnected client triggers cancellation unless detached execution is configured.
- Deadlines use a monotonic clock.
- Queue time, prefill time, decode time, and stream-blocked time are measured separately.

---

## 8. Major Subsystems

## 8.1 Async HTTP Frontend

### Responsibilities

- Listen for HTTP connections.
- Parse and validate request bodies with bounded input sizes.
- Expose OpenAI-compatible endpoints.
- Stream tokens via Server-Sent Events.
- Propagate client disconnects and explicit cancellation.
- Enforce request and connection deadlines.
- Apply output backpressure when clients consume slowly.
- Provide health, readiness, metrics, and build-information endpoints.

### Initial endpoint surface

| Method | Endpoint | Purpose |
|---|---|---|
| `POST` | `/v1/completions` | Text completion |
| `POST` | `/v1/chat/completions` | Chat completion |
| `POST` | `/v1/cancel/{request_id}` | Best-effort explicit cancellation |
| `GET` | `/healthz` | Process liveness |
| `GET` | `/readyz` | Model and scheduler readiness |
| `GET` | `/metrics` | Prometheus-format metrics |
| `GET` | `/debug/build` | Build, model, backend, and CPU/GPU metadata |

### Streaming behavior

- Set `stream=true` to receive SSE frames.
- The final frame includes finish reason and usage.
- A bounded per-request output queue prevents slow clients from consuming unlimited memory.
- Backpressure time is recorded separately from model time.
- The server may cancel a request if the output queue remains full past a configured timeout.

### Suggested implementation

Start with Boost.Asio and Beast for portability and reviewability. Place transport details behind an interface so an `io_uring` experiment can be added later without rewriting the scheduler.

### Acceptance criteria

- At least 1,000 simultaneous idle connections without unbounded growth.
- Streaming frames remain correctly ordered.
- Disconnect tests release request and cache resources.
- Malformed and oversized request bodies are rejected safely.
- Shutdown stops admission, drains or cancels active work according to policy, and exits within a bounded time.

---

## 8.2 Admission Controller

### Responsibilities

- Reject work the runtime cannot serve safely.
- Estimate whether a request can meet its deadline.
- Bound queued requests, prompt tokens, expected output tokens, and memory commitments.
- Apply request priority and tenant weights.
- Prevent one client from monopolizing capacity.

### Admission policies

1. **Capacity-only**
   - Admit until hard queue or memory limits are reached.

2. **Deadline-aware**
   - Reject a request when estimated earliest service time exceeds its deadline.

3. **Reserved capacity**
   - Preserve a fraction of capacity for high-priority requests.

### Overload behavior

Overload must be explicit:

- HTTP `429` for policy or quota limits;
- HTTP `503` for unavailable serving capacity;
- `Retry-After` when a defensible estimate exists;
- a machine-readable rejection reason;
- a counter labeled by rejection reason.

Tokamak must not accept infinite work and call the resulting latency a scheduler problem.

---

## 8.3 Scheduler and Continuous Batching

### Responsibilities

- Track runnable prefill and decode sequences.
- Select work on each scheduling iteration.
- Build a backend batch under token, sequence, memory, and deadline constraints.
- Balance throughput, fairness, and latency.
- Handle arrivals and completions between decode iterations.
- Expose scheduling decisions to traces.

### Batch constraints

A batch may be limited by:

- maximum sequences;
- maximum total tokens;
- maximum prefill tokens;
- available KV-cache pages;
- backend-specific shape or context restrictions;
- configured prefill/decode ratio;
- earliest request deadline;
- maximum queueing window.

### Required scheduling policies

#### FIFO baseline

Oldest runnable request first. Simple, deterministic, and necessary as a reference.

#### Round-robin decode

Give active decode sequences regular progress and guard against starvation.

#### Weighted fair policy

Allocate service by tenant or request class while bounding deficit.

#### Deadline-aware policy

Prioritize requests with lower scheduling slack:

$$
\text{slack}_r =
\text{deadline}_r - t_{\text{now}} - \widehat{T}_{\text{remaining},r}
$$

The estimate may begin as a moving average by prompt/output-length bucket.

#### Adaptive policy

Choose a scheduling profile from live observations. Initial actions should be discrete and inspectable:

- latency profile;
- balanced profile;
- throughput profile;
- speculative profile.

### Fairness requirements

- Long prompts cannot permanently starve short prompts.
- Continuous arrivals cannot permanently starve an admitted request.
- Cancellation removes work promptly.
- Priority affects order but does not bypass hard resource limits.
- Fairness is measured, not assumed.

### Scheduler trace event

Each iteration should optionally emit a compact event:

```json
{
  "iteration": 18291,
  "timestamp_ns": 9328112001,
  "policy": "deadline_aware",
  "runnable_prefill": 7,
  "runnable_decode": 12,
  "selected_sequences": 10,
  "prefill_tokens": 128,
  "decode_tokens": 9,
  "kv_pages_free": 41,
  "earliest_slack_ms": 74.2,
  "decision_us": 8.6
}
```

Sampling or ring-buffering is required in production mode to control overhead.

### Acceptance criteria

- Deterministic scheduler tests use a fake monotonic clock and mock backend.
- No starvation in bounded synthetic scenarios.
- Policy behavior matches golden traces.
- Scheduling overhead remains a small measured fraction of iteration time.
- Static policies can be selected from configuration without recompilation.

---

## 8.4 Backend Abstraction

Tokamak must not embed policy decisions inside a backend adapter.

### Conceptual interface

```cpp
struct BackendCapabilities {
    bool supports_continuous_batching;
    bool supports_speculative_verification;
    bool supports_prefix_sharing;
    bool supports_kv_export;
    std::size_t max_context_tokens;
};

class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;

    virtual BackendCapabilities capabilities() const = 0;
    virtual TokenizedPrompt tokenize(std::string_view text) = 0;
    virtual PrefillResult prefill(const PrefillBatch& batch) = 0;
    virtual DecodeResult decode(const DecodeBatch& batch) = 0;
    virtual VerifyResult verify(const VerificationBatch& batch) = 0;
    virtual void release(SequenceHandle sequence) noexcept = 0;
};
```

The concrete API may evolve, but the following boundaries must remain:

- request scheduling is owned by Tokamak;
- tensor/model execution is owned by the backend;
- capabilities are queried, not guessed;
- backend handles are RAII-managed;
- backend errors carry stable categories;
- tests can run without loading a real model.

### Required backends

#### Deterministic mock backend

The mock backend is mandatory and comes first. It supports:

- configurable prefill cost;
- configurable per-token decode cost;
- deterministic token output;
- simulated cache pressure;
- injected failures and stalls;
- configurable speculative acceptance patterns;
- a virtual clock mode for fast scheduler tests.

#### llama.cpp backend

Initial real-model backend. It should expose sufficient primitives for Tokamak to control request lifecycle and batching rather than forwarding requests to a separate server process.

#### Optional ONNX Runtime GenAI backend

Add only after the backend boundary is stable. Its purpose is to prove portability and compare orchestration behavior across execution engines, not to maximize feature count.

### Backend validation

- Fixed seeds and deterministic sampling where the backend permits it.
- Token outputs compared against backend-native reference execution.
- Capability tests run per backend.
- Unsupported features fail clearly at startup or request validation.

---

## 8.5 KV-Cache Policy Layer

Different backends expose different levels of cache control. Tokamak therefore separates the **logical cache policy** from physical backend storage.

### V1 responsibilities

- Account for cache consumption per sequence.
- Reserve capacity before scheduling work.
- Track reusable prompt prefixes.
- Select retention and eviction candidates.
- Prevent overcommit.
- Export page occupancy and fragmentation proxies.

### Logical page model

```text
CacheArena
  ├── Page 0  → sequence A, tokens 0..N
  ├── Page 1  → shared prefix P, refcount 3
  ├── Page 2  → sequence B, tokens N..2N
  ├── Page 3  → free
  └── ...
```

### Page metadata

- page identifier;
- owner sequence or shared-prefix identifier;
- logical token range;
- backend handle;
- reference count;
- last-access epoch;
- retention score;
- dirty or transferable state where applicable.

### Prefix cache key

A prefix entry should include more than token IDs. At minimum:

- model identity and revision;
- tokenizer identity;
- adapter identity;
- relevant generation or template configuration;
- token sequence hash;
- optional tenant salt when isolation is required.

Hash collisions must be checked against full token identity before reuse.

### Initial policies

- `none`: no prefix reuse;
- `lru`: least recently used prefix;
- `size_aware_lru`: recency adjusted by pages recovered;
- `cost_aware`: estimated recomputation cost adjusted by size and reuse probability.

A possible retention score is:

$$
S(p) =
\frac{
  \widehat{P}(\text{reuse}\mid p)
  \cdot \widehat{C}_{\text{recompute}}(p)
}{
  \text{pages}(p)
}
$$

Evict lower-scoring entries first. This is an experiment, not an assumed optimum.

### Fragmentation

If physical page control is available, report:

- allocated pages;
- free pages;
- partially used final pages;
- reserved but unused capacity;
- failed allocations despite nominal free capacity.

If the backend does not expose physical pages, label metrics as logical estimates.

### Stretch goals

- host-memory spill;
- asynchronous prefetch;
- prefix cache persistence;
- secure cache salting;
- NUMA-aware host cache;
- KV transfer between prefill and decode workers.

---

## 8.6 Speculative Decoding

### V1 algorithm

Use a smaller draft model to propose up to $$k$$ tokens. The target model verifies those tokens in one operation, accepting the longest valid prefix according to the chosen speculative sampling algorithm. If a proposal is rejected, sample the corrective token from the adjusted target distribution.

Correctness must not be reduced to greedy decoding unless the feature is explicitly limited to greedy mode. For stochastic decoding, implement or reuse a mathematically valid acceptance and correction procedure.

### Controller inputs

- draft/target model pair;
- prompt-length bucket;
- generated-length bucket;
- recent acceptance rate;
- accepted tokens per verification;
- sampling temperature;
- token entropy proxy if available;
- current queue depth;
- current decode batch width;
- draft and verification latency;
- workload class if supplied.

### Controller actions

Start with:

- disable speculation;
- enable with draft length 2;
- enable with draft length 4;
- enable with draft length 8.

Avoid an unbounded continuous action space in the first adaptive version.

### Rule-based baseline

For action $$a$$, estimate benefit:

$$\widehat{B}(a) = \widehat{T}_{\text{ordinary}} - \left(\widehat{T}_{\text{draft}}(a) + \widehat{T}_{\text{verify}}(a) + \widehat{T}_{\text{recovery}}(a)\right)$$

Enable action $$a$$ only when expected benefit exceeds a hysteresis margin. Hysteresis prevents rapid oscillation.

### Online-learning version

Use a contextual bandit only after the rule-based controller and replay simulator are stable.

Possible formulation:

- **context:** bucketed runtime and request features;
- **actions:** speculation modes and draft lengths;
- **reward:** accepted target-equivalent tokens per unit time, with SLO violation penalty;
- **exploration:** disabled or tightly bounded in production; enabled in benchmark mode;
- **fallback:** deterministic safe profile.

One possible reward is:

$$ R = \frac{\text{committed output tokens}}{\Delta t} - \lambda_1 \cdot \mathbf{1}[TTFT > SLO] - \lambda_2 \cdot \text{wasted draft tokens} $$

### Required metrics

- draft tokens proposed;
- draft tokens accepted;
- acceptance rate;
- accepted-token histogram by draft position;
- verification calls;
- draft latency;
- verification latency;
- correction latency;
- selected draft length;
- controller switches;
- speculative speedup versus paired baseline.

### Acceptance criteria

- Greedy speculative output exactly matches ordinary greedy output.
- Stochastic implementation passes distributional correctness tests or is explicitly deferred.
- Controller automatically disables speculation when measured benefit is negative.
- Benchmark reports include workloads where speculation loses.
- Draft and target tokenizer compatibility is checked at startup.

---

## 8.7 Adaptive Policy Engine

The adaptive engine selects named, versioned profiles rather than mutating arbitrary knobs invisibly.

### Example profiles

| Profile | Batch window | Prefill cap | Decode priority | Speculation |
|---|---:|---:|---|---|
| `latency` | 0–1 ms | Low | High | Conservative |
| `balanced` | 2–4 ms | Medium | Fair | Conditional |
| `throughput` | 5–10 ms | High | Batch width | Aggressive |
| `recovery` | 0 ms | Restricted | Deadline-first | Off |

Exact values are hardware- and model-specific and must be calibrated rather than hard-coded as universal defaults.

### State machine

```text
WARMUP
  │ enough observations
  ▼
BALANCED ───────────────▶ THROUGHPUT
  │ SLO pressure             │ queue falls / SLO pressure
  ▼                          ▼
LATENCY ◀──────────────── RECOVERY
```

### Safety rules

- Minimum dwell time per profile.
- Hysteresis around SLO thresholds.
- Hard limits override the learned policy.
- Unknown or stale model state falls back to `balanced`.
- Every transition records reason and observations.
- Production mode forbids uncontrolled exploration.
- Policy computation has a strict time budget.

### Objective

A configurable objective can be expressed as:

$$
J =
\text{output throughput}
- \lambda_1 \max(0, p95(TTFT)-SLO_{TTFT})
- \lambda_2 \cdot \text{peak memory}
- \lambda_3 \cdot \text{fairness penalty}
$$

For online operation, use robust rolling estimators rather than repeatedly computing full-distribution percentiles on the critical path.

---

## 8.8 Telemetry and Observability

### Metrics

Use low-cardinality Prometheus metrics. Never label a metric with request ID, prompt text, or arbitrary model input.

Suggested metrics:

```text
tokamak_requests_total{state,reason}
tokamak_requests_active{phase}
tokamak_queue_depth{queue}
tokamak_queue_time_seconds
tokamak_ttft_seconds
tokamak_inter_token_seconds
tokamak_request_latency_seconds
tokamak_tokens_total{kind}
tokamak_batch_size{phase}
tokamak_batch_tokens{phase}
tokamak_scheduler_iteration_seconds
tokamak_kv_pages{state}
tokamak_prefix_cache_requests_total{result}
tokamak_spec_draft_tokens_total{result}
tokamak_spec_verifications_total
tokamak_policy_transitions_total{from,to,reason}
tokamak_backend_errors_total{category}
tokamak_stream_backpressure_seconds
```

### Tracing

Trace spans should cover:

- HTTP parse and validation;
- admission;
- queue wait;
- tokenization;
- prefill;
- each decode or verification batch;
- token publication;
- backpressure;
- cleanup.

Per-token tracing is expensive and disabled by default. Use sampled requests or compact binary events.

### Structured logs

Logs are JSON in server mode. Required fields:

- timestamp;
- severity;
- component;
- event;
- request ID when appropriate;
- error category;
- model/backend identity;
- policy profile;
- duration.

Prompt and generated content are never logged by default.

### Benchmark provenance

Every benchmark artifact includes:

- Git commit and dirty-tree status;
- compiler and flags;
- build type;
- operating system and kernel;
- CPU model, core count, and affinity;
- memory size and NUMA topology;
- GPU model, driver, and runtime versions where applicable;
- backend commit/version;
- model identity, file hash, quantization, and context size;
- Tokamak configuration;
- random seeds;
- workload trace hash;
- thermal or power settings when known.

---

## 9. Public Configuration

Use a versioned YAML configuration file with command-line overrides.

```yaml
version: 1

server:
  host: 0.0.0.0
  port: 8080
  max_connections: 4096
  shutdown_grace_ms: 10000
  stream_backpressure_timeout_ms: 5000

model:
  backend: llama_cpp
  target: ./models/target.gguf
  draft: ./models/draft.gguf
  context_tokens: 8192
  seed: 42

admission:
  max_queued_requests: 512
  max_queued_prompt_tokens: 262144
  default_deadline_ms: 30000
  overload_policy: reject

scheduler:
  policy: adaptive
  max_sequences: 64
  max_batch_tokens: 2048
  max_prefill_tokens: 1024
  batch_window_us: 2000
  ttft_slo_ms: 300

cache:
  logical_page_tokens: 16
  max_bytes: 10GiB
  prefix_cache: true
  eviction_policy: cost_aware

speculation:
  enabled: true
  controller: rules
  allowed_draft_lengths: [2, 4, 8]
  min_dwell_iterations: 32
  benefit_hysteresis_us: 100

telemetry:
  metrics: true
  scheduler_trace_sample_rate: 0.01
  request_trace_sample_rate: 0.001
  log_level: info
  log_content: false
```

### Configuration requirements

- Invalid combinations fail before accepting traffic.
- Resolved configuration is printed with secrets and sensitive paths handled safely.
- Size and duration units are explicit.
- Environment-variable overrides are documented.
- Hot reload is a non-goal for V1.

---

## 10. Command-Line Interface

```text
tokamak serve          Start the inference server
tokamak benchmark      Run or replay a workload
tokamak inspect-model  Print backend and model metadata
tokamak validate       Validate a configuration
tokamak replay         Replay a scheduler trace against a policy
tokamak version        Print build and dependency versions
```

Examples:

```bash
tokamak validate --config configs/local.yaml

tokamak serve --config configs/local.yaml

tokamak benchmark \
  --target http://127.0.0.1:8080 \
  --workload workloads/mixed-chat.jsonl \
  --arrival poisson \
  --rate 12 \
  --duration 10m \
  --output results/run-001

tokamak replay \
  --trace traces/run-001.tktrace \
  --policy deadline_aware
```

---

## 11. Workload and Trace Formats

### Workload JSONL record

```json
{
  "id": "req-000001",
  "arrival_ms": 143.2,
  "messages": [
    {"role": "user", "content": "Explain lock-free queues briefly."}
  ],
  "max_tokens": 128,
  "temperature": 0.0,
  "priority": 1,
  "deadline_ms": 2000,
  "tenant": "benchmark-a"
}
```

### Dataset policy

- Include synthetic and real-shaped workloads.
- Do not publish private prompts.
- Store prompt hashes and token counts when content cannot be distributed.
- Version chat templates.
- Record whether output quality comparisons use identical sampling seeds.

### Arrival processes

The load generator should support:

- closed-loop fixed concurrency;
- constant-rate arrivals;
- Poisson arrivals;
- burst traces;
- recorded timestamp replay.

Closed-loop load alone is insufficient because it suppresses overload behavior by making clients wait before sending more work.

---

## 12. Benchmark Methodology

## 12.1 Experimental principles

- Change one independent variable at a time.
- Hold model, quantization, context, prompts, sampling, and hardware constant when comparing policies or engines.
- Warm up before measurement.
- Run multiple independent repetitions.
- Publish raw per-request records.
- Report medians and tail percentiles.
- Include uncertainty, such as bootstrap confidence intervals.
- Separate server time from client/network time where possible.
- Pin CPU affinity and record power mode.
- Avoid benchmarking during model download, compilation, or thermal transients.
- Report failures, rejections, and cancellations.

## 12.2 Required baselines

1. Backend-native server with its recommended configuration.
2. Tokamak FIFO without speculation.
3. Tokamak deadline-aware without speculation.
4. Tokamak static speculation.
5. Tokamak adaptive policy.
6. Optional second backend with equivalent model and settings where technically valid.

Comparisons against the backend-native server must acknowledge architectural differences and avoid claiming that adapter overhead is model-kernel superiority.

## 12.3 Benchmark matrix

### Concurrency or offered load

- 1, 4, 16, 64 concurrent clients;
- saturation sweep until SLO attainment collapses;
- at least one burst workload.

### Prompt/output shapes

| Workload | Input tokens | Output tokens | Purpose |
|---|---:|---:|---|
| Short chat | 32–128 | 32–128 | Interactive latency |
| Long-context QA | 2K–8K | 32–256 | Prefill pressure |
| Summarization | 1K–8K | 128–512 | Mixed phase cost |
| Code completion | 128–2K | 64–512 | Speculation opportunity |
| Mixed trace | Distribution | Distribution | Scheduler realism |

### Model configurations

At minimum:

- one target model that fits the available development hardware;
- one compatible smaller draft model;
- one non-speculative target-only run;
- one documented quantization level.

### Reported results

- TTFT p50, p95, p99;
- ITL p50, p95, p99;
- end-to-end latency p50, p95, p99;
- output tokens/s;
- requests/s;
- goodput;
- SLO attainment;
- queue depth over time;
- active sequences over time;
- rejection and cancellation rate;
- KV usage and cache hit rate;
- speculative acceptance and speedup;
- scheduler CPU overhead;
- peak resident memory;
- GPU memory and utilization when available;
- energy per output token as a stretch metric.

## 12.4 Performance claims

Every headline claim must include:

- baseline;
- workload;
- model and quantization;
- hardware;
- sample size;
- confidence interval or run-to-run variation;
- configuration files;
- raw result location.

Preferred language:

> On workload W and hardware H, adaptive policy P improved median goodput by X% over static profile B while maintaining at least Y% TTFT-SLO attainment across N independent runs.

Avoid:

> Tokamak is 3× faster.

---

## 13. Testing Strategy

## 13.1 Unit tests

Cover:

- request state transitions;
- cancellation idempotence;
- deadline calculations;
- queue ordering;
- batch constraints;
- fair-queue accounting;
- cache reference counts;
- prefix key validation;
- eviction scoring;
- speculative acceptance logic;
- policy hysteresis;
- metric aggregation;
- configuration parsing and validation.

## 13.2 Deterministic simulation tests

Use the mock backend and virtual clock to test complete scenarios in milliseconds rather than wall-clock minutes.

Scenarios:

- simultaneous arrivals;
- continuous short-request arrivals around one long request;
- cache exhaustion;
- slow streaming client;
- backend stall;
- cancellation during prefill;
- cancellation during decode;
- deadline expiry while queued;
- speculative acceptance collapse;
- profile oscillation pressure;
- shutdown with active requests.

## 13.3 Property and invariant tests

Useful properties:

- a request has one terminal state;
- allocated cache pages equal owned plus shared plus free accounting;
- no cache page has a negative reference count;
- selected batches never exceed configured limits;
- admitted deadlines do not move backward;
- completed output never exceeds `max_tokens`;
- greedy speculative output equals greedy target-only output.

## 13.4 Fuzzing

Fuzz targets:

- HTTP and JSON parsing boundary;
- configuration parser;
- SSE encoder;
- stop-sequence matcher;
- workload trace parser;
- cache metadata operations;
- scheduler event replay parser.

## 13.5 Concurrency testing

Use:

- ThreadSanitizer builds;
- deterministic stress tests;
- randomized cancellation and disconnect injection;
- repeated startup/shutdown loops;
- lock-order diagnostics where needed.

## 13.6 Sanitizers

CI or scheduled jobs should include:

- AddressSanitizer;
- UndefinedBehaviorSanitizer;
- ThreadSanitizer in a separate compatible build;
- LeakSanitizer where supported.

---

## 14. Reliability and Failure Handling

### Error categories

Use stable internal categories:

- invalid request;
- admission rejected;
- deadline exceeded;
- client cancelled;
- stream backpressure timeout;
- model unavailable;
- backend out of memory;
- backend invalid state;
- tokenizer failure;
- internal invariant violation;
- shutdown.

### Out-of-memory policy

- Reserve before scheduling where possible.
- Reject new work before corrupting active work.
- Attempt policy-defined cache eviction.
- Do not retry indefinitely.
- Record requested and available capacity.
- Move readiness to false if the backend becomes unrecoverable.

### Backend failure

- Fail affected requests explicitly.
- Quarantine or restart backend state only through a defined recovery path.
- Avoid process-wide termination for ordinary request errors.
- Crash on proven invariant corruption only when continuing would be unsafe.

### Graceful shutdown

1. Mark readiness false.
2. Stop accepting new requests.
3. Drain active requests up to grace period.
4. Cancel remaining requests.
5. Flush telemetry and benchmark records.
6. Release backend resources.
7. Exit with a meaningful status.

---

## 15. Security and Privacy

Tokamak is a personal project, but basic serving hygiene is required.

- Bound HTTP headers, body size, prompt tokens, and output tokens.
- Validate all integer conversions and allocation sizes.
- Disable prompt and output logging by default.
- Never use prompt text as a metric label.
- Support an authentication hook without making identity management a V1 goal.
- Treat model files and configuration as untrusted inputs at process boundaries.
- Document network exposure assumptions.
- Run as a non-root user in containers.
- Add dependency and container scanning.
- Define prefix-cache isolation semantics across tenants.
- Apply request IDs generated by the server when client IDs are absent or unsafe.

---

## 16. C++ Engineering Guidelines

### Language and build

- C++20 minimum; selectively use C++23 when toolchain support is documented.
- CMake presets for developer, release, sanitizer, and benchmark builds.
- Warnings enabled and treated as errors in project code.
- Dependencies pinned through a reproducible mechanism.
- Export `compile_commands.json`.

### Ownership

- Prefer value types and RAII.
- Use `std::unique_ptr` for exclusive polymorphic ownership.
- Use shared ownership only when the lifetime is genuinely shared.
- Use `std::span` and `std::string_view` for bounded non-owning access.
- Make async callback lifetimes explicit.
- Never retain views beyond owner lifetime.

### Memory

- Measure allocation hot spots before introducing custom allocators.
- Use arenas or `std::pmr` for request-scoped and batch-scoped allocations where beneficial.
- Avoid allocation per generated token.
- Align frequently accessed concurrent counters to avoid false sharing.
- Distinguish logical cache bytes from physical backend memory.

### Concurrency

- Prefer message passing or clearly owned state over broad shared mutable state.
- Document every lock and the state it protects.
- Never perform blocking backend work while holding scheduler locks.
- Establish and document lock ordering.
- Use lock-free structures only with a benchmark and a written memory-ordering argument.
- Separate network executors from model-execution threads.

### Error handling

- Use exceptions only according to a documented boundary policy.
- Do not allow exceptions to cross C ABI boundaries.
- Use typed result/error objects for expected runtime failures.
- Add context when propagating errors without losing category.

### Performance

- No optimization is accepted without before/after measurements.
- Benchmark debug-disabled release builds.
- Inspect generated assembly for custom kernels.
- Use `perf`, hardware counters, flamegraphs, and allocation profiles.
- Record compiler version and optimization flags.

---

## 17. Optional Hand-Written Kernel Track

Tokamak's orchestration is the main project. One focused low-level kernel provides additional depth without derailing delivery.

### Recommended kernel

Implement quantized matrix-vector multiplication for a small educational backend:

- scalar reference implementation;
- AVX2 implementation;
- optional AVX-512 implementation;
- optional ARM NEON implementation;
- grouped INT8 first;
- optional grouped INT4 after correctness and profiling.

### Requirements

- documented quantization layout;
- deterministic packing utility;
- tail handling for non-vector-width dimensions;
- numerical error tests against FP32;
- alignment and unaligned-input tests;
- microbenchmarks across matrix shapes;
- cycles, bandwidth, and effective operations per second;
- roofline-style interpretation;
- no claim that the kernel replaces the production backend.

### Why it exists

The kernel track demonstrates SIMD, data layout, cache behavior, quantization, and measurement. It should integrate into a tiny decoder or isolated benchmark, not block the inference-server milestones.

---

## 18. Repository Layout

```text
tokamak/
├── CMakeLists.txt
├── CMakePresets.json
├── LICENSE
├── README.md
├── project.md
├── cmake/
├── configs/
│   ├── local.example.yaml
│   ├── latency.yaml
│   ├── balanced.yaml
│   └── throughput.yaml
├── include/tokamak/
│   ├── admission/
│   ├── backend/
│   ├── cache/
│   ├── common/
│   ├── config/
│   ├── networking/
│   ├── policy/
│   ├── request/
│   ├── scheduler/
│   ├── speculation/
│   └── telemetry/
├── src/
│   ├── admission/
│   ├── backends/
│   │   ├── llama_cpp/
│   │   ├── mock/
│   │   └── onnx/
│   ├── cache/
│   ├── config/
│   ├── networking/
│   ├── policy/
│   ├── request/
│   ├── scheduler/
│   ├── speculation/
│   └── telemetry/
├── apps/
│   ├── tokamak/
│   ├── loadgen/
│   └── trace_viewer/
├── kernels/
│   ├── reference/
│   ├── avx2/
│   ├── avx512/
│   └── neon/
├── benchmarks/
│   ├── micro/
│   ├── server/
│   ├── workloads/
│   └── analysis/
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── simulation/
│   └── golden/
├── fuzz/
├── docs/
│   ├── architecture.md
│   ├── request-lifecycle.md
│   ├── scheduler.md
│   ├── cache.md
│   ├── speculative-decoding.md
│   ├── benchmarking.md
│   ├── threat-model.md
│   └── adr/
├── scripts/
├── third_party/
└── results/
    └── README.md
```

Large generated benchmark outputs and model weights must not be committed directly. Publish stable summaries and checksums; store bulky artifacts in release assets or external artifact storage.

---

## 19. Milestones

Each milestone ends with a demonstrable artifact. Later milestones must not be allowed to erase the evidence from earlier ones.

## Milestone 0 — Foundation

### Deliverables

- Repository and license.
- CMake presets.
- Formatting and static-analysis configuration.
- Unit-test framework.
- CI for Linux release and sanitizer builds.
- Build metadata endpoint or CLI output.
- Architecture decision record template.

### Exit criteria

- Clean checkout builds and tests using documented commands.
- CI produces reproducible binaries or build logs.
- Sanitizer smoke tests pass.

---

## Milestone 1 — Deterministic Runtime Skeleton

### Deliverables

- Request state machine.
- Mock backend.
- Virtual clock.
- FIFO scheduler.
- In-process request submission API.
- Deterministic simulation runner.
- Structured event trace.

### Demo

Replay 100 synthetic requests with configurable prefill/decode costs and render a textual timeline.

### Exit criteria

- Golden scheduler traces pass.
- Cancellation and deadlines pass deterministic tests.
- No real model or network required.

---

## Milestone 2 — Streaming Model Server

### Deliverables

- Async HTTP frontend.
- `/v1/completions` and `/v1/chat/completions`.
- SSE streaming.
- llama.cpp backend.
- Health, readiness, and metrics endpoints.
- Graceful shutdown.

### Demo

Run a local model and serve concurrent streaming clients.

### Exit criteria

- Output matches backend-native greedy generation for reference prompts.
- Disconnects release resources.
- Basic load test runs for 30 minutes without leaks or deadlock.

---

## Milestone 3 — Benchmark Laboratory

### Deliverables

- Standalone load generator.
- Closed-loop, constant-rate, Poisson, burst, and trace-replay modes.
- Per-request JSONL output.
- Provenance capture.
- Summary report generator.
- Backend-native baseline scripts.

### Demo

Publish the first benchmark report comparing backend-native serving with Tokamak FIFO at concurrency 1, 4, 16, and 64.

### Exit criteria

- Raw data can regenerate every chart and table.
- Runs are seeded and versioned.
- Tail latency, goodput, rejection rate, and memory are reported.

---

## Milestone 4 — Continuous Batching and Scheduling Policies

### Deliverables

- Runnable prefill/decode queues.
- Continuous batch builder.
- Round-robin, weighted fair, and deadline-aware policies.
- Queue and batch traces.
- Starvation and fairness tests.

### Demo

Use a mixed workload to show the throughput/TTFT trade-off among policies.

### Exit criteria

- No starvation in defined bounded tests.
- Scheduler overhead is measured.
- Policy selection requires configuration only.
- A sustained-load report identifies the saturation point.

---

## Milestone 5 — Cache Policy and Prefix Reuse

### Deliverables

- Logical page accounting.
- Capacity reservation.
- Prefix cache key and validation.
- LRU and cost-aware retention.
- Cache occupancy and hit metrics.
- Cache-pressure simulation.

### Demo

Replay repeated system-prompt workloads and compare no-cache, LRU, and cost-aware policies.

### Exit criteria

- Refcount and allocation invariants pass under fuzz/stress tests.
- Cache benefit is separated from changes in model or prompt.
- Tenant-isolation behavior is documented.

---

## Milestone 6 — Static Speculative Decoding

### Deliverables

- Draft-model integration.
- Verification path.
- Greedy correctness tests.
- Fixed draft lengths 2, 4, and 8.
- Acceptance and timing metrics.
- Startup compatibility validation.

### Demo

Benchmark code completion, chat, and long-form generation. Show both winning and losing cases.

### Exit criteria

- Greedy output parity passes.
- Speculative overhead is fully accounted for.
- Results include acceptance by draft position.

---

## Milestone 7 — Adaptive Controller

### Deliverables

- Rolling estimators.
- Named policy profiles.
- Hysteresis and dwell time.
- Rule-based speculation controller.
- SLO-aware scheduler profile selection.
- Offline trace replay for policy comparison.

### Demo

Run a workload that shifts from interactive traffic to throughput-heavy traffic and back. Show automatic profile transitions and SLO behavior.

### Exit criteria

- Adaptive policy beats at least one static baseline in goodput on the declared target workload.
- No claim is made if confidence intervals overlap materially.
- Every transition is explainable from recorded state.
- Safe fallback behavior is tested.

---

## Milestone 8 — Learned Policy Experiment

### Deliverables

- Contextual-bandit or small supervised policy prototype.
- Offline training/evaluation pipeline.
- ONNX export if a learned model is used.
- C++ inference for the controller.
- Distribution-shift and fallback tests.

### Demo

Compare rules, learned policy, and an oracle chosen by offline replay.

### Exit criteria

- Training and test traces are separated.
- The learned policy is compared against simple heuristics.
- Inference overhead is included.
- Production mode has bounded exploration or none.

This milestone is optional. A rigorous rules-based controller is more valuable than a decorative neural network.

---

## Milestone 9 — Low-Level Kernel Track

### Deliverables

- Reference and SIMD quantized GEMV.
- Packing format.
- Correctness suite.
- Microbenchmark report.
- Optional educational tiny backend integration.

### Exit criteria

- Numerical error and performance are reported across representative shapes.
- Results include hardware counters or bandwidth analysis.
- The optimization is justified by profiling.

---

## Milestone 10 — Release and Technical Narrative

### Deliverables

- `v1.0.0` release.
- Five-minute demo script.
- Architecture overview.
- Benchmark report.
- Performance tuning guide.
- Failure-mode guide.
- Short technical article.
- Roadmap and known limitations.

### Exit criteria

A new reviewer can, within 15 minutes:

1. understand the thesis;
2. build or run a container;
3. execute a mock benchmark without model weights;
4. see a real-model demo command;
5. inspect raw results supporting the headline claim;
6. identify what Tokamak owns versus what the backend owns.

---

## 20. Suggested 16-Week Execution Plan

| Weeks | Focus | Visible outcome |
|---|---|---|
| 1–2 | Foundation and mock runtime | Deterministic scheduler simulation |
| 3–4 | HTTP streaming and request lifecycle | Mock streaming server |
| 5–6 | llama.cpp backend | Real local model endpoint |
| 7–8 | Load generator and benchmark records | First baseline report |
| 9–10 | Continuous batching and scheduling | Policy comparison report |
| 11–12 | Cache accounting and prefix reuse | Cache experiment |
| 13–14 | Static speculative decoding | Acceptance/speedup report |
| 15 | Adaptive rules controller | Workload-shift demo |
| 16 | Hardening and release narrative | Public V1 release |

The learned controller and custom kernel are post-V1 unless schedule remains healthy.

---

## 21. Definition of Done for V1

V1 is complete when all of the following are true:

- [ ] C++ runtime serves a real open-weight model.
- [ ] OpenAI-compatible streaming works.
- [ ] Requests support cancellation and deadlines.
- [ ] Admission control bounds overload.
- [ ] Continuous batching is owned and controlled by Tokamak.
- [ ] FIFO and deadline-aware policies are implemented.
- [ ] Cache capacity is accounted for and cannot silently overcommit.
- [ ] Draft-model speculative decoding works in a documented mode.
- [ ] Adaptive rules can enable, disable, or resize speculation.
- [ ] TTFT, ITL, E2E, throughput, goodput, memory, and acceptance are reported.
- [ ] Mock-backend deterministic tests cover scheduler behavior.
- [ ] Sanitizer and fuzz targets run in CI or scheduled automation.
- [ ] A backend-native baseline is included.
- [ ] Raw benchmark records reproduce summary results.
- [ ] Known limitations and unsupported configurations are explicit.
- [ ] The headline claim is supported—or honestly reported as not supported.

---

## 22. Decision Log

Record important architectural choices in `docs/adr/`.

Initial ADRs:

1. Why Tokamak owns scheduling instead of proxying a backend-native server.
2. Why the deterministic mock backend is implemented first.
3. Why Boost.Asio/Beast is the initial transport.
4. Why adaptive actions are named profiles rather than arbitrary continuous knobs.
5. Why goodput is a headline metric.
6. Logical versus physical KV-cache ownership.
7. Speculative-decoding correctness scope for V1.
8. Exception and error-boundary policy.
9. Telemetry cardinality and content-privacy policy.
10. Reproducibility and benchmark provenance requirements.

---

## 23. Risks and Mitigations

| Risk | Consequence | Mitigation |
|---|---|---|
| Backend API does not expose enough batching control | Tokamak becomes a wrapper | Validate backend control surface in a spike before networking polish |
| Scope expands into a full tensor runtime | Project never ships | Keep backend boundary; restrict custom kernel to optional track |
| Speculative decoding is incorrect under sampling | Invalid outputs or biased distribution | Begin with greedy parity; add mathematically valid stochastic verification separately |
| Benchmarks are not comparable | Untrustworthy claims | Pin all variables and publish provenance/raw data |
| Adaptive policy oscillates | Tail-latency instability | Hysteresis, minimum dwell time, safe fallback |
| Telemetry distorts latency | Misleading measurements | Measure telemetry overhead and support sampled/disabled modes |
| Cache abstraction does not match backend reality | Fake low-level claim | Label logical accounting clearly and expose backend capabilities |
| GPU access is limited | Slow iteration | Mock backend, small models, CPU path, rentable benchmark runs only for release evidence |
| Networking consumes too much time | Core thesis delayed | Start with established library and postpone `io_uring` |
| Lock-free ambitions introduce bugs | Reliability loss | Begin with bounded mutex/queue designs; optimize only from profiles |
| Learned controller adds no value | Decorative GenAI | Make it optional and compare against simple rules |

---

## 24. Demo Story

The final demo should tell one coherent story.

### Scene 1 — The workload

Start Tokamak with a 300 ms p95 TTFT target. Begin a low-rate interactive workload.

### Scene 2 — Load rises

Introduce a burst of long prompts. Show queue depth, batch width, and TTFT pressure increasing.

### Scene 3 — Runtime adapts

Tokamak switches from `balanced` to `latency` or `recovery`, reduces batching delay, and disables unhelpful speculation.

### Scene 4 — Workload becomes generation-heavy

As long-running decode requests dominate and acceptance improves, Tokamak selects a speculative profile and increases draft length.

### Scene 5 — Evidence

Show:

- policy transition reasons;
- p95 TTFT relative to the SLO;
- goodput versus static profiles;
- speculative acceptance;
- raw benchmark metadata;
- one scheduler trace explaining a slow request.

The demo should emphasize decisions and evidence, not a chat UI.

---

## 25. README-Level Positioning

Suggested short description:

> Tokamak is an SLO-aware LLM serving runtime written in C++. It combines continuous batching, deadline-aware scheduling, cache policy, and adaptive speculative decoding behind an OpenAI-compatible streaming API, with a reproducible benchmark laboratory built in.

Suggested headline benchmark format:

> On `<hardware>`, serving `<model>` under `<workload>`, Tokamak's adaptive policy achieved `<result>` compared with `<baseline>` while maintaining `<SLO attainment>` for a `<TTFT target>`.

Do not fill this sentence until the benchmark exists.

---

## 26. Potential Research Questions

The implementation should enable concrete experiments:

1. At what offered load does a batching window stop helping TTFT-adjusted goodput?
2. Which prompt/output distributions favor deadline-aware scheduling over FIFO?
3. Can recent token acceptance predict whether speculative decoding will help the next verification window?
4. Does dynamic draft length outperform one globally tuned fixed length?
5. How much scheduler sophistication is justified before scheduler CPU overhead becomes visible?
6. Which prefix-retention score works best under repeated system prompts and bounded cache?
7. How quickly can an adaptive controller respond to workload shifts without oscillating?
8. Does optimizing average throughput reduce fairness or p99 latency?
9. How portable are learned policy decisions across models, quantization levels, and hardware?
10. When does slow-client backpressure become a material serving bottleneck?

Each research question should map to a reproducible workload and a falsifiable result.

---

## 27. Future Work

Only after a strong single-host V1:

- prefill/decode disaggregation;
- multi-GPU scheduling;
- remote KV-cache transfer;
- LoRA-aware batching;
- grammar-constrained generation;
- multimodal request scheduling;
- persistent prefix cache;
- host-memory cache offload;
- power-aware policy selection;
- NUMA-aware CPU inference;
- eBPF-based network and scheduling diagnostics;
- pluggable policy SDK;
- distributed trace replay;
- model routing across quality and latency tiers;
- fault-tolerant worker processes.

---

## 28. First Ten Issues

Create these issues immediately:

1. **Bootstrap CMake presets and CI**
2. **Define request state machine and invariants**
3. **Implement fake monotonic clock**
4. **Implement deterministic mock backend**
5. **Implement FIFO scheduler and golden trace test**
6. **Define structured scheduler-event format**
7. **Build in-process simulation CLI**
8. **Specify backend capability contract**
9. **Spike llama.cpp direct-library integration**
10. **Write benchmark provenance schema**

Issue 9 is a technical risk spike. Resolve it before committing to the final scheduler/backend interface.

---

## 29. Immediate Next Step

Do not start with HTTP, CUDA, a dashboard, or a learned policy.

Build this first:

```text
JSONL workload
      │
      ▼
virtual clock → admission → FIFO scheduler → mock backend
      │                                      │
      └──────────── scheduler trace ◀────────┘
```

The first executable should run a complete deterministic workload and print:

- each request's state transitions;
- queue, prefill, decode, and completion times;
- batch composition per iteration;
- TTFT and E2E distributions;
- throughput and goodput;
- invariant-check result.

Once this foundation is correct, substitute real time, real networking, and a real backend one boundary at a time.

That sequence keeps Tokamak a systems project with a testable core instead of becoming a pile of asynchronous code wrapped around someone else's inference loop.
