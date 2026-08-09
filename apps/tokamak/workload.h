#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tokamak {

// One record from the Milestone 1 JSONL workload file (ADR-008 Decision 4).
// Field names match the JSONL schema exactly:
// {"id": "req-000001", "arrival_ms": 143.2, "prompt_token_ids": [1,2,3],
//  "max_tokens": 128, "deadline_ms": 2000}
struct WorkloadRequest {
  std::string id;
  double arrival_ms = 0.0;
  std::vector<std::uint32_t> prompt_token_ids;
  std::size_t max_tokens = 0;
  double deadline_ms = 0.0;
};

// Reads a JSONL workload file (one JSON object per line) and returns its
// records sorted ascending by arrival_ms -- run_simulation()'s event loop
// relies on this ordering and does not re-sort.
//
// Aborts the process (panic) on a malformed line or a missing/wrong-typed
// required field: a broken workload file is an input error the caller should
// fix, not a runtime condition the simulation should try to recover from.
std::vector<WorkloadRequest> load_workload(const std::string &path);
} // namespace tokamak
