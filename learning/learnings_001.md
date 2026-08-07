# Learnings 001 — Clocks, Time, and Why Tokamak Wraps `std::chrono`

## Questions

1. What is this clock stuff — what does time actually mean to a computer?
2. What does this entail in distributed systems?
3. Why do we have a `FakeClock`?
4. What is the resemblance to a real clock?
5. Why do we wrap around `std::chrono` instead of using it directly?
6. Comprehension check: Why does `FakeClock` not have a `set(TimePoint)` method to jump
   to an arbitrary time? What invariant would that break, and is there a legitimate
   reason we might still want to jump the clock (hint: process startup)?

---

## 1. What does "time" even mean to a computer?

A computer doesn't experience time the way we do. It has hardware counters (a crystal
oscillator ticking at a known frequency), and the OS exposes those counters through
different APIs, each with different guarantees. Two matter for us:

- **Wall clock** (`std::chrono::system_clock`, POSIX `gettimeofday`) — tracks *calendar
  time*: "it is currently 2:45:03pm, Aug 7 2026." Its whole purpose is to match reality,
  so the OS is allowed — and expected — to **adjust it**: NTP sync corrects drift, leap
  seconds get inserted, a user/admin can change the system date, VMs get corrected after
  being paused. This means wall clock can jump **backward**, or leap **forward**
  unexpectedly, from your program's perspective.

