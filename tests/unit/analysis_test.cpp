#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "analysis.h"
#include "simulation.h"
#include "tokamak/common/clock.h"
#include "tokamak/request/state.h"

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using tokamak::compute_latency_stats;
using tokamak::compute_throughput_stats;
using tokamak::check_invariants;
using tokamak::Duration;
using tokamak::LatencyStats;
using tokamak::RequestState;
using tokamak::RequestSummary;
using tokamak::ThroughputStats;
using tokamak::TimePoint;

namespace {
// Builds a fully "clean" completed RequestSummary: admitted at t=0ms,
// first token at t=first_token_ms, completed at t=e2e_ms, emitted exactly
// `emitted` of `max` output tokens, deadline far in the future so it never
// factors into SLO-miss tests unless overridden.
RequestSummary make_clean_summary(std::string id, double first_token_ms,
                                  double e2e_ms, std::size_t emitted,
                                  std::size_t max,
                                  double deadline_ms = 1'000'000.0) {
  RequestSummary r{};
  r.id = std::move(id);
  r.final_state = RequestState::kCompleted;
  r.deadline_at = TimePoint{} + Duration(std::chrono::duration_cast<Duration>(
                                    std::chrono::duration<double, std::milli>(
                                        deadline_ms)));
  r.admitted_at = TimePoint{};
  r.first_token_at =
      TimePoint{} + std::chrono::duration_cast<Duration>(
                        std::chrono::duration<double, std::milli>(first_token_ms));
  r.completed_at =
      TimePoint{} + std::chrono::duration_cast<Duration>(
                        std::chrono::duration<double, std::milli>(e2e_ms));
  r.output_tokens_emitted = emitted;
  r.max_output_tokens = max;
  return r;
}
} // namespace

// ---------------------------------------------------------------------------
// compute_latency_stats
// ---------------------------------------------------------------------------

TEST_CASE("compute_latency_stats returns all-zero stats for empty input",
          "[analysis]") {
  LatencyStats stats = compute_latency_stats({});
  REQUIRE(stats.ttft_p50 == 0.0);
  REQUIRE(stats.ttft_mean == 0.0);
  REQUIRE(stats.e2e_max == 0.0);
}

TEST_CASE("compute_latency_stats: single request, all percentiles equal the "
          "one value",
          "[analysis]") {
  std::vector<RequestSummary> reqs{make_clean_summary("r1", 10.0, 50.0, 8, 8)};
  LatencyStats stats = compute_latency_stats(reqs);

  REQUIRE_THAT(stats.ttft_p50, WithinAbs(10.0, 1e-9));
  REQUIRE_THAT(stats.ttft_p90, WithinAbs(10.0, 1e-9));
  REQUIRE_THAT(stats.ttft_p99, WithinAbs(10.0, 1e-9));
  REQUIRE_THAT(stats.ttft_min, WithinAbs(10.0, 1e-9));
  REQUIRE_THAT(stats.ttft_max, WithinAbs(10.0, 1e-9));
  REQUIRE_THAT(stats.e2e_p50, WithinAbs(50.0, 1e-9));
}

TEST_CASE("compute_latency_stats: linear-interpolation percentiles match "
          "ADR-010's numpy convention on a known 3-point set",
          "[analysis]") {
  // TTFT values 7, 8, 10 (sorted) -- matches the earlier hand-verified
  // smoke-test run: p50=8, p90=9.6, p99=9.96.
  std::vector<RequestSummary> reqs{
      make_clean_summary("r1", 8.0, 100.0, 1, 1),
      make_clean_summary("r2", 7.0, 100.0, 1, 1),
      make_clean_summary("r3", 10.0, 100.0, 1, 1),
  };
  LatencyStats stats = compute_latency_stats(reqs);

  REQUIRE_THAT(stats.ttft_p50, WithinAbs(8.0, 1e-9));
  REQUIRE_THAT(stats.ttft_p90, WithinAbs(9.6, 1e-9));
  REQUIRE_THAT(stats.ttft_p99, WithinAbs(9.96, 1e-9));
  REQUIRE_THAT(stats.ttft_mean, WithinAbs(8.3333333333, 1e-6));
}

