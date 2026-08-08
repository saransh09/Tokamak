#include "tokamak/backend/mock_backend.h"
#include "tokamak/common/panic.h"

namespace tokamak {

BackendCapabilities MockBackend::capabilities() const {
  return BackendCapabilities{
      .supports_continuous_batching = true,
      .supports_prefix_sharing = false,
      .supports_kv_export = false,
      .max_context_tokens = 8192,
  };
}

TokenizedPrompt MockBackend::tokenize(std::string_view text) {
  TokenizedPrompt result;
  result.token_ids.reserve(text.size());
  for (char c : text) {
    // char can be signed on some platforms, hence the double cast
    result.token_ids.push_back(
        static_cast<std::uint32_t>(static_cast<unsigned char>(c)));
  }
  return result;
}

PrefillResult MockBackend::prefill(const PrefillBatch &batch) {
  PrefillResult result;
  result.outcomes.reserve(batch.sequences.size());

  for (const auto &request : batch.sequences) {
    clock_.advance(per_token_prefill_cost_ * request.prompt.token_ids.size());

    if (!pending_prefill_errors_.empty()) {
      BackendError error = std::move(pending_prefill_errors_.front());
      pending_prefill_errors_.pop_front();
      result.outcomes.push_back(std::unexpected(std::move(error)));
      continue;
    }

    SequenceHandle handle(next_sequence_id_++);
    sequences_.emplace(handle.id(), SequenceState{});

    result.outcomes.push_back(PrefillOutcome{.handle = handle});
  }
  return result;
}

DecodeResult MockBackend::decode(const DecodeBatch &batch) {
  clock_.advance(per_decode_cost_);

  DecodeResult result;
  result.outcomes.reserve(batch.sequences.size());

  for (const auto &request : batch.sequences) {

    if (!pending_decode_errors_.empty()) {
      BackendError error = std::move(pending_decode_errors_.front());
      pending_decode_errors_.pop_front();
      result.outcomes.push_back(std::unexpected(std::move(error)));
      continue;
    }

    auto it = sequences_.find(request.handle.id());
    if (it == sequences_.end()) {
      panic("decode() called with unknown or already-released SequenceHandle");
    }

    SequenceState &state = it->second;
    ++state.tokens_emitted;

    bool finished = state.finished_after_tokens.has_value() &&
                    state.tokens_emitted >= *state.finished_after_tokens;

    result.outcomes.push_back(DecodedToken{
        .token_id = state.tokens_emitted,
        .finished = finished,
    });
  }
  return result;
}

void MockBackend::release(SequenceHandle sequence) noexcept {
  auto it = sequences_.find(sequence.id());
  if (it == sequences_.end()) {
    panic("release() called with unknown or already-released SequenceHandle");
  }
  sequences_.erase(it);
}

void MockBackend::configure_eos(SequenceHandle handle,
                                std::uint32_t after_tokens) {
  auto it = sequences_.find(handle.id());
  if (it == sequences_.end()) {
    panic("configure_eos() called with unknown or already-released "
          "SequenceHandle");
  }
  it->second.finished_after_tokens = after_tokens;
}

void MockBackend::fail_next_prefill(BackendError error) {
  pending_prefill_errors_.push_back(std::move(error));
}

void MockBackend::fail_next_decode(BackendError error) {
  pending_decode_errors_.push_back(std::move(error));
}

} // namespace tokamak
