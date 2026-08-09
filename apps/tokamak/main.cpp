#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>

#include "analysis.h"
#include "simulation.h"
#include "tokamak/common/clock.h"
#include "tokamak/telemetry/trace_writer.h"

namespace {

using tokamak::Duration;
using tokamak::TimePoint;

// Returns NaN if either milestone was never reached (RequestSummary's
// std::optional fields stay nullopt for unreached states, per ADR-009) --
// printed as "N/A" rather than treated as zero, matching the "absent, not
// stubbed" discipline used throughout this project (ADR-008/ADR-009).
double duration_ms_or_nan(std::optional<TimePoint> from,
                          std::optional<TimePoint> to) {
  if (!from || !to) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return std::chrono::duration<double, std::milli>(*to - *from).count();
}

void print_ms(const char *label, double value_ms) {
  std::cout << "    " << label << ": ";
  if (std::isnan(value_ms)) {
    std::cout << "N/A";
  } else {
    std::cout << value_ms << "ms";
  }
  std::cout << "\n";
}

void print_usage(const char *program_name) {
  std::cerr << "usage: " << program_name
            << " <workload.jsonl> [--trace-out <path>]\n";
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  tokamak::SimulationConfig config;
  config.workload_path = argv[1];

  std::optional<std::string> trace_out_path;
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--trace-out") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 1;
      }
      trace_out_path = argv[++i];
    } else {
      print_usage(argv[0]);
      return 1;
    }
  }

  std::ofstream trace_file;
  std::unique_ptr<tokamak::TraceWriter> trace_writer;
  if (trace_out_path) {
    trace_file.open(*trace_out_path);
    if (!trace_file) {
      std::cerr << "error: could not open trace output file: "
                << *trace_out_path << "\n";
      return 1;
    }
    trace_writer = std::make_unique<tokamak::TraceWriter>(trace_file);
  }

  tokamak::SimulationResult result =
      tokamak::run_simulation(config, trace_writer.get());

  // -- per-request breakdown (state transitions + queue/prefill/decode/
  // completion times, per project.md SS29) --
  std::cout << "=== Per-request breakdown ===\n";
  for (const auto &r : result.requests) {
    std::cout << "  " << r.id << " [" << tokamak::to_string(r.final_state)
              << "]\n";
    print_ms("queue_time",
             duration_ms_or_nan(r.waiting_prefill_at, r.prefilling_at));
    print_ms("prefill_time",
             duration_ms_or_nan(r.prefilling_at, r.waiting_decode_at));
    print_ms("decode_time", duration_ms_or_nan(r.decoding_at, r.completed_at));
    print_ms("ttft", duration_ms_or_nan(r.admitted_at, r.first_token_at));
    print_ms("e2e", duration_ms_or_nan(r.admitted_at, r.completed_at));
  }

  // -- batch composition per iteration --
  std::cout << "\n=== Batch composition per iteration ===\n";
  for (std::size_t i = 0; i < result.tick_reports.size(); ++i) {
    const auto &tr = result.tick_reports[i];
    std::cout << "  iter " << i
              << ": prefill_attempted=" << tr.prefill_attempted
              << " prefill_succeeded=" << tr.prefill_succeeded
              << " prefill_failed=" << tr.prefill_failed
              << " prefill_tokens=" << tr.prefill_tokens
              << " decode_attempted=" << tr.decode_attempted
              << " decode_tokens=" << tr.decode_tokens
              << " decode_completed=" << tr.decode_completed
              << " decode_failed=" << tr.decode_failed
              << " expired=" << tr.expired_count << "\n";
  }

  // -- TTFT / E2E distributions --
  tokamak::LatencyStats latency =
      tokamak::compute_latency_stats(result.requests);
  std::cout << "\n=== Latency (ms) ===\n";
  std::cout << "  TTFT: p50=" << latency.ttft_p50 << " p90=" << latency.ttft_p90
            << " p99=" << latency.ttft_p99 << " min=" << latency.ttft_min
            << " mean=" << latency.ttft_mean << " max=" << latency.ttft_max
            << "\n";
  std::cout << "  E2E:  p50=" << latency.e2e_p50 << " p90=" << latency.e2e_p90
            << " p99=" << latency.e2e_p99 << " min=" << latency.e2e_min
            << " mean=" << latency.e2e_mean << " max=" << latency.e2e_max
            << "\n";

  // -- throughput / goodput (ADR-010) --
  // sim_wall_time is derived from the last tick's timestamp, relative to the
  // same TimePoint{} epoch run_simulation() treats as "sim start" throughout
  // -- SimulationResult does not carry an explicit end-of-sim field today.
  Duration sim_wall_time =
      result.tick_reports.empty()
          ? Duration::zero()
          : (result.tick_reports.back().timestamp - TimePoint{});
  tokamak::ThroughputStats throughput =
      tokamak::compute_throughput_stats(result.requests, sim_wall_time);
  std::cout << "\n=== Throughput ===\n";
  std::cout << "  throughput: " << throughput.throughput_tokens_per_sec
            << " tok/s\n";
  std::cout << "  goodput:    " << throughput.goodput_tokens_per_sec
            << " tok/s\n";
  std::cout << "  SLO attainment: " << (throughput.slo_attainment_rate * 100.0)
            << "%\n";

  // -- invariant-check result --
  auto violations = tokamak::check_invariants(result.requests);
  std::cout << "\n=== Invariant check ===\n";
  if (violations.empty()) {
    std::cout << "  PASSED\n";
  } else {
    std::cout << "  FAILED (" << violations.size() << " violation(s)):\n";
    for (const auto &v : violations) {
      std::cout << "    " << v.request_id << ": " << v.description << "\n";
    }
  }

  return violations.empty() ? 0 : 1;
}
