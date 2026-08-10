# Learnings 011 — Engine Thread: Two-Domain Concurrency in Practice

**Date**: 2026-08-10
**Piece**: Milestone 2, Piece B (Engine thread + submission queue)
**Related**: ADR-011 (HTTP Frontend Architecture), learnings_010 (TokenChannel)

---

## 1. Realising "exactly one lock" in practice

ADR-011 Decision 3 claims two execution domains bridged by exactly one
mutex-guarded submission queue. Piece B is where that claim becomes real
code. The submission mutex guards two vectors (`pending_submissions_`,
`pending_cancels_`) and nothing else. The engine thread drains them via
swap-under-lock, then operates on scheduler and active-request state
without any lock held.

The proof that this works is structural, not probabilistic: the scheduler
is never touched from two threads. The engine thread is the sole caller
of `scheduler_.tick()`, `scheduler_.submit()`, `scheduler_.cancel()`.
Network-side code touches only the guarded vectors. No data race is
possible because no data is shared beyond those two vectors — and access
to them is serialised by the mutex.

---

## 2. Lock scope discipline: "hold the lock only for the swap"

During review we caught `drain_submissions()` holding the submission
mutex while calling `scheduler_.submit()` and inserting into `active_`.
This isn't a correctness bug (only the engine thread calls drain), but
it violates the mechanical rule: acquire, swap, release, then iterate.

Why this matters even when "safe":

- It establishes a pattern. A future contributor might add a second lock
  and introduce a lock-ordering problem against this unnecessarily-held
  first lock.
- It increases the window during which `submit()` from the network thread
  would block on the mutex, adding latency to an otherwise non-blocking
  call.
- It makes reasoning harder. "Is the scheduler ever called under lock?"
  should have a trivially-verifiable answer of "no."

The correct pattern (demonstrated in `drain_cancels()`):

```cpp
std::vector<T> batch;
{
  std::lock_guard lock(mutex_);
  batch.swap(pending_);
}
// Lock released — iterate freely
for (auto &item : batch) { ... }
```

---

## 3. Backpressure as wall-clock policy, not channel responsibility

A key separation: `TokenChannel` knows whether it is full (structural
fact), but the Engine decides what *consequence* fullness has (policy).
The channel's `try_push()` returns false; the Engine records *when* this
happened and compares against a configurable timeout.

### Design decision: wall-clock backpressure timing

Backpressure uses `std::chrono::steady_clock::now()` (real wall time),
not the virtual `Clock` abstraction that the scheduler and request
lifecycle use.

**Rationale**: Backpressure measures whether a *real network consumer* is
stalled — reading SSE events too slowly, or disconnected without a clean
close. This is fundamentally a wall-clock phenomenon. A `FakeClock` that
advances only during simulated prefill/decode steps would never detect a
consumer that stopped reading, because the fake clock only ticks when
the model does work.

**Consequence for testing**: Tests that exercise backpressure must use a
real sleep (`std::this_thread::sleep_for`) to let wall time elapse, and
must use a very short timeout (`0ms`) so the test completes quickly. This
is acceptable because we're testing the timeout *mechanism*, not a
specific production timeout value.

**Alternative considered**: Use `Clock::now()` for backpressure. Rejected
because it would make backpressure undetectable in the simulation runner
(Milestone 1's `FakeClock` never advances unless the engine is actively
decoding — a stalled consumer wouldn't trigger any clock advance).

---

## 4. The "diff cursor" pattern for token delivery

The engine doesn't receive a callback when a new token is emitted. Instead,
each `ActiveEntry` stores `last_seen_tokens` — a high-water mark of how
many tokens have been pushed to the channel. On each tick, `push_tokens()`
compares this against `request->output_tokens_emitted()`.

Why polling beats notification here:

- The producer (scheduler calling `emit_token()`) runs on the same thread
  as the consumer (the engine's `push_tokens()`). There's no thread to
  wake — it's the same loop iteration.
- The scheduler has no knowledge of channels or the Engine. Adding a
  callback into `emit_token()` would violate the dependency direction
  (Request depends on nothing above Clock; Engine depends on everything).
- The diff is O(new_tokens_this_tick), which is bounded by
  `max_batch_size × 1` — one token per request per decode step.

The cursor also naturally handles the case where `try_push()` fails: we
simply don't advance `last_seen_tokens`, and the next tick retries from
the same position.

---

## 5. `emit_token(uint32_t token_id)` — breaking change rationale

Original signature: `void emit_token()` — increments a counter.
New signature: `void emit_token(uint32_t token_id)` — increments counter
*and* stores the token ID.

This change was forced by a simple requirement: `TokenEvent` carries a
`token_id` field. The Engine needs to know *which* token was generated,
not just *how many*. The information was already available at the call
site (`outcome.value().token_id` in `fifo_scheduler.cpp`) — we just
weren't capturing it.

The accessor `output_token_ids()` exposes the stored sequence so
`push_tokens()` can index into it by position. This is how the diff
cursor pattern (Section 4) actually gets the data it needs.

Impact: one production call site (`fifo_scheduler.cpp`), one test file
(`request_test.cpp`). Both updated in the same change.

---

## 6. `-Wunused-private-field` and the "store for later" antipattern

Initial implementation stored `InferenceBackend &backend_` in Engine,
reasoning that "we might need it later." The compiler immediately flagged
it — `FifoScheduler` holds its own reference, and no Engine method uses
the backend directly.

Lesson: don't store references "for later." If no method in the class
uses a member, it shouldn't exist. The compiler enforces this under
`-Werror`, which is exactly the kind of discipline §16's strict warning
flags are meant to provide. If a future piece needs direct backend access
from Engine, it can add the member at that point — with a concrete use
site justifying its presence.

---

## 7. Testing threaded code without sleeps

The Engine tests use `tick_interval = 0ms`, making the engine thread spin
continuously rather than sleeping between iterations. This means:

- `channel.pop()` (which blocks via condition variable) acts as the
  natural synchronization point between the test thread and the engine
  thread. No explicit sleep needed.
- The engine processes submissions as fast as it can drain them, so tokens
  appear in the channel almost immediately.
- Test determinism comes from the *blocking pop* on the consumer side, not
  from timing assumptions about how fast the engine runs.

The one exception is the backpressure test, which deliberately uses
`std::this_thread::sleep_for(50ms)` to let the engine thread cycle
through enough iterations to detect and act on backpressure. This is
acceptable because:

1. The timeout is `0ms`, so any single iteration that observes fullness
   will trigger cancellation.
2. The 50ms sleep is generous (the engine is spinning with 0ms tick
   interval, so it will cycle thousands of times in 50ms).
3. The assertion is loose (`events.size() <= 2`) to accommodate timing
   variance.

---

## Summary of reusable patterns from Piece B

| Pattern | Where applied |
|---------|--------------|
| Swap-under-lock, iterate-after-release | `drain_submissions()`, `drain_cancels()` |
| Diff cursor (poll high-water mark) | `push_tokens()` / `last_seen_tokens` |
| Collect-then-mutate (avoid iterator invalidation) | `check_backpressure()`, `reap_terminal()` |
| CAS for idempotent start/stop | `Engine::start()`, `Engine::stop()` |
| Spin-with-zero-interval for test determinism | All engine tests except backpressure |