- **Monotonic/steady clock** (`std::chrono::steady_clock`, POSIX `CLOCK_MONOTONIC`) —
  tracks *elapsed time since some arbitrary, unspecified reference point* (often boot
  time, but deliberately unspecified — you're not supposed to care). Its only guarantee:
  **it never goes backward while the process is running.** It says nothing about
  matching the calendar.

### Why this distinction matters, concretely

Imagine measuring "how long did this request take" using wall clock:

```
start = wall_clock.now()   // 2:45:03.000pm
... NTP silently corrects the clock backward by 2 seconds ...
end = wall_clock.now()     // 2:45:01.500pm
duration = end - start      // NEGATIVE 1.5 seconds!!
```

Negative durations corrupt every downstream computation: histograms, deadline checks,
"time remaining" math. This is a classic, real production bug class, not a hypothetical.

**Rule:** use wall clock only for displaying/logging a timestamp a human reads. Use
monotonic clock for anything you compute with — durations, deadlines, timeouts, "has
this been queued too long." `project.md` states this explicitly (Section 7: *"Deadlines
use a monotonic clock"*) precisely because this bug class is so common in serving
systems.

---

## 2. The distributed-systems angle

Tokamak is explicitly single-process/single-host (`project.md` Section 3, Non-goals),
so this is context, not a direct requirement — but it's worth understanding why "clock
design" is a serious systems topic.

In a **single process**, monotonic clock solves the problem completely — one clock, one
ordering, done.

In a **distributed system** (multiple machines), monotonic clock stops being enough,
because *each machine has its own independent monotonic clock* — machine A's "elapsed
since boot" has zero relationship to machine B's. You can't compare timestamps from A
and B and get a meaningful ordering, even though both clocks are individually
well-behaved.

Tools distributed systems use instead:

- **NTP-synced wall clocks with bounded uncertainty** (Google's TrueTime, used in
  Spanner) — instead of pretending clocks are perfectly synced, TrueTime reports time as
  an *interval* `[earliest, latest]`, and the uncertainty bound is used explicitly in
  correctness proofs.
- **Logical clocks** (Lamport timestamps) — abandon real time entirely; assign each
  event a counter that only needs to respect causality ("if A happened-before B, A's
  counter < B's counter"), not real elapsed time.
- **Vector clocks / Hybrid Logical Clocks** — richer versions of the same idea, used in
  systems like Cassandra, CockroachDB.

Getting clock semantics wrong in a distributed system doesn't just produce bad latency
numbers — it can break correctness proofs entirely (a real cause of historical outages,
e.g., clock-skew bugs in HBase/Cassandra-style systems).

---

## 3 & 4. Why Tokamak needs a clock *abstraction*, and its resemblance to the real clock

Two reasons, both grounded in `project.md`:

**(a) Correctness** — deadlines, TTFT, ITL, E2E all need monotonic time, for the reason
in section 1.

**(b) Testability** — the bigger reason for the *abstraction itself*, not just "use
monotonic clock." Consider: how do you write a fast, deterministic unit test for "a
request whose deadline is 30 seconds away gets rejected once 31 seconds of queue time
have passed"? If scheduler code calls `steady_clock::now()` directly, the test must
either:

- actually sleep ~31 real seconds (slow, especially across hundreds of such tests), or
- accept that timing-sensitive assertions might occasionally flake due to OS scheduling
  jitter (CI machine under load, thread preemption, etc.)

Neither is acceptable for the deterministic test suite `project.md` demands (Section
13.2: *"Use the mock backend and virtual clock to test complete scenarios in
milliseconds rather than wall-clock minutes"*).

The fix is a standard design pattern: **dependency injection**. Instead of code reaching
out and grabbing a global time source, we pass a `Clock&` into whatever needs time, as a
collaborator. Production wires in `SystemClock`. Tests wire in `FakeClock`. The scheduler
code itself doesn't know or care which one it's talking to — it just calls
`clock.now()`.

This is why `Clock` is an abstract base class with `now()` as its only method — the
minimum contract needed by everything downstream. `FakeClock`'s resemblance to the real
clock is intentional: it satisfies the exact same interface and the exact same
"never goes backward while running" invariant, so any code written against `Clock`
behaves identically regardless of which implementation it's handed. This pattern is
standard in serious C++ systems code — gRPC, Envoy, and Seastar all inject a clock
interface rather than calling OS time functions directly, specifically so their
schedulers/timers can be tested in milliseconds instead of real wall-clock minutes.

---

## 5. What does wrapping `std::chrono` actually buy us?

`std::chrono` itself is already a big improvement over raw integers (like
`int64_t millis_since_epoch`), because its types encode *units* and *clock identity* at
compile time:

- `TimePoint` (a `steady_clock::time_point`) represents "a specific instant." The type
  system **will not let you add two `TimePoint`s together** (nonsensical — "2:45pm +
  2:46pm" means nothing). You can only subtract two `TimePoint`s to get a `Duration`, or
  add a `Duration` to a `TimePoint` to get a new `TimePoint`. This is enforced by the
  compiler, not convention — a whole category of "I added two timestamps by accident"
  bugs simply cannot compile.
- `Duration` carries its unit in the type (`milliseconds`, `microseconds`, etc.), so you
  can't accidentally treat "500" as milliseconds in one place and microseconds in
  another without an explicit (and safe) conversion.

So why wrap it further in our own `Clock` / `SystemClock` / `FakeClock`? Because
`std::chrono::steady_clock::now()` is a **static function** — you can't substitute it,
mock it, or inject a fake version of it. It's baked into the call site. Our `Clock`
interface exists purely to make time **swappable at the call site** via polymorphism,
which `std::chrono` alone doesn't offer. We're not replacing `std::chrono`'s type
safety — we're reusing it (`TimePoint`/`Duration` *are* `std::chrono` types) while adding
the one capability it lacks: substitutability.

---

## 6. Comprehension check — why no `FakeClock::set(TimePoint)`?

**The invariant being protected:** the real `steady_clock` guarantees "never decreases
while the process runs." If `FakeClock` allowed `set()` to jump to *any* value
(including backward, or even just "arbitrarily," which could accidentally go backward
via a caller bug), it would let test code create scenarios that **cannot happen with the
real clock** — meaning a test could pass or fail based on a scenario that's physically
impossible in production. That defeats the entire purpose of the fake: it needs to be
*at least as constrained* as production, ideally more so, never less.

Only `advance(Duration)` with a non-negative duration is exposed, which makes "going
backward" a compile-time-visible mistake (you'd have to explicitly pass a negative
duration — and an `assert` catches that at runtime too).

There's a subtler readability benefit too: forcing test code to say `advance(50ms)`
instead of `set(t + 50ms)` makes tests read as relative stories ("50ms passes, then...")
rather than requiring the reader to track absolute magic numbers.

**The process-startup nuance:** the real clock *is* allowed to start at an arbitrary,
meaningless value at boot — this is literally in the C++ standard (`steady_clock`'s
epoch is unspecified). What it's *not* allowed to do is move backward **once your
process has already observed a value from it**. Our `FakeClock` already supports the
"arbitrary start" case — that's exactly why the constructor takes an optional `start`
parameter:

```cpp
FakeClock clock(some_specific_time_point);  // fine — this is the "boot" moment
clock.advance(50ms);                          // fine — time moves forward
// clock.set(earlier_time);                   // NOT fine — never happens on real hardware
```

So the rule isn't "the clock's value can never be chosen" — it's "once running, it can
only move forward." The constructor covers the legitimate startup case; the lack of
`set()` afterward protects the monotonic invariant during the simulated run. If we ever
needed to model something like "resume a trace replay starting at wall-clock time X"
(e.g., loading a saved workload trace and continuing from where it left off), that's
still just a fresh `FakeClock(X)` constructed once at the start of that run — never a
mid-run jump.
</content>
