#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "tokamak/common/clock.h"
#include "tokamak/request/state.h"

namespace tokamak {
using RequestId = std::string;

// A single client request as it flows through Tokamak's lifecycle. Owns a
// RequestLifecycle (state machine + timing milestones) plus the metadata
// needed for scheduling and enforcing output-length limits.
//
// Request is constructible and movable, but -- because RequestLifecycle
// holds a reference to its Clock -- not assignable. Long-lived containers
// (e.g. the scheduler's queues, added in a later milestone) should hold
// Request behind std::unique_ptr<Request> rather than storing Request by
// value in a reassignable container such as a plain std::vector<Request>.
class Request {
public:
  Request(RequestId id, const Clock &clock, Duration deadline_from_now,
          std::size_t max_output_token, int priority = 0);

  Request(RequestId id, const Clock &clock, Duration deadline_from_now,
          std::size_t max_output_token,
          std::vector<std::uint32_t> prompt_token_ids, int priority = 0);

  const RequestId &id() const { return id_; }

  RequestLifecycle &lifecycle() { return lifecycle_; }
  const RequestLifecycle &lifecycle() const { return lifecycle_; }

  // Fixed once at construction time (clock.now() + deadline_from_now) and
  // never recomputed, "admitted deadlines do not move backward."
  TimePoint deadline_at() const { return deadline_at_; }

  int priority() const { return priority_; }

  std::size_t max_output_tokens() const { return max_output_tokens_; }
  std::size_t output_tokens_emitted() const { return output_tokens_emitted_; }

  const std::vector<std::uint32_t> &prompt_token_ids() const {
    return prompt_token_ids_;
  }

  void emit_token(std::uint32_t token_id);

  const std::vector<std::uint32_t> &output_token_ids() const {
    return output_token_ids_;
  }

private:
  RequestId id_;
  RequestLifecycle lifecycle_;
  TimePoint deadline_at_;
  std::size_t max_output_tokens_;
  std::size_t output_tokens_emitted_ = 0;
  std::vector<std::uint32_t> prompt_token_ids_;
  int priority_;
  std::vector<std::uint32_t> output_token_ids_;
};
} // namespace tokamak
