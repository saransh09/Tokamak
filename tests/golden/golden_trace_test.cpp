#include <catch2/catch_test_macros.hpp>

#include <string>

#include "analysis.h"
#include "simulation.h"
#include "tokamak/request/state.h"

using tokamak::check_invariants;
using tokamak::RequestState;
using tokamak::run_simulation;
using tokamak::SimulationConfig;
using tokamak::SimulationResult;

namespace {
// TOKAMAK_GOLDEN_FIXTURES_DIR is injected by tests/golden/CMakeLists.txt as
// an absolute path to tests/golden/fixtures/ -- avoids any assumption about
// CTest's working directory.
std::string fixture_path(const std::string &name) {
  return std::string(TOKAMAK_GOLDEN_FIXTURES_DIR) + "/" + name;
}
} // namespace

TEST_CASE("run_simulation: single request happy path completes with "
          "correct milestone timestamps",
          "[golden]") {
  SimulationConfig config{fixture_path("simple_single_request.jsonl")};
  SimulationResult result = run_simulation(config);

  REQUIRE(result.tick_reports.size() == 2);
  REQUIRE(result.requests.size() == 1);

  const auto &r = result.requests[0];
  REQUIRE(r.id == "req-1");
  REQUIRE(r.final_state == RequestState::kCompleted);
  REQUIRE(r.output_tokens_emitted == 2);
  REQUIRE(r.max_output_tokens == 2);

  // Precisely computed from MockBackend(1ms/token prefill, flat 5ms decode):
  // prefill 3 tokens -> 3ms, decode -> +5ms (1st token @ 8ms), +5ms (2nd
  // token & completion @ 13ms).
  REQUIRE(r.admitted_at.has_value());
  REQUIRE(r.first_token_at.has_value());
  REQUIRE(r.completed_at.has_value());
  REQUIRE((*r.first_token_at - *r.admitted_at) == std::chrono::milliseconds(8));
  REQUIRE((*r.completed_at - *r.admitted_at) == std::chrono::milliseconds(13));

  REQUIRE(check_invariants(result.requests).empty());
}

TEST_CASE("run_simulation: a request with a tight deadline expires under "
          "genuine FIFO batching pressure from a larger co-batched request",
          "[golden]") {
  // "big" (20 prompt tokens, max_tokens=3) and "tight" (1 prompt token,
  // max_tokens=2, deadline=25ms) both arrive at t=0 and get batched
  // together. Because FifoScheduler::prefill_phase()/decode_phase() call
  // the backend once for the WHOLE batch and only transition state after
  // the batch call returns, both requests share the batch's total elapsed
  // cost, not their own individual cost -- see this session's timing
  // trace. tight's deadline (25ms) is passed by the time tick 1 ends
  // (26ms), so it survives tick 1 (gets one token) and is caught and
  // failed by tick 2's expire_deadlines() before decode_phase() runs.
  SimulationConfig config{fixture_path("deadline_miss.jsonl")};
  SimulationResult result = run_simulation(config);

  REQUIRE(result.tick_reports.size() == 3);
  REQUIRE(result.requests.size() == 2);

  const auto &big =
      result.requests[0].id == "big" ? result.requests[0] : result.requests[1];
  const auto &tight = result.requests[0].id == "tight" ? result.requests[0]
                                                       : result.requests[1];

  REQUIRE(big.final_state == RequestState::kCompleted);
  REQUIRE(big.output_tokens_emitted == 3);
  REQUIRE((*big.completed_at - *big.admitted_at) ==
          std::chrono::milliseconds(36));

  REQUIRE(tight.final_state == RequestState::kFailed);
  REQUIRE(tight.output_tokens_emitted ==
          1); // got exactly one token before expiring
  REQUIRE_FALSE(tight.completed_at.has_value()); // never reached kCompleted
  REQUIRE(tight.first_token_at.has_value());
  REQUIRE((*tight.first_token_at - *tight.admitted_at) ==
          std::chrono::milliseconds(26));

  // Batch composition: tick 0 processes both (prefill+decode attempted=2);
  // tick 1 catches the expiry and only decodes "big" (attempted=1); tick 2
  // finishes "big" alone.
  REQUIRE(result.tick_reports[0].prefill_attempted == 2);
  REQUIRE(result.tick_reports[0].decode_attempted == 2);
  REQUIRE(result.tick_reports[1].expired_count == 1);
  REQUIRE(result.tick_reports[1].decode_attempted == 1);
  REQUIRE(result.tick_reports[2].decode_completed == 1);

  // check_invariants() must still pass: tight's own final state (kFailed)
  // is terminal, and it never emitted more tokens than its budget.
  REQUIRE(check_invariants(result.requests).empty());
}

TEST_CASE("run_simulation: an empty workload file produces no requests, no "
          "tick reports, and no invariant violations",
          "[golden]") {
  SimulationConfig config{fixture_path("empty_workload.jsonl")};
  SimulationResult result = run_simulation(config);

  REQUIRE(result.requests.empty());
  REQUIRE(result.tick_reports.empty());
  REQUIRE(check_invariants(result.requests).empty());
}
