#include "tokamak/request/request.h"

#include "tokamak/common/clock.h"
#include "tokamak/common/panic.h"

namespace tokamak {

Request::Request(RequestId id, const Clock &clock, Duration deadline_from_now,
                 std::size_t max_output_tokens, int priority)
    : id_(std::move(id)), lifecycle_(clock),
      deadline_at_(clock.now() + deadline_from_now),
      max_output_tokens_(max_output_tokens), priority_(priority) {}

void Request::emit_token() {
    lifecycle_.record_first_token();

    if (output_tokens_emitted_ >= max_output_tokens_) {
        panic("emit_token() called after max_output_tokens already reached");
    }
    ++output_tokens_emitted_;
}
} // namespace tokamak
