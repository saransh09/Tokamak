# ADR-001: Time access is abstracted behind a Clock interface

## Status
Accepted

## Date
2026-08-07

## Context

Tokamak's request lifecycle depends heavily on elapsed time: deadlines, TTFT, ITL, E2E,
and scheduling slack (project.md Section 5, Section 7) all require measuring durations
and comparing timestamps.

`std::chrono::system_clock` ("wall clock") is unsuitable for any of this: it tracks
calendar time and the OS is permitted to adjust it (NTP sync, manual changes, leap
seconds), meaning it can jump backward mid-process. Using it for duration measurement
can silently produce negative durations, corrupting deadline checks, latency
histograms, and scheduling decisions.

`std::chrono::steady_clock` ("monotonic clock") solves the correctness problem — it
only ever moves forward while the process runs. However, calling
`std::chrono::steady_clock::now()` directly, scattered throughout scheduler and
lifecycle code, creates a testability problem: deterministic tests for
time-dependent behavior (deadline expiry, queue-time thresholds, dwell-time hysteresis)
would otherwise require real wall-clock sleeps, making the test suite slow and prone to
scheduling-jitter flakiness -- unacceptable given project.md Section 13.2's requirement
to "test complete scenarios in milliseconds rather than wall-clock minutes."

### Options considered

**A. Call `std::chrono::steady_clock::now()` directly wherever time is needed.**
Simplest, but makes time a hidden global dependency with no way to substitute a
deterministic source in tests.

**B. Inject an abstract `Clock` interface, with `SystemClock` (production) and
`FakeClock` (deterministic, test/simulation) implementations.** Standard dependency-
injection pattern used by systems such as gRPC, Envoy, and Seastar specifically to keep
timer/scheduler logic testable without real delays.

## Decision

Option B. All time-dependent Tokamak code depends on the abstract `tokamak::Clock`
interface (`now()` only), never on `std::chrono::steady_clock` directly. Production
wiring uses `SystemClock`; tests and the deterministic simulation runner use
`FakeClock`, which only exposes `advance(Duration)` (non-negative) rather than an
arbitrary `set(TimePoint)`, preserving the same "never goes backward once running"
invariant the real clock guarantees. `FakeClock`'s constructor accepts an initial
`TimePoint`, mirroring the one legitimate case where a real clock's value is chosen
arbitrarily: process/simulation startup.

`Clock`/`SystemClock`/`FakeClock` reuse `std::chrono::steady_clock`'s `TimePoint` and
`Duration` types directly rather than reinventing time representations -- the wrapper
adds only substitutability, not a replacement type system.

## Consequences

- Every subsystem that needs time (request lifecycle, admission deadlines, scheduler
  slack calculations, adaptive-policy dwell timers) takes a `const Clock&` dependency
  rather than calling a static clock function.
- Deterministic simulation and unit tests can exercise time-dependent behavior (deadline
  expiry, TTFT/E2E computation, hysteresis) in microseconds of wall-clock test time by
  calling `FakeClock::advance()`.
- `RequestLifecycle` (and, transitively, `Request`) hold `Clock` as a reference member,
  which has downstream implications for container storage -- see ADR-003.
- Real distributed-system clock concerns (NTP uncertainty bounds, logical/vector clocks)
  are explicitly out of scope: Tokamak V1 is single-process, single-host (project.md
  Section 3).