TEST_CASE("compute_latency_stats excludes a request from TTFT if it never "
          "reached first_token_at, but E2E still requires completed_at "
          "independently",
          "[analysis]") {
  // Failed before ever decoding: no first_token_at, no completed_at at all
  // (Failed isn't "completed" -- RequestSummary.completed_at stays nullopt
  // for it too since analysis.cpp only reads whatever simulation.cpp
  // snapshotted, and simulation.cpp never sets completed_at for non-
  // Completed requests). Should contribute to neither TTFT nor E2E.
  RequestSummary failed{};
  failed.id = "failed-1";
  failed.final_state = RequestState::kFailed;
  failed.deadline_at = TimePoint{} + 1000ms;
  failed.admitted_at = TimePoint{};
  failed.output_tokens_emitted = 0;
  failed.max_output_tokens = 5;
  // first_token_at, completed_at left as std::nullopt (default)

  std::vector<RequestSummary> reqs{failed,
                                   make_clean_summary("ok-1", 10.0, 20.0, 3, 3)};
  LatencyStats stats = compute_latency_stats(reqs);

  // Only "ok-1" contributes -- single-value percentile branch.
  REQUIRE_THAT(stats.ttft_p50, WithinAbs(10.0, 1e-9));
  REQUIRE_THAT(stats.e2e_p50, WithinAbs(20.0, 1e-9));
}

// ---------------------------------------------------------------------------
// compute_throughput_stats
// ---------------------------------------------------------------------------

TEST_CASE("compute_throughput_stats: all requests meet their deadline -> "
          "goodput equals throughput and SLO attainment is 100%",
          "[analysis]") {
  std::vector<RequestSummary> reqs{
      make_clean_summary("r1", 5.0, 30.0, 8, 8, /*deadline_ms=*/500.0),
      make_clean_summary("r2", 5.0, 30.0, 4, 4, /*deadline_ms=*/500.0),
  };
  ThroughputStats stats =
      compute_throughput_stats(reqs, std::chrono::duration_cast<Duration>(1s));

  REQUIRE_THAT(stats.throughput_tokens_per_sec, WithinAbs(12.0, 1e-9));
  REQUIRE_THAT(stats.goodput_tokens_per_sec, WithinAbs(12.0, 1e-9));
  REQUIRE_THAT(stats.slo_attainment_rate, WithinAbs(1.0, 1e-9));
}

TEST_CASE("compute_throughput_stats: all requests miss their deadline -> "
          "goodput is zero but throughput still counts every token",
          "[analysis]") {
  std::vector<RequestSummary> reqs{
      make_clean_summary("r1", 5.0, 600.0, 8, 8, /*deadline_ms=*/500.0),
      make_clean_summary("r2", 5.0, 600.0, 4, 4, /*deadline_ms=*/500.0),
  };
  ThroughputStats stats =
      compute_throughput_stats(reqs, std::chrono::duration_cast<Duration>(1s));

  REQUIRE_THAT(stats.throughput_tokens_per_sec, WithinAbs(12.0, 1e-9));
  REQUIRE_THAT(stats.goodput_tokens_per_sec, WithinAbs(0.0, 1e-9));
  REQUIRE_THAT(stats.slo_attainment_rate, WithinAbs(0.0, 1e-9));
}

TEST_CASE("compute_throughput_stats: mixed hit/miss -> goodput only counts "
          "the attaining request's tokens",
          "[analysis]") {
  std::vector<RequestSummary> reqs{
      make_clean_summary("hit", 5.0, 100.0, 8, 8, /*deadline_ms=*/500.0),
      make_clean_summary("miss", 5.0, 600.0, 4, 4, /*deadline_ms=*/500.0),
  };
  ThroughputStats stats =
      compute_throughput_stats(reqs, std::chrono::duration_cast<Duration>(1s));

  REQUIRE_THAT(stats.throughput_tokens_per_sec, WithinAbs(12.0, 1e-9));
  REQUIRE_THAT(stats.goodput_tokens_per_sec, WithinAbs(8.0, 1e-9));
  REQUIRE_THAT(stats.slo_attainment_rate, WithinAbs(0.5, 1e-9));
}

TEST_CASE("compute_throughput_stats: zero sim_wall_time leaves rates at "
          "their zero-initialized default rather than dividing by zero",
          "[analysis]") {
  std::vector<RequestSummary> reqs{
      make_clean_summary("r1", 5.0, 30.0, 8, 8),
  };
  ThroughputStats stats =
      compute_throughput_stats(reqs, Duration::zero());

  REQUIRE(stats.throughput_tokens_per_sec == 0.0);
  REQUIRE(stats.goodput_tokens_per_sec == 0.0);
  // slo_attainment_rate is independent of wall time -- should still compute.
  REQUIRE_THAT(stats.slo_attainment_rate, WithinAbs(1.0, 1e-9));
}

