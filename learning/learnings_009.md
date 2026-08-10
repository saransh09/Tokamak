# Learnings 009 — Designing the HTTP Frontend: Coroutines, Thread Boundaries, and Honest Admission

## Questions

1. What actually happens when you write `co_await some_async_op(...)` inside
   an Asio coroutine — what is Asio doing underneath that keyword?
2. Why can't the engine thread just call `scheduler.tick()` directly from
   inside an Asio coroutine, given Asio already runs on its own thread(s)?
3. Why is the per-request output channel bounded, and why does a full channel
   become the request's problem (backpressure/cancellation) instead of the
   engine's problem (blocking)?
4. Why does ADR-011 implement only the capacity-only admission policy and
   explicitly defer the deadline-aware one, when project.md lists deadline
   awareness as a real Section 8.2 responsibility?
5. ADR-011 claims "exactly one lock" in the whole design — is that actually
   true, and what would make it stop being true?

---

## 1. Coroutines: a mental model for `co_await`

A C++20 coroutine is a normal-looking function whose body can *suspend* and
later *resume* — from the caller's point of view, it doesn't block a thread
while suspended; the thread goes off and does other work, and comes back
later to continue exactly where it left off.

Concretely, for an Asio coroutine written as `asio::awaitable<T>
handle_connection(tcp::socket socket)`:

```cpp
asio::awaitable<void> handle_connection(tcp::socket socket) {
    std::string buffer;
    std::size_t n = co_await socket.async_read(
        asio::buffer(buffer), asio::use_awaitable);
    // ... more co_awaits ...
}
```

What `co_await socket.async_read(..., asio::use_awaitable)` actually does:

1. `async_read` is called with a special *completion token*
   (`asio::use_awaitable`) instead of a callback. Asio recognizes this token
   and, instead of invoking a callback when the read completes, arranges to
   **resume this coroutine** at that point.
2. The compiler-generated coroutine machinery packages up everything the
   function needs to resume later (local variables, the current suspension
   point) into a heap-allocated coroutine frame — this is the "state that
   would otherwise be a closure" from ADR-011's Decision 2, but the compiler
   manages it instead of you writing a class by hand.
3. Control returns immediately to whoever called `handle_connection` — most
   likely `asio::co_spawn`, which registered this coroutine with the
   `io_context`. The calling thread is now free; it can run other coroutines,
   accept new connections, whatever else is queued on that executor.
