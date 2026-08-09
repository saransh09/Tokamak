#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "workload.h"

using tokamak::load_workload;

namespace {
// Writes `content` to a fresh temp file and returns its path. Each test
// gets a unique filename (via a static counter) so parallel/repeated runs
// never collide -- load_workload() only needs a std::string path, so a
// real file on disk is simpler than any in-memory-stream indirection.
std::string write_temp_workload(const std::string &content) {
  static int counter = 0;
  auto path = std::filesystem::temp_directory_path() /
              ("tokamak_workload_test_" + std::to_string(counter++) + ".jsonl");
  std::ofstream out(path);
  out << content;
  out.close();
  return path.string();
}
} // namespace

// NOTE: load_workload()'s error paths (unopenable file, malformed JSON,
// missing/wrong-typed field) all call panic(), which calls std::abort() --
// Catch2 has no way to catch that without taking down the whole test
// binary (see mock_backend_test.cpp:190-191 for the same documented
// limitation). Those paths are intentionally not covered here; only the
// success paths are testable in-process.

TEST_CASE("load_workload parses all five fields correctly", "[workload]") {
  auto path = write_temp_workload(
      R"({"id": "req-1", "arrival_ms": 12.5, "prompt_token_ids": [1, 2, 3], "max_tokens": 128, "deadline_ms": 2000.0})"
      "\n");

  auto result = load_workload(path);
  std::filesystem::remove(path);

  REQUIRE(result.size() == 1);
  REQUIRE(result[0].id == "req-1");
  REQUIRE(result[0].arrival_ms == 12.5);
  REQUIRE(result[0].prompt_token_ids == std::vector<std::uint32_t>{1, 2, 3});
  REQUIRE(result[0].max_tokens == 128);
  REQUIRE(result[0].deadline_ms == 2000.0);
}

TEST_CASE("load_workload sorts records ascending by arrival_ms regardless "
          "of file order",
          "[workload]") {
  auto path = write_temp_workload(
      R"({"id": "third", "arrival_ms": 30.0, "prompt_token_ids": [1], "max_tokens": 1, "deadline_ms": 100.0})"
      "\n"
      R"({"id": "first", "arrival_ms": 5.0, "prompt_token_ids": [1], "max_tokens": 1, "deadline_ms": 100.0})"
      "\n"
      R"({"id": "second", "arrival_ms": 10.0, "prompt_token_ids": [1], "max_tokens": 1, "deadline_ms": 100.0})"
      "\n");

  auto result = load_workload(path);
  std::filesystem::remove(path);

  REQUIRE(result.size() == 3);
  REQUIRE(result[0].id == "first");
  REQUIRE(result[1].id == "second");
  REQUIRE(result[2].id == "third");
}

TEST_CASE("load_workload tolerates blank lines between records",
          "[workload]") {
  auto path = write_temp_workload(
      "\n"
      R"({"id": "req-1", "arrival_ms": 0.0, "prompt_token_ids": [1], "max_tokens": 1, "deadline_ms": 100.0})"
      "\n"
      "\n"
      R"({"id": "req-2", "arrival_ms": 1.0, "prompt_token_ids": [2], "max_tokens": 1, "deadline_ms": 100.0})"
      "\n"
      "\n");

  auto result = load_workload(path);
  std::filesystem::remove(path);

  REQUIRE(result.size() == 2);
  REQUIRE(result[0].id == "req-1");
  REQUIRE(result[1].id == "req-2");
}

TEST_CASE("load_workload returns an empty vector for a file with no records",
          "[workload]") {
  auto path = write_temp_workload("\n\n");

  auto result = load_workload(path);
  std::filesystem::remove(path);

  REQUIRE(result.empty());
}

TEST_CASE("load_workload handles an empty prompt_token_ids array",
          "[workload]") {
  auto path = write_temp_workload(
      R"({"id": "req-1", "arrival_ms": 0.0, "prompt_token_ids": [], "max_tokens": 5, "deadline_ms": 100.0})"
      "\n");

  auto result = load_workload(path);
  std::filesystem::remove(path);

  REQUIRE(result.size() == 1);
  REQUIRE(result[0].prompt_token_ids.empty());
}
