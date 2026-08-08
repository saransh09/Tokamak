# ADR-007: FIFO scheduler tick semantics and backend handle lifecycle

## Status
Accepted

## Date
2026-08-08

## Context

The FIFO scheduler is Milestone 1's remaining core deliverable, sitting on top of the
already-built `Request`/`RequestLifecycle` (state machine) and `InferenceBackend`/
`MockBackend` (compute execution) modules. Before writing its interface, two contract
questions surfaced that are not answered by any existing ADR and are foundational
enough to lock in before code exists, rather than deciding them ad hoc inside an
implementation:

1. Within a single scheduling iteration ("tick"), does a request that finishes prefill
   also decode in that same tick, or does it wait for the next tick's decode phase?
2. When `decode()` reports a `BackendError` for a specific sequence (a real,
   backend-reported failure — distinct from an invalid-handle `panic()`), is the
   scheduler still obligated to call `release()` for that sequence's handle?

Neither `backend.h` nor ADR-004 (which defined the backend abstraction boundary)
addressed either question. This ADR resolves both, informed by how production
continuous-batching schedulers (vLLM, and the broader pattern shared with TGI and
TensorRT-LLM) are actually built — documented in detail in `learning/learnings_005.md`.

This ADR is intentionally a **new, standalone record** rather than an amendment to
ADR-004: these are decisions made *along the way*, discovered while designing the
scheduler, not corrections to a mistake in the original backend-boundary design.
ADR-004 remains historically accurate to what was decided at the time it was written;
this ADR extends the backend contract with additional invariants the scheduler must
uphold.

## Decision

### 1. Same-tick prefill-to-decode promotion

Within one `tick()`, the prefill phase and decode phase run in this order:

1. **Prefill phase**: call `prefill()` once for every request currently waiting.
   Every successful outcome mints a `SequenceHandle` and immediately advances that
   request through `WaitingPrefill → Prefilling → WaitingDecode → Decoding` — fully
   within this phase, before the decode phase begins.
2. **Decode phase**: call `decode()` once for every request now in `Decoding` —
   both those promoted in this tick's prefill phase and those already decoding from
   prior ticks — as a single batch.

`WaitingDecode` remains a real, meaningful state in `RequestState` (matching project.md
§7's lifecycle diagram, useful for tracing/observability — a request's current state
is always inspectable), but under this scheduling policy a request never actually rests
in `WaitingDecode` across a tick boundary; it is promoted out of it within the same
tick it entered.

**Rationale**: real continuous-batching schedulers are built around minimizing wasted
iterations. Deferring a ready-to-decode sequence to the next tick for no structural
reason directly and needlessly inflates TTFT — project.md's own headline latency
metric (§5). This costs nothing in backend-call count: exactly one `prefill()` call and
one `decode()` call still happen per tick, matching the flat-per-call decode cost
established in ADR-004/learnings_004.

### 2. Unconditional release() on every retirement path

The scheduler must call `release(handle)` exactly once for every `SequenceHandle` it
ever obtains from a successful `prefill()` outcome, on **every** path by which that
request's sequence stops being actively tracked — successful completion, a
`BackendError` reported by `decode()`, cancellation, or deadline expiry. No retirement
path is exempt.

This amends/clarifies `InferenceBackend`'s contract (originally specified in
`backend.h` and ADR-004): a `BackendError` returned by `decode()` describes a failed
compute step for that sequence, but does **not** imply the backend has already freed
that sequence's resources. Resource cleanup (`release()`) and compute-result reporting
(`BackendError`) are orthogonal concerns; a caller must never infer one from the other.

**Rationale**: real backend resource managers (KV-cache/context allocators) are freed
by an explicit, separate call, never implicitly as a side effect of a failed compute
step. Assuming otherwise risks either a resource leak (if the assumption is "already
freed" but it wasn't) or a double-free (if something else also calls `release()`
believing it hadn't been called yet). The universal, symmetric invariant — every
minted handle gets exactly one `release()` call, unconditionally — has no such failure
mode.

## Consequences

- `FifoScheduler::tick()`'s implementation must structure its prefill and decode
  phases as two sequential, same-tick steps, not two independently-triggered halves
  that might run a tick apart.
- Every code path in the scheduler that retires a request (marks it `Completed`,
  `Failed`, or `Cancelled`) must call `release()` on that request's `SequenceHandle`
  before dropping its reference to it — including the deadline-expiry and
  cancellation paths, not just the "ran out of output tokens" success path.
- `MockBackend`'s existing behavior already satisfies this: it never implicitly frees a
  `SequenceState` entry except via an explicit `release()` call, so no changes to
  `mock_backend.{h,cpp}` are required by this ADR. Future real backends (llama.cpp,
  Milestone 2+) must honor the same symmetry.
- The Milestone 1 FIFO scheduler still does not implement preemption, batch-size
  bounding, or chunked prefill — these remain explicitly deferred (see
  `learning/learnings_005.md` §3, §4, §6 for why, and what problem each would solve
  when introduced).
- This ADR's decisions apply to the FIFO baseline specifically; later scheduling
  policies (round-robin, weighted-fair, deadline-aware, adaptive) may revisit
  same-tick-promotion behavior (e.g., a policy might deliberately delay promotion to
  enforce fairness), but the unconditional-`release()` invariant applies universally to
  every policy, since it is a backend-contract requirement, not a scheduling-policy
  choice.
- **Deadline expiry inherits the same intermediate-state requirement as
  `BackendError` handling.** `is_valid_transition()` has no `WaitingPrefill → Failed`
  edge (only `WaitingPrefill → Prefilling` or `WaitingPrefill → Cancelled`), so a
  request whose deadline has already expired *while still waiting* must first
  transition to `Prefilling` before `retire()` can move it to `Failed` — the exact
  same stopover the prefill-failure path already needed. This was implemented once in
  the prefill-failure branch and initially missed in `expire_deadlines()`, only
  surfacing via a targeted test (deadline expired before the request had ever been
  picked up for prefill). The general rule this generalizes to: **every call site that
  invokes `retire()` must independently re-verify its source state has a legal direct
  edge to every terminal state it might target** — fixing the gap in one call site does
  not imply the same gap is closed in another, even when both call the same shared
  `retire()` helper.
- **Consequence of the fix above, worth naming explicitly**: an expired-while-waiting
  request now ends up nominally recorded as having passed through `Prefilling` on its
  way to `Failed`, even though the entire point of checking deadlines before doing any
  backend work (this ADR's own ordering rule) is to guarantee `backend_.prefill()` is
  *never actually called* for it. `RequestLifecycle` state describes bookkeeping
  shape — "this request's transition history satisfies `is_valid_transition()`" — not
  "this request caused a corresponding real backend call." These are related but
  distinct claims, and `retire()`'s design as a single, universal release chokepoint
  means satisfying the state machine's shape occasionally requires a transition that,
  read literally, overstates what actually happened on the backend side.