TEST_CASE("compute_throughput_stats: no terminal requests leaves "
          "slo_attainment_rate at its zero-initialized default",
          "[analysis]") {
  // final_state left default (kReceived) -- not terminal, so the request
  // is excluded from the terminal_count denominator entirely.
  RequestSummary non_terminal{};
  non_terminal.id = "still-running";
  non_terminal.final_state = RequestState::kReceived;
  non_terminal.deadline_at = TimePoint{} + 1000ms;
  non_terminal.output_tokens_emitted = 2;
  non_terminal.max_output_tokens = 10;

  ThroughputStats stats = compute_throughput_stats(
      {non_terminal}, std::chrono::duration_cast<Duration>(1s));

  REQUIRE_THAT(stats.throughput_tokens_per_sec, WithinAbs(2.0, 1e-9));
  REQUIRE(stats.slo_attainment_rate == 0.0);
}

// ---------------------------------------------------------------------------
// check_invariants
// ---------------------------------------------------------------------------

TEST_CASE("check_invariants: a fully clean request produces zero violations",
          "[analysis]") {
  std::vector<RequestSummary> reqs{make_clean_summary("clean", 10.0, 50.0, 8, 8)};
  REQUIRE(check_invariants(reqs).empty());
}

TEST_CASE("check_invariants: non-terminal final_state is flagged",
          "[analysis]") {
  RequestSummary r{};
  r.id = "stuck";
  r.final_state = RequestState::kDecoding; // not terminal
  r.deadline_at = TimePoint{} + 1000ms;

  auto violations = check_invariants({r});
  REQUIRE(violations.size() == 1);
  REQUIRE(violations[0].request_id == "stuck");
}

TEST_CASE("check_invariants: output_tokens_emitted exceeding "
          "max_output_tokens is flagged",
          "[analysis]") {
  RequestSummary r = make_clean_summary("over-budget", 10.0, 50.0,
                                        /*emitted=*/9, /*max=*/8);
  auto violations = check_invariants({r});
  REQUIRE(violations.size() == 1);
  REQUIRE(violations[0].request_id == "over-budget");
}

TEST_CASE("check_invariants: first_token_at before admitted_at is flagged",
          "[analysis]") {
  RequestSummary r = make_clean_summary("time-travel-1", /*first_token_ms=*/-5.0,
                                        50.0, 3, 3);
  auto violations = check_invariants({r});
  REQUIRE(violations.size() == 1);
  REQUIRE(violations[0].description ==
          "first_token_at occurred before admitted_at");
}

TEST_CASE("check_invariants: completed_at before first_token_at is flagged",
          "[analysis]") {
  RequestSummary r =
      make_clean_summary("time-travel-2", /*first_token_ms=*/50.0,
                         /*e2e_ms=*/10.0, 3, 3);
  auto violations = check_invariants({r});

  // e2e_ms (10.0) < first_token_ms (50.0) also implies completed_at <
  // admitted_at (admitted_at is fixed at 0ms), so both check 2 and check 3
  // fire here -- assert on the specific description we care about rather
  // than the total count.
  bool found = false;
  for (const auto &v : violations) {
    if (v.description == "completed_at occurred before first_token_at") {
      found = true;
    }
  }
  REQUIRE(found);
}

TEST_CASE("check_invariants: multiple requests each contribute their own "
          "violations independently",
          "[analysis]") {
  RequestSummary clean = make_clean_summary("clean", 10.0, 50.0, 8, 8);
  RequestSummary stuck{};
  stuck.id = "stuck";
  stuck.final_state = RequestState::kWaitingPrefill;
  stuck.deadline_at = TimePoint{} + 1000ms;

  auto violations = check_invariants({clean, stuck});
  REQUIRE(violations.size() == 1);
  REQUIRE(violations[0].request_id == "stuck");
}

TEST_CASE("check_invariants: Cancelled and Failed are accepted as terminal "
          "even though run_simulation() cannot currently produce them",
          "[analysis]") {
  RequestSummary cancelled{};
  cancelled.id = "cancelled-1";
  cancelled.final_state = RequestState::kCancelled;
  cancelled.deadline_at = TimePoint{} + 1000ms;

  RequestSummary failed{};
  failed.id = "failed-1";
  failed.final_state = RequestState::kFailed;
  failed.deadline_at = TimePoint{} + 1000ms;

  REQUIRE(check_invariants({cancelled, failed}).empty());
}
