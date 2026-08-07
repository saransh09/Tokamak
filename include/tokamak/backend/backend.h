#pragma once

#include "tokamak/backend/error.h"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace tokamak {

// What a backend can do. Capabilities are queried, never guessed
// (project.md Section 8.4) -- callers must check these before relying on
// a feature rather than assuming every backend behaves identically.
struct BackendCapabilities {
  bool supports_continuous_batching;
  bool supports_prefix_sharing;
  bool supports_kv_export;
  std::size_t max_context_tokens;
  // supports_speculative_verification deferred to Milestone 6 with verify()
  // verify() also omitted for now.
};

struct TokenizedPrompt {
  std::vector<std::uint32_t> token_ids;
};

// Opaque identifier for a sequence the backend is tracking. Only
// prefill() constructs one (via a PrefillOutcome); callers must treat it
// as opaque and pass it back unmodified to decode()/release().
class SequenceHandle {
public:
  explicit SequenceHandle(std::uint64_t id) : id_(id) {}

  std::uint64_t id() const { return id_; }

  friend bool operator==(const SequenceHandle &,
                         const SequenceHandle &) = default;

private:
  std::uint64_t id_;
};

// --- Prefill -----------------------------------------------------------

struct PrefillRequest {
  TokenizedPrompt prompt;
};

struct PrefillBatch {
  std::vector<PrefillRequest> sequences;
};

struct PrefillOutcome {
  SequenceHandle handle;
};

// outcomes[i] corresponds to batch.sequences[i]. One sequence failing
// does not fail the others -- see learning/learnings_004.md (TODO) for
// why batch-level partial failure matters for continuous batching.
struct PrefillResult {
  std::vector<std::expected<PrefillOutcome, BackendError>> outcomes;
};

// --- Decode --------------------------------------------------------------

struct DecodeRequest {
  SequenceHandle handle;
};

struct DecodeBatch {
  std::vector<DecodeRequest> sequences;
};

struct DecodedToken {
  std::uint32_t token_id;

  // True if the backend's model itself decided generation is complete
  // (EOS token), independent of Tokamak's own max_output_tokens /
  // stop-sequence checks (project.md Section 7: EOS, stop, and limit are
  // three distinct terminating conditions).
  bool finished;
};

struct DecodeResult {
  std::vector<std::expected<DecodedToken, BackendError>> outcomes;
};

// Narrow backend boundary (project.md Section 8.4): Tokamak owns
// scheduling; the backend owns tensor/model execution. verify() for
// speculative decoding is deferred to Milestone 6.
class InferenceBackend {
public:
  virtual ~InferenceBackend() = default;
  virtual BackendCapabilities capabilities() const = 0;
  virtual TokenizedPrompt tokenize(std::string_view text) = 0;
  virtual PrefillResult prefill(const PrefillBatch &batch) = 0;
  virtual DecodeResult decode(const DecodeBatch &batch) = 0;
  virtual void release(SequenceHandle sequence) noexcept = 0;
  // verify() deferred
};

} // namespace tokamak
