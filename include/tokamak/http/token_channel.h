#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
namespace tokamak {

enum class FinishReason : std::uint8_t {
  kNone,   // not finished yet (normal token)
  kStop,   // EOS token from backend
  kLength, // max_tokens reached
};

struct TokenEvent {
  std::uint32_t token_id;
  FinishReason finish_reason = FinishReason::kNone;
};

class TokenChannel {
public:
  explicit TokenChannel(std::size_t capacity);

  bool try_push(TokenEvent event); // non-blocking, false if full/closed
  std::optional<TokenEvent> pop(); // blocks until available or closed
  void close();                    // wakes blocked consumer

  bool is_closed() const;
  std::size_t size() const;
  std::size_t capacity() const;

private:
  std::deque<TokenEvent> buffer_;
  mutable std::mutex mutex_;
  std::condition_variable non_empty_cv_;
  bool closed_ = false;
  std::size_t capacity_;
};
} // namespace tokamak
