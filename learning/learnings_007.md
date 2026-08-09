# Learnings 007 — Trace Reporting and Lifecycle History: Design Theory

## Questions

1. Why does `FifoScheduler::tick()` return a `TickReport` value instead of taking an observer/callback, or writing telemetry itself?
2. What makes an "honest subset" schema different from a stubbed-out full schema, and why does that distinction matter beyond style?
3. Why does `state_entered_at_[to] = entered_at_;` compile and behave correctly, when `handles_[req->id()] = ...` did neither?
4. Why did the actual cost of ADR-009's change turn out much smaller than the first framing of the decision assumed — and what does that say about how to size an architectural change *before* deciding on it?
5. `entered_at(RequestState)` uses `.contains()` followed by `.at()` — two map lookups instead of one. Is that a bug, and is it worth fixing?

---

## 1. Returning a value struct vs. an observer/callback

`TickReport` is a plain data value returned by `tick()` — not a callback invoked mid-tick, not an observer registered ahead of time. The alternative (an observer interface `FifoScheduler` calls into during `expire_deadlines()`/`prefill_phase()`/`decode_phase()`) was never seriously on the table once ADR-008 framed the question correctly: *how does the scheduler expose decision data without knowing how that data will be used?*

A callback-based design would need the scheduler to hold a reference to *something* that knows about JSON, or logging, or metrics — even if that something is an abstract interface, the scheduler now has a dependency that exists purely to serve telemetry, threaded into its hot path. A returned value has zero such dependency: `TickReport` is defined in the scheduler's own module, describes only facts about what the scheduler did, and the caller decides afterward what to do with it — build a `TraceEvent`, increment a Prometheus counter, ignore it entirely.

**Takeaway**: when a component needs to report what it did, and multiple different things might want to consume that report, returning a plain value keeps the boundary narrow (the same principle ADR-004 named for the backend boundary) — the producer doesn't grow a dependency on any particular consumer's shape, present or hypothetical.

---

## 2. "Absent" is a stronger claim than "zero" or "not yet implemented"

ADR-008 chose to omit `kv_pages_free`, `earliest_slack_ms`, and `decision_us` from `TraceEvent` entirely, rather than including them with placeholder values like `0` or `-1`. The difference sounds cosmetic but isn't: a JSON field with a numeric value invites a reader — human or downstream script — to treat it as measured data. `"kv_pages_free": 0` looks like "the cache is full," not "we don't have a cache model yet." The field's mere presence asserts something false.

Omitting the field entirely is a weaker, more honest claim: it says nothing at all about KV pages, rather than saying something wrong. This mirrors `BackendCapabilities`' existing rule — "queried, never guessed" — extended from *querying a backend* to *emitting a trace*: a trace schema should describe what the system actually measured this tick, not what the target schema eventually wants to look like.

