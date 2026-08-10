#include "tokamak/request/request.h"

#include "tokamak/common/clock.h"
#include "tokamak/common/panic.h"

namespace tokamak {

Request::Request(RequestId id, const Clock &clock, Duration deadline_from_now,
                 std::size_t max_output_tokens, int priority)
    : id_(std::move(id)), lifecycle_(clock),
      deadline_at_(clock.now() + deadline_from_now),
      max_output_tokens_(max_output_tokens), priority_(priority) {}

Request::Request(RequestId id, const Clock &clock, Duration deadline_from_now,
                 std::size_t max_output_tokens,
                 std::vector<std::uint32_t> prompt_token_ids, int priority)
    : id_(std::move(id)), lifecycle_(clock),
      deadline_at_(clock.now() + deadline_from_now),
      max_output_tokens_(max_output_tokens),
      prompt_token_ids_(std::move(prompt_token_ids)), priority_(priority) {}

void Request::emit_token(std::uint32_t token_id) {
  lifecycle_.record_first_token();

  if (output_tokens_emitted_ >= max_output_tokens_) {
    panic("emit_token() called after max_output_tokens already reached");
  }
  ++output_tokens_emitted_;
  output_token_ids_.push_back(token_id);
}
} // namespace tokamak
