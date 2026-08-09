#pragma once

#include "tokamak/scheduler/tick_report.h"
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
namespace tokamak {

// The honest-subset scheduler trace event (ADR-008 Decision 2): only fields
// that describe something real today. Deliberately omits kv_pages_free,
// earliest_slack_ms, decision_us -- see ADR-008 for why these are left out
// entirely rather than stubbed with placeholder values.
struct TraceEvent {
  std::size_t iteration = 0;
  std::int64_t timestamp_ns = 0;
  std::string policy;

  std::size_t runnable_prefill = 0;
  std::size_t runnable_decode = 0;
  std::size_t selected_sequences = 0;
  std::size_t prefill_tokens = 0;
  std::size_t decode_tokens = 0;
};

// Builds a TraceEvent from one tick's TickReport.
//
// `iteration` and `policy` are supplied by the caller (the simulation
// runner) rather than tracked by FifoScheduler itself -- the scheduler
// stays trace-format-agnostic (ADR-008 Decision 1); "which loop iteration
// this is" and "what policy produced this tick" are properties of the
// simulation driving the scheduler, not of the scheduler's internal state.
//
// `sim_start` is the TimePoint the simulation began at; timestamp_ns is
// computed as nanoseconds elapsed since then, since steady_clock::time_point
// has no meaningful absolute epoch on its own.
TraceEvent make_trace_event(const TickReport &report, std::size_t iteration,
                            const std::string &policy, TimePoint sim_start);

// Serializes a TraceEvent to its JSON representation (nlohmann::json ADL
// hook -- enables `nlohmann::json(event)` and `json_obj = event`).
void to_json(nlohmann::json &json_obj, const TraceEvent &event);

}; // namespace tokamak
