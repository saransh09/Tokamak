#pragma once

#include "tokamak/backend/backend.h"
#include "tokamak/common/clock.h"
#include "tokamak/http/token_channel.h"
#include "tokamak/request/request.h"
#include "tokamak/scheduler/fifo_scheduler.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>
namespace tokamak {
struct EngineConfig {
  std::chrono::milliseconds tick_interval{10};
  std::chrono::milliseconds backpressure_timeout{5000};
};

struct Submission {
  RequestId id;
  std::vector<std::uint32_t> prompt_token_ids;
  std::size_t max_tokens;
  Duration deadline;
  TokenChannel *channel; // non-owning; caller owns channel lifetime
};

class Engine {
public:
  Engine(InferenceBackend &backend, const Clock &clock, EngineConfig config);
  ~Engine();

  void start();
  void stop();
  bool is_running() const;

  // Thread-safe. Returns false if engine is not running.
  bool submit(Submission submission);
  void cancel(const RequestId &id);

private:
  void run();
  void drain_submissions();
  void drain_cancels();
  void push_tokens();
  void check_backpressure();
  void reap_terminal();

  const Clock &clock_;
  EngineConfig config_;
  FifoScheduler scheduler_;

  std::thread thread_;
  std::atomic<bool> running_{false};

  // Submission queue (network -> engine)
  mutable std::mutex submission_mutex_;
  std::condition_variable submission_cv_;
  std::vector<Submission> pending_submissions_;
  std::vector<RequestId> pending_cancels_;

  // Engine-thread-only state (no lock needed)
  struct ActiveEntry {
    Request *request;
    TokenChannel *channel;
    std::size_t last_seen_tokens = 0;
    std::optional<std::chrono::steady_clock::time_point> backpressure_since;
  };

  std::unordered_map<RequestId, ActiveEntry> active_;
};
} // namespace tokamak
