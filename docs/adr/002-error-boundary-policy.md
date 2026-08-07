# ADR-002: Invariant violations panic; expected failures use typed results

## Status
Accepted

## Date
2026-08-07

## Context

project.md Section 16 requires a documented boundary between exception use, typed
result/error objects, and other failure-handling mechanisms, and explicitly lists this
as a required decision record (Section 22, item 8). Section 14 further requires that
"proven invariant corruption" crash the process "when continuing would be unsafe,"
while ordinary request-level errors (bad input, admission rejection, deadline exceeded,
backend errors) must be handled as explicit, recoverable, per-request outcomes that do
not take down the server.

Two categories of failure exist in Tokamak's request lifecycle code so far:

1. **Expected runtime conditions**: a client sends an invalid request, admission
   rejects work due to capacity, a deadline expires. These are normal, anticipated
   outcomes of serving real traffic and must not crash the process. In a typical web
   framework, this is where an exception-and-catch or an HTTP error response fits.

2. **Invariant violations**: an illegal request-state transition is attempted (e.g.
   something tries to move a `Completed` request back to `Decoding`), or
   `emit_token()` is called after `max_output_tokens` has already been reached. These
   indicate a bug in Tokamak's own scheduling/lifecycle logic, not a problem with the
   client's request. Because requests share mutable resources with other concurrent
   requests (KV-cache pages, scheduler queues, batch slots -- see
   `learning/learnings_002.md`), silently continuing past a corrupted invariant risks
   leaking or double-freeing shared resources, or emitting a token after a terminal
   state has already been published (an explicit prohibition in project.md Section 7).

### Options considered for category 2 (invariant violations)

**A. `assert()`.** Free in release builds (compiled out under `NDEBUG`), which is
exactly the problem: it silently disables the safety net in the build type most likely
to be running under real, sustained concurrent load where these bugs are most likely to
manifest.

**B. Throw a C++ exception.** Recoverable by a caller, which is the wrong semantic here
-- there is no meaningful way to "recover" from a corrupted state machine at the call
site; the corruption already happened. Exceptions are appropriate for *expected*,
*recoverable* failures (category 1), not for signaling that internal invariants have
already been broken.

**C. A dedicated `panic()` that unconditionally logs and calls `std::abort()`,
regardless of build type.** Guarantees the process cannot silently continue past
detected corruption, at the cost of taking down the whole process (acceptable per
project.md Section 14's explicit allowance to "crash on proven invariant corruption
only when continuing would be unsafe").

## Decision

- **Invariant violations** (illegal state transitions, exceeding `max_output_tokens`,
  and similar internal-logic-corruption conditions) call `tokamak::panic()`
  (`include/tokamak/common/panic.h`), which unconditionally logs to stderr and calls
  `std::abort()` in every build type. This is a deliberate, always-on safety net, not a
  debug-only assertion.
- **Expected runtime failures** (invalid client input, admission rejection, deadline
  exceeded, backend errors, cancellation) must use typed result/error objects (or
  request-level state transitions to `Rejected`/`Failed`/`Cancelled`) rather than
  exceptions or `panic()`. This category is not yet implemented in code as of this ADR
  (admission control and backend error categories arrive in later milestones) but the
  boundary is fixed now: these paths must never call `panic()`, and `panic()` must never
  be used for a condition a client can trigger through ordinary, even if invalid,
  input.
- C++ exceptions are not used for either category at this stage. If a specific
  future subsystem has a well-motivated need for exceptions (e.g. a third-party
  dependency's API surface), that decision should be scoped and justified separately
  rather than adopted implicitly.

## Consequences

- Any code path that can be triggered purely by external client input (malformed
  request, oversized prompt, expired deadline) must be validated *before* it can reach
  a `panic()` call; a `panic()` firing in production should always indicate a Tokamak
  bug, never a client mistake.
- Tests that would trigger `panic()` (e.g. attempting an illegal transition or exceeding
  a token budget) cannot assert on it directly via the test framework's normal
  pass/fail mechanism, since `std::abort()` terminates the whole test binary. Such
  tests instead exercise the pure decision predicates (e.g. `is_valid_transition()`)
  directly, and treat the enforcement wrapper (`transition_to()`) as trusted-by-code-
  review for the abort path itself.
- As typed error/result types are introduced for admission and backend failures in
  later milestones, they must be reviewed against this ADR to confirm they fall under
  "expected failure" (typed result) rather than "invariant violation" (`panic()`).
