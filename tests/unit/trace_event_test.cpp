#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <sstream>

#include "tokamak/telemetry/trace_event.h"
#include "tokamak/telemetry/trace_writer.h"

using tokamak::make_trace_event;
using tokamak::TickReport;
using tokamak::TraceEvent;
using tokamak::TraceWriter;

TEST_CASE("make_trace_event maps TickReport fields correctly",
          "[trace_event]") {
  tokamak::TimePoint start{};

  TickReport report;
  report.timestamp = start + std::chrono::milliseconds(5);
  report.prefill_attempted = 3;
  report.prefill_tokens = 42;
  report.decode_attempted = 7;
  report.decode_tokens = 7;

  TraceEvent event =
      make_trace_event(report, /*iteration=*/18291, "fifo", start);

  REQUIRE(event.iteration == 18291);
  REQUIRE(event.timestamp_ns == 5'000'000); // 5ms in ns
  REQUIRE(event.policy == "fifo");
  REQUIRE(event.runnable_prefill == 3);
  REQUIRE(event.runnable_decode == 7);
  REQUIRE(event.selected_sequences == 7);
  REQUIRE(event.prefill_tokens == 42);
  REQUIRE(event.decode_tokens == 7);
}

TEST_CASE("TraceEvent serializes to the honest-subset JSON schema only",
          "[trace_event]") {
  tokamak::TimePoint start{};
  TickReport report;
  report.timestamp = start;

  TraceEvent event = make_trace_event(report, 0, "fifo", start);
  nlohmann::json json_obj = event;

  REQUIRE(json_obj["policy"] == "fifo");
  REQUIRE(json_obj.contains("runnable_prefill"));
  REQUIRE(json_obj.contains("selected_sequences"));

  // ADR-008: fields for subsystems that don't exist yet must be absent,
  // not stubbed with placeholder values.
  REQUIRE_FALSE(json_obj.contains("kv_pages_free"));
  REQUIRE_FALSE(json_obj.contains("earliest_slack_ms"));
  REQUIRE_FALSE(json_obj.contains("decision_us"));
}

TEST_CASE("TraceWriter writes one JSON line per event", "[trace_writer]") {
  std::ostringstream out;
  TraceWriter writer(out);

  tokamak::TimePoint start{};
  TickReport report1;
  report1.timestamp = start;
  TickReport report2;
  report2.timestamp = start + std::chrono::milliseconds(1);

  writer.write(make_trace_event(report1, 0, "fifo", start));
  writer.write(make_trace_event(report2, 1, "fifo", start));

  std::string output = out.str();
  REQUIRE(std::count(output.begin(), output.end(), '\n') == 2);
}
