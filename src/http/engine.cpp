#include "tokamak/http/engine.h"
#include "tokamak/http/token_channel.h"
#include "tokamak/request/request.h"
#include <chrono>
#include <vector>

namespace tokamak {
Engine::Engine(InferenceBackend &backend, const Clock &clock,
               EngineConfig config)
    : clock_(clock), config_(std::move(config)),
      scheduler_(backend, clock) {}

Engine::~Engine() {
  if (running_)
    stop();
}

void Engine::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }
  thread_ = std::thread(&Engine::run, this);
}

void Engine::stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) {
    return;
  }
  submission_cv_.notify_one();
  thread_.join();
}

bool Engine::is_running() const { return running_.load(); }

bool Engine::submit(Submission submission) {
  if (!running_.load()) {
    return false;
  }
  {
    std::lock_guard lock(submission_mutex_);
    pending_submissions_.push_back(std::move(submission));
  }
  submission_cv_.notify_one();
  return true;
}

void Engine::cancel(const RequestId &id) {
  {
    std::lock_guard lock(submission_mutex_);
    pending_cancels_.push_back(id);
  }
  submission_cv_.notify_one();
}

void Engine::run() {
  while (running_.load()) {
    {
      std::unique_lock lock(submission_mutex_);
      if (pending_submissions_.empty() && active_.empty()) {
        submission_cv_.wait_for(lock, config_.tick_interval, [this] {
          return !running_.load() || !pending_submissions_.empty() ||
                 !pending_cancels_.empty();
        });
      }
    }
    if (!running_.load()) {
      break;
    }
    drain_submissions();
    drain_cancels();

    if (!active_.empty()) {
      scheduler_.tick();
      push_tokens();
      check_backpressure();
      reap_terminal();
    }
  }
}

void Engine::drain_submissions() {
  std::vector<Submission> batch;
  {
    std::lock_guard lock(submission_mutex_);
    batch.swap(pending_submissions_);
  }
  for (auto &sub : batch) {
    auto req =
        std::make_unique<Request>(sub.id, clock_, sub.deadline, sub.max_tokens,
                                  std::move(sub.prompt_token_ids));
    Request &ref = scheduler_.submit(std::move(req));
    active_.emplace(sub.id, ActiveEntry{&ref, sub.channel, 0, std::nullopt});
  }
}

void Engine::drain_cancels() {
  std::vector<RequestId> batch;
  {
    std::lock_guard lock(submission_mutex_);
    std::swap(batch, pending_cancels_);
  }
  for (auto &id : batch) {
    scheduler_.cancel(id);
    if (auto it = active_.find(id); it != active_.end()) {
      it->second.channel->close();
      active_.erase(it);
    }
  }
}

void Engine::push_tokens() {
  for (auto &[id, entry] : active_) {
    std::size_t emitted = entry.request->output_tokens_emitted();
    const auto &token_ids = entry.request->output_token_ids();
    bool is_terminal = entry.request->lifecycle().is_terminal();

    for (std::size_t i = entry.last_seen_tokens; i < emitted; ++i) {
      FinishReason reason = FinishReason::kNone;
      if (is_terminal && i == emitted - 1) {
        reason = (emitted >= entry.request->max_output_tokens())
                     ? FinishReason::kLength
                     : FinishReason::kStop;
      }
      TokenEvent event{token_ids[i], reason};
      if (entry.channel->try_push(event)) {
        entry.last_seen_tokens = i + 1;
        entry.backpressure_since = std::nullopt;
      } else {
        if (!entry.backpressure_since) {
          entry.backpressure_since = std::chrono::steady_clock::now();
        }
        break;
      }
    }
  }
}

void Engine::check_backpressure() {
  auto now = std::chrono::steady_clock::now();
  std::vector<RequestId> to_cancel;

  for (auto &[id, entry] : active_) {
    if (entry.backpressure_since &&
        (now - *entry.backpressure_since) >= config_.backpressure_timeout) {
      to_cancel.push_back(id);
    }
  }

  for (const auto &id : to_cancel) {
    scheduler_.cancel(id);
    active_.at(id).channel->close();
    active_.erase(id);
  }
}

void Engine::reap_terminal() {
  std::vector<RequestId> to_reap;
  for (auto &[id, entry] : active_) {
    if (entry.request->lifecycle().is_terminal() &&
        entry.last_seen_tokens >= entry.request->output_tokens_emitted()) {
      to_reap.push_back(id);
    }
  }

  for (const auto &id : to_reap) {
    active_.at(id).channel->close();
    active_.erase(id);
  }
}

} // namespace tokamak