**Takeaway**: when a target schema exists (project.md's full scheduler-trace event) but the subsystem behind some of its fields doesn't exist yet, the honest move is field *absence*, not field *stubbing* — a placeholder value is a claim, and an absent field is the refusal to make a claim you can't yet back up.

---

## 3. Why `operator[]` was safe here, unlike the `SequenceHandle` case

learnings_006 (§4) already covered why `handles_[req->id()] = outcome.value().handle;` failed to *compile*: `operator[]` requires the map's value type to be default-constructible (it default-constructs a value in place before the assignment overwrites it), and `SequenceHandle` deliberately has no default constructor — by design, since an "empty" opaque backend handle would be meaningless.

`state_entered_at_[to] = entered_at_;` in `RequestLifecycle::transition_to()` uses the exact same `operator[]` pattern, on a `std::unordered_map<RequestState, TimePoint>` — and this one is correct, for a specific, checkable reason: `TimePoint` (`std::chrono::time_point`) *does* have a default constructor (it default-initializes to the clock's epoch). So `operator[]`'s implicit "default-construct if absent" step is satisfiable, and — crucially — the transient default value is never observable, because the assignment on the same line overwrites it immediately, before any other code has a chance to call `entered_at(to)` and read the stale default.

This also clarifies why the *read* side (`entered_at()`) correctly avoids `operator[]` altogether, using `.contains()` + `.at()` instead: reading is a different contract than writing. A read should never *insert* a new entry as a side effect of being asked "does this exist" — `operator[]` would silently insert an epoch-zero `TimePoint` for any state queried but never visited, corrupting the very "never visited" signal the method exists to report. `.at()` after `.contains()` (or `.find()` in one lookup) reads without ever mutating.

**Takeaway**: `operator[]`'s "insert-default-if-absent" contract is a two-part question every time it's used for a write: (a) is the value type default-constructible at all (a compile-time gate, as `SequenceHandle` demonstrated), and (b) even if it is, does a transient default value ever leak somewhere observable before being overwritten (a correctness question `TimePoint`'s immediate-assignment pattern here answers cleanly, but a more complex value type or a delayed assignment might not). Passing gate (a) is necessary but not sufficient — this session's code passed both, but they're genuinely separate checks.

---

## 4. Measuring a change's actual size beats estimating it before deciding

ADR-009 started as a question framed with real hesitation: extending `RequestLifecycle` to record every state's entry timestamp (not just `kAdmitted`/`kCompleted`) was initially presented as a meaningful scope decision, comparable in weight to deferring the CLI dispatcher or deferring preemption — something to lean toward *not* doing unless clearly justified.

That framing turned out to be wrong-sized, and the way it got corrected is the actual lesson: instead of debating the tradeoff in the abstract, the concrete blast radius got checked directly — every call site of `transition_to()` (grep across the codebase), what each one reads versus just drives, and whether any existing test's assertions depended on the old single-`entered_at_`-field behavior in a way an additive change would break. The answer was: zero call sites read history today, and the change was ~15 lines, fully additive, with no risk to anything already built. Once that was known, the "cost" side of the cost-benefit comparison collapsed to near-zero, and the decision became easy in the opposite direction from where the conversation started.

**Takeaway**: "how big is this change" is an empirical question, answerable by actually tracing call sites and reading the affected code, not a question to answer by analogy to a previous, differently-shaped decision (the CLI dispatcher deferral was about *unbuilt* abstraction with no second consumer; this was about *additive* extension to an existing, already-narrow class). Treating every "should we extend X" question as equally risky, without checking, risks either overbuilding from unfounded caution or underbuilding from an unexamined assumption that "small extensions are always small" — the only way to know which is true is to look.

---

## 5. Two lookups instead of one is a performance question, not a correctness one — and the two categories should not be conflated

`RequestLifecycle::entered_at(RequestState state) const` was written as:

```cpp
std::optional<TimePoint>
RequestLifecycle::entered_at(RequestState state) const {
  if (!state_entered_at_.contains(state))
    return std::nullopt;
  return state_entered_at_.at(state);
}
```

This calls into the map twice on the "found" path — once via `.contains()`, once via `.at()` — where a single `.find()` call returning an iterator would answer both "is it present" and "what's the value" in one traversal:

```cpp
auto it = state_entered_at_.find(state);
if (it == state_entered_at_.end())
  return std::nullopt;
return it->second;
```

The `.contains()` + `.at()` version is not a bug: both calls are `const`, neither mutates, and `.at()` is only ever reached after `.contains()` has already confirmed the key exists, so the "throws if absent" behavior of `.at()` never actually fires here. It is strictly a *cost* question (two hash computations and bucket traversals instead of one), and at this project's current scale — a handful of states per request, looked up a handful of times per request during summary reporting — that cost is immaterial; there is no loop calling this at high frequency in a hot path.

**Takeaway**: not every inefficiency is worth fixing the moment it's spotted, and it's worth being explicit about *why* — the distinguishing question is "does this run somewhere frequent/hot enough for the redundant work to matter," not "is there a more elegant way to write it." Rewriting `.contains()` + `.at()` into a single `.find()` here would be a reasonable cleanup with no downside, but treating it as urgent would be solving a performance problem that doesn't exist yet, in a codebase whose stated engineering discipline (ADR-004, ADR-008) has consistently been "build what today's real requirement supports, not what looks more clever."

---

## Summary: what this session's design work added, beyond ADR-007/ADR-008's prior lessons

| Concern | What we learned |
|---|---|
| Reporting boundary shape | Returning a plain value (`TickReport`) keeps a producer's boundary narrower than a callback/observer would, since the producer never needs to know what any consumer does with the report |
| Schema honesty | Omitting a field entirely is a strictly weaker, more honest claim than stubbing it with a placeholder value — presence of a field asserts something, even a wrong something |
| `operator[]` safety conditions | Two separate checks, not one: is the value type default-constructible (compile-time), and does the transient default ever leak observably before being overwritten (correctness) — `TimePoint` passes both here, `SequenceHandle` failed the first by design |
| Sizing a change before deciding | "How big is this" is answered by tracing actual call sites and existing test coverage, not by analogy to a differently-shaped prior decision — the real cost of ADR-009 was far smaller than its first framing assumed |
| Efficiency vs. correctness | A redundant-but-correct lookup (`.contains()` + `.at()` vs. a single `.find()`) is a cost question, not a bug — worth noticing, not worth treating as urgent absent a measured hot path |
