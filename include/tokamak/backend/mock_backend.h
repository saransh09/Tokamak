#pragma once

#include "tokamak/backend/backend.h"
#include "tokamak/common/clock.h"
#include <cstdint>
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
};

} // namespace tokamak
