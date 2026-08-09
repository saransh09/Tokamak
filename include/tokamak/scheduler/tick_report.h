#pragma once

#include "tokamak/common/clock.h"
namespace tokamak {
struct TickReport {
  TimePoint timestamp;

  std::size_t expired_count = 0;

  std::size_t prefill_attempted = 0;
  std::size_t prefill_succeeded = 0;
  std::size_t prefill_failed = 0;
  std::size_t prefill_tokens = 0; // sum of prompt tokens across the batch

  std::size_t decode_attempted = 0;
  std::size_t decode_tokens = 0;
  std::size_t decode_completed = 0; // EOS or max_output_tokens reached
  std::size_t decode_failed = 0;
};

} // namespace tokamak
