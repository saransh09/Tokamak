#include "tokamak/request/state.h"

#include <cstdio>
#include <cstdlib>

namespace tokamak {
namespace {
// Always-on invariant check: deliberately NOT assert(), which is compiled
// out under NDEBUG. An illgegal state transition indicates corrupted
// scheduling logic
// continuting would be unsafe in any build type
[[noreturn]] void invariant_violation(std::string_view message) {
  std::fprintf(stderr, "tokamak: invariant violation: %.*s\n",
               static_cast<int>(message.size()), message.data());
  std::abort();
}
} // namespace

std::string_view to_string(RequestState state) {
  switch (state) {
  case RequestState::kReceived:
    return "Received";
  case RequestState::kAdmitted:
    return "Admitted";
  case RequestState::kWaitingPrefill:
    return "WaitingPrefill";
  case RequestState::kPrefilling:
    return "Prefilling";
  case RequestState::kWaitingDecode:
    return "WaitingDecode";
  case RequestState::kDecoding:
    return "Decoding";
  case RequestState::kCompleted:
    return "Completed";
  case RequestState::kRejected:
    return "Rejected";
  case RequestState::kCancelled:
    return "Cancelled";
  case RequestState::kFailed:
    return "Failed";
  }
  invariant_violation("unknown RequestState");
}

bool is_terminal(RequestState state) {
  switch (state) {
  case RequestState::kCompleted:
  case RequestState::kRejected:
  case RequestState::kCancelled:
  case RequestState::kFailed:
    return true;
  default:
    return false;
  }
}

bool is_valid_transition(RequestState from, RequestState to) {
  switch (from) {
  case RequestState::kReceived:
    return to == RequestState::kAdmitted || to == RequestState::kRejected ||
           to == RequestState::kCancelled;
  case RequestState::kAdmitted:
    return to == RequestState::kWaitingPrefill ||
           to == RequestState::kCancelled;
  case RequestState::kWaitingPrefill:
    return to == RequestState::kPrefilling || to == RequestState::kCancelled;
  case RequestState::kPrefilling:
    return to == RequestState::kWaitingDecode || to == RequestState::kFailed ||
           to == RequestState::kCancelled;
  case RequestState::kWaitingDecode:
    return to == RequestState::kDecoding || to == RequestState::kCancelled;
  case RequestState::kDecoding:
    return to == RequestState::kCompleted || to == RequestState::kFailed ||
           to == RequestState::kCancelled;
  // Terminal states: no outgoing transitions.
  case RequestState::kCompleted:
  case RequestState::kRejected:
  case RequestState::kCancelled:
  case RequestState::kFailed:
    return false;
  }
  return false;
}

RequestLifecycle::RequestLifecycle(const Clock &clock)
    : clock_(clock), entered_at_(clock.now()) {}

void RequestLifecycle::transition_to(RequestState to) {
  if (!is_valid_transition(state_, to)) {
    invariant_violation("illegal request statet transition attempted");
  }

  state_ = to;
  entered_at_ = clock_.now();

  switch (to) {
  case RequestState::kAdmitted:
    admitted_at_ = entered_at_;
    break;
  case RequestState::kCompleted:
    completed_at_ = entered_at_;
    break;
  default:
    break;
  }
}

void RequestLifecycle::cancel() {
  if (is_terminal()) {
    return; // Idempotent: already terminal, nothing to do.
  }
  transition_to(RequestState::kCancelled);
}

void RequestLifecycle::record_first_token() {
  if (state_ != RequestState::kDecoding) {
    invariant_violation("record_first_token() called outside Decoding state.");
    ;
  }
  if (!first_token_at_.has_value()) {
    first_token_at_ = clock_.now();
  }
}

} // namespace tokamak
