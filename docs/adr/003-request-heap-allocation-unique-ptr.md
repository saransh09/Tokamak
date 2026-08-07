# ADR-003: Request objects are heap-allocated and held via std::unique_ptr

## Status

Accepted

## Date

2026-08-07

## Context

The `Request` domain object owns a `RequestLifecycle`, which holds a `const Clock&`
(reference) to an injected monotonic clock. References in C++ cannot be rebound after
construction, which means any class containing a reference member loses its
copy-assignment and move-assignment operators.

The FIFO scheduler (Milestone 1) and all subsequent scheduling policies need to store
collections of in-flight requests and perform operations like removing completed
requests from the middle of a queue, reordering by priority or deadline slack, and
iterating over runnable subsets. Standard container operations (`std::vector::erase`,
`std::sort`, swap-based removal) rely on move-assignment to shuffle elements, which is
unavailable for types containing reference members.

Two options were evaluated:

### Option A: Store the clock as a raw pointer internally

Switch `RequestLifecycle`'s internal `const Clock& clock_` to `const Clock* clock_`,
while keeping the public constructor API as `const Clock&` (preventing null at
construction). This restores move-assignability, allowing `Request` to be stored
directly by value in `std::vector<Request>` or similar containers.

**Pros:**
- Simpler container types (plain `std::vector<Request>`).
- Defers the "how does the scheduler store things" decision.

**Cons:**
- A pointer that is never null is a weaker compile-time signal than a reference.
- `Request` objects would become copyable/movable by value, but they represent
  identity-bearing, long-lived entities with unique IDs and lifecycle state — copying
  them is semantically nonsensical, and allowing it invites bugs.
- As `Request` grows (backend handles, KV-cache page references, output buffers), value
  semantics become increasingly expensive and inappropriate.

### Option B: Heap-allocate requests, hold via `std::unique_ptr<Request>`

Keep `RequestLifecycle`'s clock as a reference. The scheduler stores requests as
`std::unique_ptr<Request>` in its queues. Container operations only move pointers
(trivial, O(1)), never the `Request` object itself — so the non-assignability
restriction never applies.

**Pros:**
- Matches project.md Section 16 guidance: "Use `std::unique_ptr` for exclusive
  polymorphic ownership."
- `Request` is an identity-bearing, resource-owning entity — it should not be
  accidentally copied or value-shuffled. `unique_ptr` makes this structurally
  impossible rather than relying on convention.
- Moving a pointer is trivially cheap regardless of how large `Request` becomes.
- Pointers to requests remain stable (no invalidation on container resize), which
  simplifies cross-referencing from other subsystems (cache pages, backend handles,
  scheduler indices) that need to point back at a specific request.
- The reference member in `RequestLifecycle` continues to express "always valid, never
  null" at the type level — no ambiguity, no defensive null checks.

**Cons:**
- One heap allocation per request (amortizable via arena/pool allocators later if
  profiling shows it matters — per project.md Section 16: "Measure allocation hot spots
  before introducing custom allocators").
- Slightly more indirection when accessing request fields (pointer dereference).

## Decision

**Option B.** The scheduler and all other subsystems that store collections of requests
will hold them as `std::unique_ptr<Request>`. `Request` objects are constructed on the
heap, exclusively owned by one container at a time, and never copied or assigned.

## Consequences

- `RequestLifecycle` retains `const Clock&` as its clock storage — no change needed.
- Scheduler queues will be typed as e.g. `std::vector<std::unique_ptr<Request>>` or
  `std::deque<std::unique_ptr<Request>>`.
- Transfer of ownership between subsystems (e.g., admission → scheduler queue →
  active batch) is expressed via `std::move` of the `unique_ptr`, making ownership
  transfer explicit and compiler-checked.
- If heap allocation overhead becomes measurable in profiles (unlikely given that
  request lifetimes are milliseconds-to-seconds, not microseconds), a pool allocator
  can be introduced without changing the `unique_ptr`-based API.
