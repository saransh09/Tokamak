#include "simulation.h"
#include "tokamak/backend/mock_backend.h"
#include "tokamak/common/clock.h"
#include "tokamak/request/state.h"
#include "tokamak/scheduler/fifo_scheduler.h"
#include "tokamak/scheduler/tick_report.h"
#include "tokamak/telemetry/trace_event.h"
#include "tokamak/telemetry/trace_writer.h"
#include "workload.h"
#include <chrono>
#include <memory>
#include <ratio>
#include <vector>

using namespace std::chrono_literals;

namespace tokamak {
SimulationResult run_simulation(const SimulationConfig &config,
                                TraceWriter *trace_writer) {
  FakeClock clock;
  TimePoint sim_start = clock.now();
  MockBackend backend(clock, 1ms, 5ms);
  FifoScheduler scheduler(backend, clock);

  auto workload = load_workload(config.workload_path); // sorted by arrival_ms
  std::vector<Request *> submitted; // raw pointers into scheduler-owned memory;
                                    // valid until scheduler is destroyed

  std::size_t next_to_submit = 0;
  std::size_t iteration = 0;
  std::vector<TickReport> reports;

  while (next_to_submit < workload.size() || scheduler.waiting_count() > 0 ||
         scheduler.decoding_count() > 0) {
    // 1. submit everything whose arrival has passed
    double elapsed_ms =
        std::chrono::duration<double, std::milli>(clock.now() - TimePoint{})
            .count();
    while (next_to_submit < workload.size() &&
           workload[next_to_submit].arrival_ms <= elapsed_ms) {
      auto &w = workload[next_to_submit++];
      auto deadline_from_now = duration_cast<Duration>(
          std::chrono::duration<double, std::milli>(w.deadline_ms));
      auto req = std::make_unique<Request>(w.id, clock, deadline_from_now,
                                           w.max_tokens, w.prompt_token_ids);
      submitted.push_back(&scheduler.submit(std::move(req)));
    }

    // 2. tick while there's runnable work
    if (scheduler.waiting_count() > 0 || scheduler.decoding_count() > 0) {
      TickReport r = scheduler.tick();
      if (trace_writer) {
        trace_writer->write(make_trace_event(r, iteration, "fifo", sim_start));
      }
      reports.push_back(r);
      ++iteration;
    } else if (next_to_submit < workload.size()) {
      // 3. idle : jump clock to next arrival
      auto next_arrival = workload[next_to_submit].arrival_ms;
      clock.advance(
          std::chrono::duration_cast<Duration>(
              std::chrono::duration<double, std::milli>(next_arrival)) -
          (clock.now() - TimePoint{}));
    }
  }

  // snapshot before scheduler/backend/clock goes out of scope
  std::vector<RequestSummary> summaries;
  for (Request *r : submitted) {
    summaries.push_back(
        {r->id(), r->lifecycle().state(), r->deadline_at(),
         r->lifecycle().admitted_at(),
         r->lifecycle().entered_at(RequestState::kWaitingPrefill),
         r->lifecycle().entered_at(RequestState::kPrefilling),
         r->lifecycle().entered_at(RequestState::kWaitingDecode),
         r->lifecycle().entered_at(RequestState::kDecoding),
         r->lifecycle().first_token_at(), r->lifecycle().completed_at(),
         r->output_tokens_emitted(), r->max_output_tokens()});
  }
  return {std::move(reports), std::move(summaries)};
}
} // namespace tokamak
