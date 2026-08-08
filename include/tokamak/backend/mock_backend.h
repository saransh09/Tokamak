#pragma once

#include "tokamak/backend/backend.h"
#include "tokamak/backend/error.h"
#include "tokamak/common/clock.h"
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
namespace tokamak {

// Deterministic backend for tests and simulation (project.md Section
// 8.4). Never loads a real model. Time cost is modeled by advancing a
// FakeClock synchronously rather than sleeping -- see
// learning/learnings_004.md, for why prefill cost scales with
// batch tokens but decode cost does not scale with batch width.
class MockBackend final : public InferenceBackend {
public:
  MockBackend(FakeClock &clock, Duration per_token_prefill_cost,
              Duration per_decode_cost)
      : clock_(clock), per_token_prefill_cost_(per_token_prefill_cost),
        per_decode_cost_(per_decode_cost) {}

  BackendCapabilities capabilities() const override;
  TokenizedPrompt tokenize(std::string_view text) override;
  PrefillResult prefill(const PrefillBatch &batch) override;
  DecodeResult decode(const DecodeBatch &batch) override;
  void release(SequenceHandle sequence) noexcept override;
  void configure_eos(SequenceHandle handle, std::uint32_t after_tokens);
  void fail_next_prefill(BackendError error);
  void fail_next_decode(BackendError error);

private:
  FakeClock &clock_;
  Duration per_token_prefill_cost_;
  Duration per_decode_cost_;

  struct SequenceState {
    std::uint32_t tokens_emitted = 0;
    std::optional<std::uint32_t>
        finished_after_tokens; // unset = never self-terminate
  };
  std::uint64_t next_sequence_id_ = 0;
  std::unordered_map<std::uint64_t, SequenceState> sequences_;

  // Test-only failure injection (opt-in, default empty -- zero behavior
  // change unless called). Each queued error is consumed exactly once,
  // in the order prefill()/decode() processes batch.sequences (i.e.
  // per-request-in-batch-order, not per-batch), letting a test target
  // one specific sequence within a mixed batch.
  std::deque<BackendError> pending_prefill_errors_;
  std::deque<BackendError> pending_decode_errors_;
};

} // namespace tokamak
