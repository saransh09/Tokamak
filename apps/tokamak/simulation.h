#pragma once

#include "tokamak/common/clock.h"
#include "tokamak/request/request.h"
#include "tokamak/scheduler/tick_report.h"
#include "tokamak/telemetry/trace_writer.h"
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace tokamak {
struct RequestSummary {
  RequestId id;
  RequestState final_state;
  TimePoint deadline_at;

  std::optional<TimePoint> admitted_at;
  std::optional<TimePoint> waiting_prefill_at; // entered_at(kWaitingPrefill)
  std::optional<TimePoint> prefilling_at;      // entered_at(kPrefilling)
  std::optional<TimePoint> waiting_decode_at;  // entered_at(kWaitingDecode)
  std::optional<TimePoint> decoding_at;        // entered_at(kDecoding)
  std::optional<TimePoint> first_token_at;
  std::optional<TimePoint> completed_at;

  std::size_t output_tokens_emitted;
  std::size_t max_output_tokens;
};

struct SimulationConfig {
  std::string workload_path;
};

struct SimulationResult {
  std::vector<TickReport> tick_reports;
  std::vector<RequestSummary> requests;
};

SimulationResult run_simulation(const SimulationConfig &config,
                                TraceWriter *trace_writer = nullptr);
}; // namespace tokamak
