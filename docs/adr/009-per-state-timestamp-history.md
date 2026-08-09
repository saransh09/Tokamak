# ADR-009: Per-state timestamp history in RequestLifecycle

## Status
Accepted

## Date
2026-08-09

## Context

project.md Section 29 requires the Milestone 1 simulation runner to print, per
request: "queue, prefill, decode, and completion times" — four distinct
duration breakdowns, not just the two `RequestLifecycle` currently supports
(TTFT via `first_token_at() - admitted_at()`, E2E via
`completed_at() - admitted_at()`).

`RequestLifecycle::transition_to()` overwrites a single `entered_at_` field on
every transition and only special-cases two states (`kAdmitted`, `kCompleted`)
to preserve their timestamps permanently via `admitted_at_`/`completed_at_`.
Every other state's entry time is lost the moment the request moves on — there
is currently no way to recover when a request left `kWaitingPrefill` (needed
for queue time) or when it left `kPrefilling` (needed for prefill time).

Two paths were considered while planning the simulation runner (piece C of the
Milestone 1 pipeline, see ADR-008):

1. Ship the runner with only TTFT/E2E, and document the queue/prefill/decode
   sub-breakdowns as an explicitly absent, deferred feature — consistent with
   ADR-008 Decision 2's "absent, not stubbed" precedent for `kv_pages_free`
   and friends.
2. Extend `RequestLifecycle` to record every state's entry timestamp, not just
   `kAdmitted`/`kCompleted`, so the runner can compute the four durations
   project.md actually asks for.

Before choosing, the actual size and risk of option 2 was measured rather than
assumed: every call site of `transition_to()` (all within `FifoScheduler`) only
*drives* the state machine and never reads `entered_at()`/`admitted_at()`/etc.,
and no existing test asserts on the current single-`entered_at_` behavior in a
way that a purely additive change would break. The change turned out to be
small — see Decision below — which changed the recommendation from option 1 to
option 2.

## Decision

Add a `std::unordered_map<RequestState, TimePoint> state_entered_at_` member to
`RequestLifecycle`, populated by one new line inside `transition_to()`:

```cpp
void RequestLifecycle::transition_to(RequestState to) {
  ...
  state_ = to;
  entered_at_ = clock_.now();
  state_entered_at_[to] = entered_at_;
  ...
}
```

Expose it via a new accessor:

```cpp
std::optional<TimePoint> entered_at(RequestState state) const;
```

This generalizes the existing special-case pattern (`kAdmitted` -> `admitted_at_`,
`kCompleted` -> `completed_at_`) into a single mechanism covering every state,
rather than adding a third and fourth hand-rolled field for `kWaitingPrefill`
and `kPrefilling`. `admitted_at()`/`first_token_at()`/`completed_at()` are kept
as-is (unchanged signatures, unchanged semantics) since they are the most
frequently used milestones and already have dedicated call sites and tests;
`entered_at(RequestState)` is additive, not a replacement.

The runner (piece C) computes the four durations project.md asks for as:

```cpp
auto queue_time   = *lifecycle.entered_at(kPrefilling)    - *lifecycle.entered_at(kWaitingPrefill);
auto prefill_time = *lifecycle.entered_at(kWaitingDecode) - *lifecycle.entered_at(kPrefilling);
auto decode_time  = *lifecycle.entered_at(kCompleted)     - *lifecycle.entered_at(kDecoding);
auto e2e          = *lifecycle.completed_at()             - *lifecycle.admitted_at();
```

Note a request that fails or is cancelled mid-flight may never reach some of
these states, so callers must check the `optional`s rather than assume every
duration is always computable — same discipline already required of
`admitted_at()`/`first_token_at()`/`completed_at()` today.

## Alternatives considered

- **Full ordered history** (`std::vector<std::pair<RequestState, TimePoint>>`
  recording every transition in order, including repeats/cancellations): more
  general, but nothing in Milestone 1 needs transition *order* or *repeat
  visits* to a state — a request visits each non-terminal state at most once
  on any single path through the state machine (per `is_valid_transition()`'s
  DAG-shaped graph). An unordered map keyed by state is the smallest structure
  that answers "when did this request enter state X," which is all that is
  actually asked for. If a future need arises for full transition history
  (e.g. replaying exact scheduler decisions, or detecting state re-entry after
  a design change adds cycles to the graph), this can be revisited then.
- **Ship without this (option 1 above)**: rejected once the actual size of
  option 2 was measured (see Context) — the cost was assumed to be larger than
  it is. Deferring a ~15-line additive change to avoid a footnote in the
  runner's summary output was not a good trade once the real cost was known.

## Consequences

- `RequestLifecycle` grows one new private member and one new public accessor.
  No existing accessor, call site, or test changes behavior — this is purely
  additive.
- `sizeof(RequestLifecycle)` grows by the size of an
  `unordered_map<RequestState, TimePoint>` (a handful of small heap
  allocations per request, at most one entry per non-terminal state visited).
  Not a concern at Milestone 1's scale (deterministic simulation with at most
  a few thousand synthetic requests); worth revisiting only if
  `RequestLifecycle`'s per-request memory footprint ever becomes a measured
  bottleneck at production scale.
- The simulation runner (ADR-008 piece C) can now print exactly the four
  durations project.md Section 29 asks for, with no deferred-gap footnote.
- A new test should be added to `request_state_test.cpp` asserting
  `entered_at(RequestState)` returns the correct per-state timestamp across a
  full happy-path transition sequence, and returns `std::nullopt` for a state
  never visited (e.g. `kFailed` on a request that completed successfully).
