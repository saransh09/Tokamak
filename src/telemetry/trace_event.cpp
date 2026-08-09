#include "tokamak/telemetry/trace_event.h"
#include "nlohmann/json_fwd.hpp"
#include <chrono>

namespace tokamak {

TraceEvent make_trace_event(const TickReport &report, std::size_t iteration,
                            const std::string &policy, TimePoint sim_start) {
  TraceEvent event;
  event.iteration = iteration;
  event.policy = policy;
  event.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           report.timestamp - sim_start)
                           .count();
  event.runnable_prefill = report.prefill_attempted;
  event.runnable_decode = report.decode_attempted;
  // FIFO has no batch-size cap, and same-tick prefill->decode promotion
  // (ADR-007) means decode_attempted already reflects the full compute
  // batch this tick -- there is no separate "selection" step to model yet.
  event.selected_sequences = report.decode_attempted;
  event.prefill_tokens = report.prefill_tokens;
  event.decode_tokens = report.decode_tokens;

  return event;
}

void to_json(nlohmann::json &json_obj, const TraceEvent &event) {
  json_obj = nlohmann::json{{"iteration", event.iteration},
                            {"timestamp_ns", event.timestamp_ns},
                            {"policy", event.policy},
                            {"runnable_prefill", event.runnable_prefill},
                            {"runnable_decode", event.runnable_decode},
                            {"selected_sequences", event.selected_sequences},
                            {"prefill_tokens", event.prefill_tokens},
                            {"decode_tokens", event.decode_tokens}};
}

} // namespace tokamak