4. Later, when the OS reports the socket read has data, Asio's `io_context`
   event loop notices, and resumes the coroutine exactly at the `co_await` —
   `n` gets assigned the byte count, and execution continues to the next
   line, on whichever thread happens to be running the `io_context` at that
   moment (which is why Asio code must not assume "the same thread that
   suspended is the one that resumes").
5. If the read fails (error, disconnect), `async_read` with `use_awaitable`
   throws an exception *at the `co_await` expression* — so `try { ... }
   catch (const boost::system::system_error& e) { ... }` around a sequence of
   `co_await`s catches errors from any of them, exactly like synchronous
   code. This is precisely why ADR-011 picked coroutines over callbacks: the
   error path doesn't need a separate callback parameter or an `if (ec)`
   check after every single call.

The key shift in thinking, coming from synchronous C++: a coroutine's
"thread of execution" is not tied to one OS thread for its whole lifetime.
Between suspension points, it might run on thread A; after resuming, it
might run on thread B. Any state you touch across a `co_await` must be safe
under that assumption — which is exactly why ADR-011's Decision 3 draws an
explicit, narrow boundary (one mutex-guarded queue) for the one place state
actually crosses between the network executor and the engine thread, instead
of letting coroutines reach into scheduler state directly and hoping it
works out.

**Takeaway**: `co_await` is not "wait for this synchronously" — it's "suspend
here, let the executor do other work, and resume me (possibly on a different
thread) when this operation's completion token fires." The mental model that
makes this tractable is treating suspension points as the only places
where "which thread am I on" can change — write everything between them as
if it were ordinary sequential code, because it is.

---

## 2. Why the engine thread can't just be "another Asio coroutine"

Asio's `io_context` is built around the assumption that work scheduled on it
either completes quickly or itself suspends (via `co_await`) so other queued
work can run. `FifoScheduler::tick()` — especially once it's calling a real
`llama.cpp` backend's `prefill()`/`decode()` — does neither: it's a single,
synchronous C++ function call that blocks the calling thread for the full
duration of a model forward pass (potentially 10s–100s of ms), with no
`co_await` inside it Asio could use to interleave other work.

If `tick()` ran directly inside a coroutine on the same `io_context` that
also handles network I/O, every other connection sharing that `io_context`'s
thread(s) would stall for the duration of every single tick — new
connections wouldn't be accepted, other clients' SSE frames wouldn't be
written, health checks would hang. This is exactly Section 16's stated rule
("Never perform blocking backend work while holding scheduler locks" /
"Separate network executors from model-execution threads") and exactly why
ADR-011 puts the tick loop on its own dedicated thread, entirely outside
Asio's executor, talking to the network side only through the two explicit
queues (submission in, tokens out).

**Takeaway**: coroutines solve "don't block while waiting for I/O you don't
control the timing of." They do not solve "don't block while doing real,
synchronous CPU/GPU work you fully control." Those are different problems
with different fixes — the first is a scheduling/ergonomics problem Asio's
executor solves for you; the second requires actually moving the blocking
work to a thread that isn't shared with anything latency-sensitive.

---

## 3. Bounded channels turn "the client is slow" into an explicit, attributable cost

A tempting simpler design: let the engine push tokens into an *unbounded*
queue per request, and let the network side drain it whenever the client is
ready to receive. This looks harmless — nothing ever blocks — but it hides a
real failure mode: a client that stops reading (browser tab backgrounded,
bad network) turns an unbounded queue into unbounded memory growth, one
request at a time, exactly the "unbounded growth" Section 8.1's acceptance
criteria explicitly rules out for connection state in general.

Making the channel *bounded* forces the design to answer a question an
unbounded queue lets you dodge: what happens when it's full? ADR-011's
answer is deliberately not "block the engine thread" (that would let one
slow client stall scheduling for every other request, the same class of
problem as Learning 2) and not "silently drop tokens" (a request completing
with output missing would violate every invariant `check_invariants()` was
built to enforce in Milestone 1). Instead: track how long the channel has
*been* full as its own measured quantity, separate from model time, and
after a configured timeout, cancel the request — converting an ambiguous
"slow client" situation into an explicit, bounded, measured, and ultimately
terminal outcome, consistent with Section 14's "fail affected requests
explicitly."

**Takeaway**: an unbounded buffer at a producer/consumer boundary doesn't
eliminate backpressure — it just defers the failure from "an explicit,
attributable timeout" to "an unattributable OOM sometime later," and trades
a design decision you can reason about now for an incident you'll have to
debug later.

---

## 4. Deferring deadline-aware admission is an honesty decision, not a laziness shortcut

It would be easy to write a deadline-aware admission check right now — some
formula like `estimated_wait = queue_depth * avg_prefill_cost;  if
(estimated_wait + estimated_decode_cost > deadline) reject();` — and it would
compile, pass a hand-built unit test with invented numbers, and *look* like
Section 8.2's second responsibility is done.

The problem is where `avg_prefill_cost` and `estimated_decode_cost` would
come from today: either hardcoded guesses (which are just wrong for
whatever backend eventually runs — the mock backend's costs are picked for
scheduler-test convenience, not representative serving numbers, and
llama.cpp's real costs don't exist as measured data until its own
integration work happens) or a made-up "online estimator" with no real
traffic to calibrate against yet. A policy built on invented numbers doesn't
fail loudly — it fails *quietly*, by admitting requests that will miss their
deadline anyway or rejecting ones that would have made it, while looking
complete on the surface. That's strictly worse than a policy that's honestly
simple: a hard capacity limit says exactly what it does ("no more than N
in-flight") with no pretense of predicting the future.

This is the same discipline ADR-008 already applied to the scheduler trace
schema (omit `kv_pages_free`/`earliest_slack_ms` rather than stub them at
`0`) — a different subsystem, same underlying principle: don't build the
version of a feature that requires data you don't have yet just to check a
box early; build the honest subset now, and the fuller version once the data
that would make it *real* actually exists (here: after Milestone 2's
llama.cpp integration produces calibrated per-token costs, and likely not
until Milestone 4 when deadline-aware scheduling needs the same data
anyway).

**Takeaway**: "looks done" and "is done" can diverge specifically when a
feature's correctness depends on data that doesn't exist yet — filling that
gap with a plausible-looking guess is worse than leaving the gap visible,
because a visible gap gets fixed later with real data, while a
plausible-looking guess quietly becomes load-bearing.

---

## 5. Stating "exactly one lock" in a design doc is a testable claim, not just documentation

ADR-011 asserts the entire two-thread design has exactly one piece of shared
mutable state requiring a lock: the submission queue. That's a strong claim
worth pressure-testing before implementation, because Section 16 requires
documenting every lock and the state it protects — which only stays *true*
documentation if no second lock quietly appears during implementation.

Candidates that could break the claim, and why ADR-011's design (as
written) avoids each:

- **The per-request output channel** — looks like shared state (engine
  writes, network coroutine reads), but is scoped to a bounded SPSC channel
  per request, not a general mutex-guarded structure; SPSC channels can be
  implemented lock-free (or with a lock scoped so tightly it's not "shared
  mutable state" in the sense Section 16 means — a single-slot handoff, not
  a shared map every thread touches).
- **The `shutting_down` flag** — shared between the signal handler, the
  network side, and the engine thread's tick loop, but it's a single atomic
  boolean read/written independently, not data requiring a mutex to keep
  multiple fields consistent with each other.
- **`FifoScheduler` itself** — not shared at all under this design; it's
  owned exclusively by the engine thread and never touched from a network
  coroutine directly, which is precisely the point of routing everything
  through the submission queue instead.

If implementation later needs, say, a shared metrics counter map touched
from both domains, or the admission controller needs to read scheduler state
directly instead of going through the queue, that would be a *second* lock
— and ADR-011's "exactly one lock" claim would need an explicit update, not
a silent one, the same way ADR-008 flagged that its own workload schema
would need "a second, explicit revision" as new fields gained real meaning.

**Takeaway**: naming an invariant precisely ("exactly one lock, protecting
exactly this queue") is more useful than a vague one ("mostly
single-threaded, with some synchronization") specifically because a precise
claim can be checked against the actual implementation later and caught if
it drifts — a vague claim can absorb any amount of new shared state without
ever technically becoming false.

---

## Summary: what this session's design work established, beyond ADR-011's decisions themselves

| Concern | What we learned |
|---|---|
| `co_await`'s actual mechanics | Suspends the coroutine at that point, frees the calling thread for other work, and resumes (possibly on a different thread) when the operation's completion token fires — exceptions propagate through `co_await` the same as synchronous calls |
| What coroutines solve vs. don't | They solve "don't block a thread while waiting on I/O you don't control the timing of" (network reads/writes); they do not solve "don't block while doing real synchronous CPU/GPU work" — that needs a genuinely separate thread, not a coroutine |
| Why the output channel is bounded | An unbounded producer/consumer queue doesn't remove backpressure, it just converts it from an explicit, measured, attributable timeout now into an unattributable memory-growth incident later |
| Deferring deadline-aware admission | A feature that depends on data you don't have yet (calibrated per-token costs) is more honestly represented by its simpler subset (hard capacity limit) than by a version built on invented numbers that looks complete but is quietly wrong |
| "Exactly one lock" as a checkable claim | Precise invariant statements in a design doc are valuable specifically because they can be verified against the real implementation and caught drifting — vague statements can't be falsified, which makes them useless as a check |
