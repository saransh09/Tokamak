#include "tokamak/http/token_channel.h"

namespace tokamak {

TokenChannel::TokenChannel(std::size_t capacity) : capacity_(capacity) {}

bool TokenChannel::try_push(TokenEvent event) {
  std::lock_guard lock(mutex_);
  if (closed_ || buffer_.size() >= capacity_) {
    return false;
  }
  buffer_.push_back(event);
  non_empty_cv_.notify_one();
  return true;
}

std::optional<TokenEvent> TokenChannel::pop() {
  std::unique_lock lock(mutex_);
  non_empty_cv_.wait(lock, [this] { return !buffer_.empty() || closed_; });
  if (buffer_.empty()) {
    return std::nullopt;
  }
  TokenEvent event = buffer_.front();
  buffer_.pop_front();
  return event;
}

void TokenChannel::close() {
  std::lock_guard lock(mutex_);
  closed_ = true;
  non_empty_cv_.notify_all();
}

bool TokenChannel::is_closed() const {
  std::lock_guard lock(mutex_);
  return closed_;
}

std::size_t TokenChannel::size() const {
  std::lock_guard lock(mutex_);
  return buffer_.size();
}

std::size_t TokenChannel::capacity() const { return capacity_; }

} // namespace tokamak
