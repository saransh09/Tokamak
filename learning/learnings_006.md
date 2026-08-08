# Learnings 006 — FIFO Scheduler Implementation: Hands-On Lessons

## Questions

1. Why did the same state-machine bug (missing `WaitingPrefill → Failed` edge) get
   fixed once and still slip through in a second place?
2. What does it actually mean, philosophically, for a request to be marked
   `Prefilling` when no real `prefill()` call ever happened for it?
3. Why did calling `retire()` while iterating the very container it erases from keep
   coming back as a bug, and why did the fix differ between two call sites?
4. Why did `handles_[req->id()] = ...` fail to *compile*, rather than misbehave at
   runtime — and why is that the better outcome?
5. What did adding failure-injection to `MockBackend` teach us about testing an
   "unconditional invariant"?

---

## 1. Fixing a bug once does not mean the same bug is gone everywhere

`prefill_phase()`'s failure branch needed an explicit `transition_to(kPrefilling)`
before `retire(*req, kFailed)`, because `is_valid_transition()` has no direct
`kWaitingPrefill → kFailed` edge — only `kWaitingPrefill → kPrefilling` or
`kWaitingPrefill → kCancelled`. That fix was written and reviewed correctly.

`expire_deadlines()` calls `retire(*req, RequestState::kFailed)` too, on requests
pulled from the exact same `waiting_` container — meaning any request whose deadline
had already expired *before ever being picked up for prefill* was still sitting in
`kWaitingPrefill` when `expire_deadlines()` tried to retire it. Same illegal edge, same
`panic()`. This was missed in manual review of `expire_deadlines()`, even by the same
reviewer who had just flagged and fixed the identical issue one function earlier — it
only surfaced because a test was written for that specific scenario (deadline already
past, request still waiting, before any `tick()` had run).

**Takeaway**: fixing a state-machine-transition bug at one call site is not evidence
the same shape of bug is absent at other call sites that share the same terminal-state
target, even when both funnel through the same shared helper (`retire()`). Each call
site's *source* state needs to be independently checked against
`is_valid_transition()`'s actual edges — "we already handled this" is a trap when the
"this" is actually "this specific source state," not "this general pattern." A test
targeting the specific pre-condition (expired while still in the earliest possible
state) caught what code review reading the diff in isolation did not.

---

## 2. Lifecycle state records bookkeeping shape, not "a real backend call happened"

The fix for the bug above — transitioning an expired-while-waiting request through
`kPrefilling` before retiring it `kFailed` — has an odd-sounding consequence: that
request's `RequestLifecycle` history now says it passed through `Prefilling`, even
though `expire_deadlines()` runs *before* `prefill_phase()` specifically so that
`backend_.prefill()` is never called for already-doomed requests (ADR-007's
deadline-check-first ordering). The request is marked as having been in a state whose
entire name implies backend work occurred, and no backend work occurred.

This isn't a bug — `is_valid_transition()` only encodes which state *shapes* are
reachable from which other shapes, and `retire()` is deliberately the single place
every request funnels through on its way to a terminal state, regardless of *why* it's
being retired. Satisfying that shared machinery's contract (a legal transition path
into the terminal state) is a separate concern from what actually happened operationally
on the backend side. The two are correlated in the common case (a request that reaches
`Prefilling` normally did have `prefill()` called for it) but not logically equivalent,
and this is the one path where they diverge.

**Takeaway**: a state machine's job is to describe which *transitions* are legal, not
to serve as an audit log of which *side effects* occurred. Trying to make them the same
thing (e.g. by inventing a `kExpiredBeforePrefill` state just to avoid this rhetorical
awkwardness) would add real complexity to solve a purely cosmetic concern — the actual
observable behavior (no backend call made, handle count stays at zero, request ends up
`Failed`) is correct either way.

---

## 3. Iterator invalidation via `retire()` hit twice, fixed two different ways

Both `expire_deadlines()` and `cancel()` originally called `retire()` — which mutates
`waiting_`/`decoding_` by erasing from them — from *inside* a loop iterating one of
those same containers. Both are undefined behavior for the same underlying reason
(erasing invalidates the iterator the range-`for` is about to increment), but the two
fixes look different, and it's worth understanding why:

- **`expire_deadlines()`** can match *multiple* requests in one call (every request
  past its deadline this tick). The fix: snapshot every expired pointer into a
  separate `std::vector<Request*>` first, then call `retire()` on each pointer from
  that snapshot in a second, independent pass. This mirrors the exact same
  snapshot-then-mutate pattern already used in `prefill_phase()`/`decode_phase()` for
  an unrelated reason (index-lockstep correspondence with backend batch results) — the
  same shape of fix solves two structurally different problems.
