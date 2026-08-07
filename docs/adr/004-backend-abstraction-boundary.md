# ADR-004: Backend abstraction boundary

## Status
Accepted

## Date
2026-08-08

## Context

Tokamak's architecture separates **scheduling/orchestration** (owned by Tokamak) from
**tensor/model execution** (owned by the backend). project.md Section 8.4 requires a
narrow backend interface that:

- lets Tokamak control request lifecycle and batching rather than forwarding requests
  to a separate server process;
- ensures capabilities are queried, not guessed;
- ensures backend errors carry stable, categorized error types;
- ensures tests can run without loading a real model;
- keeps backend handles RAII-managed with explicit release semantics.

Multiple backend implementations are expected over the project's lifetime (MockBackend
for deterministic testing, llama.cpp for real model serving, optional ONNX Runtime for
portability comparison). All must satisfy the same narrow contract so that scheduling,
admission, and policy code never depend on backend-specific behavior.

### Key design tensions resolved

**1. Capabilities are queried, not guessed.**

Different backends support different features (continuous batching, prefix sharing, KV
export, speculative verification). Rather than compiling these assumptions into
scheduler code or relying on runtime trial-and-error, the interface requires every
backend to declare its capabilities at construction time via `capabilities()`. Callers
check before relying — this prevents silent misbehavior when a less-capable backend is
substituted, and avoids the alternative ("try the call, catch the error") which
conflates "not supported" with "transiently failed."

Concrete example caught in review: MockBackend initially reported
`supports_prefix_sharing = true` despite implementing no prefix-sharing logic
whatsoever. A future caller checking this capability would have legitimately attempted
prefix reuse, received silently wrong/no-op behavior, and had no signal anything was
wrong. The fix was trivial (set to `false`), but the bug demonstrated exactly the
failure mode this principle exists to prevent — capabilities must reflect what the
implementation *actually does*, not what sounds reasonable or aspirational.

**2. Errors carry stable categories, not string messages.**

`BackendErrorCategory` (kInvalidRequest, kOutOfMemory, kInvalidState, kUnavailable)
maps directly onto project.md Section 14's error-category list. These categories are
stable across backend implementations, usable as metric labels (unlike free-form
messages), and machine-actionable (admission can distinguish "reject because backend
is OOM" from "reject because request itself is invalid"). Human-readable messages
accompany categories for logs but never appear in metric labels or API responses
(project.md Section 15, Section 8.8).

**3. Invariant violations vs. expected backend failures are distinct.**

Per ADR-002, an invalid `SequenceHandle` (unknown or already-released) passed to
`decode()`/`release()` is an invariant violation (`panic()`), not a `BackendError`.
The reasoning: `SequenceHandle` is opaque and only mintable via `prefill()` — an
invalid handle can only originate from a bug in Tokamak's own bookkeeping
(use-after-release, mixed backend instances), never from client input or backend-side
failure. `BackendError` is reserved for conditions the *backend itself* reports about
its own state — conceptually different from "our own code violated its own contract."

**4. Per-sequence batch-result granularity.**

`PrefillResult`/`DecodeResult` contain one `std::expected<Outcome, BackendError>` per
input sequence, not a single success/failure for the whole batch. This matches the
fundamental property of continuous batching: sequences in a batch are independent, and
one sequence's failure (e.g., OOM for its specific KV allocation) must not fail the
other 63 sequences that are running fine. The type system enforces this isolation at the
API boundary rather than relying on implementers to remember it.

**5. verify() is deferred, not omitted permanently.**

project.md Section 8.4's conceptual interface includes `verify()` for speculative
decoding. We defer it to Milestone 6 (when speculative decoding is actually
implemented) rather than declaring a stub now, because: (a) the batch types for
verification are meaningfully different from prefill/decode and would be speculative
without a real consumer to validate them against, and (b) project.md explicitly says
"the concrete API may evolve." Adding it later is a backwards-compatible extension
(new virtual method + new types), not a breaking change.

**6. MockBackend-specific test hooks stay outside the shared interface.**

`configure_eos()` (setting a per-sequence "finish after N tokens" trigger) exists only
on `MockBackend`, not on `InferenceBackend`. Real backends don't accept externally-
forced EOS positions — EOS emerges from the model's own weights and sampling. Putting
test-only knobs in the shared interface would pollute it with concepts every future
backend implementer must see, evaluate, and either stub or misimplement. Test hooks
belong on the concrete test type.

**7. MockBackend models real cost asymmetry via FakeClock::advance().**

MockBackend advances the shared `FakeClock` by `per_token_prefill_cost × num_tokens`
during prefill (cost scales with work) and by a fixed `per_decode_cost` per decode()
call regardless of batch width (modeling memory-bandwidth-bound decode where batch
amortization is the key throughput mechanism). This ensures scheduler tests observe
the same cost-structure incentives that a real GPU backend would produce, without
requiring actual hardware or wall-clock time.

## Decision

Define `InferenceBackend` as a pure-abstract C++ interface with five methods:
`capabilities()`, `tokenize()`, `prefill()`, `decode()`, `release()`. All batch
results use per-sequence `std::expected` for partial-failure isolation.
`SequenceHandle` is opaque and non-fabricable outside `prefill()`. Backend errors use
a stable `BackendErrorCategory` enum. `verify()` is deferred to Milestone 6.
`MockBackend` implements the interface with deterministic behavior, clock-advancing
cost simulation, and test-only hooks (`configure_eos`) that do not appear in the
shared interface.

## Consequences

- All scheduling, admission, and policy code depends only on `InferenceBackend`, never
  on a concrete backend type. Backend substitution (mock → llama.cpp → ONNX) requires
  no scheduler changes.
- Deterministic simulation tests (project.md §13.2) run in microseconds of wall-clock
  time by using MockBackend + FakeClock, with no model weights, no GPU, and no network.
- Future backends (llama.cpp, ONNX Runtime) must implement the same five-method
  contract and declare their capabilities honestly.
- Per-sequence error isolation in batch results means the scheduler can fail/retry
  individual requests without aborting an entire batch iteration.
- Adding `verify()` in Milestone 6 is a backwards-compatible interface extension.
- `SequenceHandle` validity is an invariant enforced by `panic()`, consistent with
  ADR-002's boundary: callers must not pass invalid handles, and doing so indicates a
  bug in Tokamak's own code.
