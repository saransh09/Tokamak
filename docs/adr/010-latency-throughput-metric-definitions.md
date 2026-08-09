# ADR-010: Latency percentile methodology and goodput definition

## Status
Accepted

## Date
2026-08-09

## Context

project.md Section 29 requires the Milestone 1 simulation runner to print, in
addition to invariant-check results and per-request timing (ADR-009): "TTFT
and E2E distributions" and "throughput and goodput." Two terms here are
ambiguous enough to need an explicit definition before writing
`compute_latency_stats()`/`compute_throughput_stats()`, since different
serving systems and papers use them inconsistently:

1. **"Distribution"** could be reported as a full histogram, as a fixed set of
   percentiles, or via nearest-rank vs. interpolated percentile computation —
   these give measurably different numbers on the same data.
2. **"Goodput"** is a term of art in the SLO-aware LLM-serving literature
   (DistServe, OSDI '24; Shepherd, NSDI '23), but its published definitions
   are built around a *capacity search* — the maximum sustainable request
   rate at which some fraction of requests still meet their SLO — which is a
   sweep across load levels, not a number computable from a single
   deterministic workload replay. Milestone 1 has no load generator or rate
   sweep yet (that is a later milestone per project.md), so the capacity-
   search definition does not apply as-is and a single-run analogue is
   needed.

These were initially treated as open design questions requiring their own
research pass, and `LatencyStats`/`ThroughputStats` were provisionally
deferred out of Piece C's scope (see ADR-008's Piece C breakdown) pending that
research. On inspection, both questions have standard, citable answers rather
than being genuinely unresolved — deferring them further was not
justified once checked.

## Decision

**Percentile methodology**: linear interpolation between ranks (the
convention used by `numpy.percentile`'s default method and by vLLM's
`benchmark_serving.py`), not nearest-rank. Report p50/p90/p99 alongside
min/mean/max for both TTFT and E2E, computed over all terminal (non-cancelled)
requests in a run.

**Goodput**: adapted from DistServe/Shepherd's single-run analogue —
**SLO attainment rate** is the primary per-run observable (the metric those
papers sweep across load levels to build their headline goodput curve), and
**goodput** for a single deterministic run is defined as *token throughput
restricted to SLO-attaining requests*:

```text
slo_attainment_rate = count(completed_at <= arrival_ms + deadline_ms)
                       / count(all terminal requests)

throughput_tokens_per_sec = total tokens emitted (all requests)
                            / simulation wall-time

goodput_tokens_per_sec    = total tokens emitted by SLO-attaining requests only
                            / simulation wall-time
```

Goodput is token-count-based, not request-count-based, so it stays
unit-consistent with `throughput_tokens_per_sec` and the two numbers are
directly comparable (`goodput <= throughput` always holds, same units) —
matching how DistServe/Shepherd present goodput alongside raw throughput.

`compute_throughput_stats()` therefore needs the request set (for the
per-request deadline check), not just the tick-level `TickReport` stream:

```cpp
struct ThroughputStats {
  double throughput_tokens_per_sec;
  double goodput_tokens_per_sec;
  double slo_attainment_rate; // 0.0-1.0
};

ThroughputStats compute_throughput_stats(
    const std::vector<std::shared_ptr<Request>> &requests,
    Duration sim_wall_time);
```

## Alternatives considered

- **Nearest-rank percentiles**: simpler to implement, but not what published
  serving benchmarks report — using it would make Tokamak's numbers
  non-comparable to the literature it is modeling itself on for no real
  implementation savings (both are a handful of lines).
- **Request-count-based goodput** (fraction of requests meeting deadline,
  reported as a rate rather than a token/sec figure): this is exactly
  `slo_attainment_rate` above, which is kept as its own field since it is
  useful on its own — but calling *that* "goodput" would leave the metric in
  different units than throughput, making the two non-comparable side by
  side. Reporting both a rate (`slo_attainment_rate`) and a rate-matched
  goodput (`goodput_tokens_per_sec`) avoids picking one at the expense of the
  other.
- **Full capacity-search goodput** (DistServe's original definition: max
  sustainable rate at a target SLO attainment threshold, found via a sweep):
  correct definition, wrong milestone — requires a load generator varying
  request rate across multiple runs, which does not exist yet. Revisit when
  Tokamak has a rate-sweep harness; the single-run metrics defined here are
  not meant to replace that, only to give Milestone 1's one-shot deterministic
  replay a comparable per-run number in the meantime.
- **Continuing to defer both metrics** (the original plan): rejected once it
  became clear neither question was actually open — the percentile method and
  goodput definition both have standard answers in tooling/literature already
  in scope for this project's stated framing (project.md's SLO-aware framing
  cites exactly this kind of goodput metric).

## Consequences

- `apps/tokamak/analysis.h`/`.cpp` gains `compute_latency_stats()` and
  `compute_throughput_stats()` alongside `check_invariants()`, completing
  project.md Section 29's full printed-output list in Piece C rather than
  leaving a deferred gap.
- `compute_throughput_stats()` takes the request set as an input (needed for
  the per-request deadline check), not only the `TickReport` stream.
- A request cancelled mid-flight (no `completed_at()`) is excluded from
  latency-distribution and SLO-attainment computation, consistent with
  ADR-009's discipline of treating unreached milestones as absent (`nullopt`)
  rather than as zero or as a violation.
- If Tokamak later adds a load-rate sweep harness, the capacity-search
  definition of goodput can be layered on top of `slo_attainment_rate`
  without changing anything defined here — this ADR's goodput is a per-run
  building block, not a competing definition.