- **`cancel()`** can match at most *one* request (a specific `RequestId` is either
  present once or not at all). The fix here is simpler: just `break` immediately after
  the matching `retire()` call. Once we've found and retired the one request we were
  looking for, there's no reason to keep iterating at all — we never touch the
  now-invalidated iterator again because the loop exits before its next increment.

**Takeaway**: "don't mutate a container while iterating it" has more than one valid
fix, and the right one depends on *how many* matches are expected. Snapshot-then-mutate
is the general-purpose answer; an early `break` is a cheaper, equally correct answer
specifically when the loop is a single-match search. Reaching for the heavier pattern
everywhere is not wrong, just occasionally more code than the problem requires.

---

## 4. `operator[]` requiring default-constructibility turned a logic bug into a compile error

`handles_[req->id()] = outcome.value().handle;` failed to *compile*, not run
incorrectly. `std::unordered_map::operator[]` is specified as "insert a
default-constructed value at this key if absent, then return a reference to it" — and
`SequenceHandle` deliberately has no default constructor:

```cpp
class SequenceHandle {
public:
  explicit SequenceHandle(std::uint64_t id) : id_(id) {}
  ...
};
```

This is intentional design, not an oversight in `backend.h`: `SequenceHandle` is
documented as opaque and backend-minted — nothing outside a `PrefillOutcome` should
ever be able to conjure one. Removing that guarantee (e.g. adding a default
constructor just to make `operator[]` compile) would have quietly reopened a hole the
type was designed to close, in service of a call site that didn't actually need it.

The fix was `handles_.emplace(req->id(), outcome.value().handle)` — `emplace`
constructs the value in place from the arguments given, with no default-construction
step at all, so it works with the intentionally-non-default-constructible type. A
separate call site (`decode_phase()`, doing a lookup rather than an insert) correctly
used `.at()` instead, which has no default-construction requirement either since it
only reads an existing entry and throws if absent.

**Takeaway**: `operator[]`'s "insert if missing" behavior is a convenience that has a
real prerequisite (default-constructibility) baked into its contract — and hitting
that prerequisite as a compile error, rather than as a subtle runtime bug, is the
better failure mode by far. A type designed to be intentionally hard to conjure (no
default constructor) turned what could have been a silent logic error into something
the compiler refused to build. `emplace()` for inserts, `.at()` for lookups are the two
`operator[]`-avoiding tools that respect that design rather than fighting it.

---

## 5. Testing an "unconditional invariant" requires a way to trigger the case that's hardest to reach

ADR-007's second decision — release() on every retirement path, no exceptions — has
four retirement paths: success, deadline expiry, cancellation, and `BackendError`. The
first three are all reachable through `MockBackend`'s existing, always-succeeding
behavior. The fourth (`BackendError`) was, before this session, permanently
unreachable — `MockBackend::prefill()`/`decode()` had no way to ever return anything
but success, so the one retirement path requiring the trickiest state-machine handling
(the `kPrefilling` stopover) had zero test coverage, not because anyone forgot to write
the test, but because there was no way to write it.

The fix — `fail_next_prefill()`/`fail_next_decode()`, each backed by a small FIFO queue
of pending `BackendError`s consumed one-per-call in batch order — is deliberately
narrow: opt-in, default-empty, zero behavior change for every test that doesn't call
it. This is the same "test-only extension point, invisible unless used" shape as
`configure_eos()`, which already existed in `MockBackend` for exactly the same reason
(EOS is a real, needed test scenario with no other way to trigger it deterministically).

**Takeaway**: "we should test this" and "we are currently *able* to test this" are
different claims. An invariant stated as "on every path, no exceptions" is only as
verified as the hardest-to-reach path in that "every" — and if the test double doesn't
support reaching it, the invariant is aspirational, not verified, no matter how
carefully the production code was reviewed by hand. Building the minimal, opt-in hook
needed to reach that path is often cheaper than it sounds, and pays for itself the
moment the hardest path turns out to have its own undiscovered bug (as
`expire_deadlines()`'s missing stopover did, independently, in the very same session).

---

## Summary: what this session's implementation work added, beyond the design in ADR-007

| Concern | What we learned |
|---|---|
| State-machine coverage | Fixing a transition bug once doesn't rule it out at other call sites targeting the same terminal state — needs independent verification per call site |
| Lifecycle semantics | `RequestState` describes legal bookkeeping shape, not a guarantee that a corresponding backend call occurred |
| Iterator invalidation | Fix shape depends on match cardinality: snapshot-then-mutate for "many possible matches," early `break` for "at most one" |
| Type design paying off | `SequenceHandle`'s missing default constructor turned a would-be runtime bug into a compile error, via `operator[]`'s implicit default-construct requirement |
| Test-double completeness | An "unconditional, every-path" invariant is unverified on any path the test double cannot trigger — `MockBackend` needed explicit failure-injection to close that gap |
