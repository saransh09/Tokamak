#pragma once

#include "simulation.h"
#include "tokamak/request/request.h"
#include <string>
#include <vector>

namespace tokamak {
struct InvariantViolation {
  RequestId request_id;
  std::string description;
};

struct LatencyStats {
  double ttft_p50, ttft_p90, ttft_p99, ttft_min, ttft_mean, ttft_max;
  double e2e_p50, e2e_p90, e2e_p99, e2e_min, e2e_mean, e2e_max;
};

LatencyStats compute_latency_stats(const std::vector<RequestSummary> &);

struct ThroughputStats {
  double throughput_tokens_per_sec, goodput_tokens_per_sec, slo_attainment_rate;
};

ThroughputStats compute_throughput_stats(const std::vector<RequestSummary> &,
                                         Duration sim_wall_time);

// Checks the three invariants Milestone 1 cares about (resolved in planning,
// not yet in an ADR): every request reaches a terminal state; token output
// never exceeds max_output_tokens (Request::emit_token()'s panic should
// never have fired -- this just confirms it didn't); and per-request
// milestone timestamps are monotonically ordered
// (admitted_at <= first_token_at <= completed_at). Returns every violation
// found rather than panicking on the first one (learnings_007 SS -- a
// post-hoc diagnostic pass, not a live safety guard, should report data, not
// abort).
std::vector<InvariantViolation>
check_invariants(const std::vector<RequestSummary> &);
}; // namespace tokamak
