#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "tokamak/common/clock.h"

namespace tokamak {
// The lifecycle stages a request moves through
enum class RequestState : std::uint8_t {
  kReceived,
  kAdmitted,
  kWaitingPrefill,
  kPrefilling,
  kWaitingDecode,
  kDecoding,
  kCompleted,
  kRejected,
  kCancelled,
  kFailed,
};

// Human-readable name, useful for logs/traces/tests
std::string_view to_string(RequestState state);

// True for the four terminal states: Completed, Rejected, Cancelled, Failed.
bool is_terminal(RequestState state);

// Return true if `to` is a legal transition target from `from`
bool is_valid_transition(RequestState from, RequestState to);

// Tracks a single request's lifecycle state and the timestamps needed to
// compute TTFT/E2E later. Not thread-safe by itself -- callers (e.g. the
// scheduler) are responsible for serializing access to a given request.
class RequestLifecycle {
public:
  explicit RequestLifecycle(const Clock &clock);

  RequestState state() const { return state_; }

  // Timestamp of the most recent transition (i.e. when the current state
  // began).
  TimePoint entered_at() const { return entered_at_; }

  bool is_terminal() const { return tokamak::is_terminal(state_); }

  // Attempts teh transition. Aborts the process if `to` is not reachable
  // from the current state: an illegal transition is a programmer/
  // invariant bug, not an expected runtime condition, and we do not want
  // this check silently disabled in release builds (unlike assert())
  void transition_to(RequestState to);

  // Cancellation is explicitly idempotent
  // cancelling cancel() on an already-terminal request (whether already
  // cancelled or terminal via another path) is a safe no-op
  void cancel();

  // Records for the first output token has been produced. This is not a
  // state transition (Decoding covers many tokens) but a milestone event
  // within the Decoding state, used to compute TTFT.
  void record_first_token();

  std::optional<TimePoint> admitted_at() const { return admitted_at_; }
  std::optional<TimePoint> first_token_at() const { return first_token_at_; }
  std::optional<TimePoint> completed_at() const { return completed_at_; }

  // Returns the timestamp `state` was entered, or std::nullopt if this
  // request never visited that state on its path through the lifecycle
  // (ADR-009). Unlike admitted_at()/first_token_at()/completed_at(), this
  // covers every state uniformly rather than special-casing a few.
  std::optional<TimePoint> entered_at(RequestState state) const;

private:
  const Clock &clock_;
  RequestState state_ = RequestState::kReceived;
  TimePoint entered_at_;

  std::optional<TimePoint> admitted_at_;
  std::optional<TimePoint> first_token_at_;
  std::optional<TimePoint> completed_at_;

  // Per-state entry timestamps (ADR-009), populated in transition_to().
  // At most one entry per state -- see ADR-009's Alternatives Considered
  // for why an unordered_map is enough and a full ordered history isn't
  // needed yet.
  std::unordered_map<RequestState, TimePoint> state_entered_at_;
};
} // namespace tokamak
