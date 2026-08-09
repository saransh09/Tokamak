#include "workload.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "tokamak/common/panic.h"

namespace tokamak {

std::vector<WorkloadRequest> load_workload(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    panic("load_workload: could not open file: " + path);
  }

  std::vector<WorkloadRequest> requests;
  std::string line;
  std::size_t line_number = 0;

  while (std::getline(in, line)) {
    ++line_number;
    if (line.empty()) {
      continue; // tolerate trailing/blank line
    }

    nlohmann::json obj;
    try {
      obj = nlohmann::json::parse(line);
    } catch (const nlohmann::json::parse_error &) {
      panic("load_workload: malformed JSON on line " +
            std::to_string(line_number) + " of " + path);
    }

    WorkloadRequest req;
    try {
      req.id = obj.at("id").get<std::string>();
      req.arrival_ms = obj.at("arrival_ms").get<double>();
      req.prompt_token_ids =
          obj.at("prompt_token_ids").get<std::vector<std::uint32_t>>();
      req.max_tokens = obj.at("max_tokens").get<std::size_t>();
      req.deadline_ms = obj.at("deadline_ms").get<double>();
    } catch (nlohmann::json::exception &) {
      panic("load_workload: missing/wrong-typed field on line " +
            std::to_string(line_number) + " of " + path);
    }
    requests.push_back(req);
  }
  std::sort(requests.begin(), requests.end(),
            [](const WorkloadRequest &a, const WorkloadRequest &b) {
              return a.arrival_ms < b.arrival_ms;
            });
  return requests;
}
} // namespace tokamak
