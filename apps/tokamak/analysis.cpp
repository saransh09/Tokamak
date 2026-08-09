#include "analysis.h"
#include "tokamak/common/clock.h"
#include "tokamak/request/state.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

namespace tokamak {
namespace {
// Linear-interpolation percentile (ADR-010) -- the numpy/vLLM-benchmark
// convention, not nearest-rank. `sorted_values` must be sorted ascending and
// non-empty; callers guard the empty case themselves.
double percentile(const std::vector<double> &sorted_values, double p) {
  if (sorted_values.size() == 1) {
    return sorted_values.front();
  }
  double rank = (p / 100.0) * static_cast<double>(sorted_values.size() - 1);
  auto lower = static_cast<std::size_t>(std::floor(rank));
  auto upper = static_cast<std::size_t>(std::ceil(rank));
  double weight = rank - static_cast<double>(lower);
  return sorted_values[lower] +
         weight * (sorted_values[upper] - sorted_values[lower]);
}

double to_ms(Duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

} // namespace

std::vector<InvariantViolation>
check_invariants(const std::vector<RequestSummary> &requests) {
  std::vector<InvariantViolation> violations;

  for (const auto &r : requests) {
    // 1. Every request must end in terminal state
    if (!is_terminal(r.final_state)) {
      violations.push_back(
          {r.id, "request did not reach a terminal state (final state: " +
                     std::string(to_string(r.final_state)) + ")"});
    }
    // 2. output_tokens_emitted() <= max_output_tokens always -- confirms
    // Request::emit_tokens()'s panic never gets fired
    if (r.output_tokens_emitted > r.max_output_tokens) {
      violations.push_back({r.id, "output_tokens_emitted (" +
                                      std::to_string(r.output_tokens_emitted) +
                                      ") exceeded max_output_tokens (" +
                                      std::to_string(r.max_output_tokens) +
                                      ")"});
    }

    // 3. Milestone timestamp ordering: admitted_at <= first_token_at <=
    // completed_at check pairwise whenever both sides are present
    if (r.admitted_at && r.first_token_at &&
        *r.first_token_at < *r.admitted_at) {
      violations.push_back(
          {r.id, "first_token_at occurred before admitted_at"});
    }
    if (r.first_token_at && r.completed_at &&
        *r.completed_at < *r.first_token_at) {
      violations.push_back(
          {r.id, "completed_at occurred before first_token_at"});
    }
    if (r.admitted_at && r.completed_at && *r.completed_at < *r.admitted_at) {
      violations.push_back({r.id, "completed_at occurred before admitted_at"});
    }
  }

  return violations;
}

LatencyStats
compute_latency_stats(const std::vector<RequestSummary> &requests) {
  std::vector<double> ttft_ms;
  std::vector<double> e2e_ms;

  for (const auto &r : requests) {
    // A request only contributes TTFT/E2E if it actually reached that
    // milestone -- cancelled/failed-before-first-token requests naturally
    // drop out here since first_token_at()/completed_at() stay nullopt for
    // them, without needing to filter on final_state explicitly.
    if (r.admitted_at && r.first_token_at) {
      ttft_ms.push_back(to_ms(*r.first_token_at - *r.admitted_at));
    }
    if (r.admitted_at && r.completed_at) {
      e2e_ms.push_back(to_ms(*r.completed_at - *r.admitted_at));
    }
  }

  std::sort(ttft_ms.begin(), ttft_ms.end());
  std::sort(e2e_ms.begin(), e2e_ms.end());

  LatencyStats stats{};

  if (!ttft_ms.empty()) {
    stats.ttft_p50 = percentile(ttft_ms, 50.0);
    stats.ttft_p90 = percentile(ttft_ms, 90.0);
    stats.ttft_p99 = percentile(ttft_ms, 99.0);
    stats.ttft_min = ttft_ms.front();
    stats.ttft_max = ttft_ms.back();
    stats.ttft_mean = std::accumulate(ttft_ms.begin(), ttft_ms.end(), 0.0) /
                      static_cast<double>(ttft_ms.size());
  }

  if (!e2e_ms.empty()) {
    stats.e2e_p50 = percentile(e2e_ms, 50.0);
    stats.e2e_p90 = percentile(e2e_ms, 90.0);
    stats.e2e_p99 = percentile(e2e_ms, 99.0);
    stats.e2e_min = e2e_ms.front();
    stats.e2e_max = e2e_ms.back();
    stats.e2e_mean = std::accumulate(e2e_ms.begin(), e2e_ms.end(), 0.0) /
                     static_cast<double>(e2e_ms.size());
  }

  return stats;
}

ThroughputStats
compute_throughput_stats(const std::vector<RequestSummary> &requests,
                         Duration sim_wall_time) {
  double wall_time_sec = std::chrono::duration<double>(sim_wall_time).count();

  // "all requests" per ADR-010 -- every token actually emitted counts
  // toward raw throughput, regardless of how the request ultimately ended.
  std::size_t total_tokens = 0;
  for (const auto &r : requests) {
    total_tokens += r.output_tokens_emitted;
  }

  // SLO attainment / goodput are only well-defined for requests the system
  // actually finished judging (terminal); by Milestone 1's design every
  // submitted request should be terminal by the time run_simulation()
  // returns, so this filter is currently a no-op safety net, not an active
  // exclusion.
  std::size_t terminal_count = 0;
  std::size_t attaining_count = 0;
  std::size_t good_tokens = 0;

  for (const auto &r : requests) {
    if (!is_terminal(r.final_state)) {
      continue;
    }
    ++terminal_count;

    bool met_deadline =
        r.completed_at.has_value() && *r.completed_at <= r.deadline_at;
    if (met_deadline) {
      ++attaining_count;
      good_tokens += r.output_tokens_emitted;
    }
  }

  ThroughputStats stats{};
  if (wall_time_sec > 0.0) {
    stats.throughput_tokens_per_sec =
        static_cast<double>(total_tokens) / wall_time_sec;
    stats.goodput_tokens_per_sec =
        static_cast<double>(good_tokens) / wall_time_sec;
  }
  if (terminal_count > 0) {
    stats.slo_attainment_rate = static_cast<double>(attaining_count) /
                                static_cast<double>(terminal_count);
  }

  return stats;
}

} // namespace tokamak
