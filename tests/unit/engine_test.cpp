#include <catch2/catch_test_macros.hpp>

#include "tokamak/backend/mock_backend.h"
#include "tokamak/common/clock.h"
#include "tokamak/http/engine.h"
#include "tokamak/http/token_channel.h"
#include <thread>
#include <vector>

using namespace std::chrono_literals;

TEST_CASE("Engine submits request and receives all tokens", "[engine]") {
  tokamak::FakeClock clock;
  tokamak::MockBackend backend(clock, 1ms, 1ms);
  tokamak::EngineConfig config{.tick_interval = 0ms,
                               .backpressure_timeout = 5000ms};
  tokamak::Engine engine(backend, clock, config);
  tokamak::TokenChannel channel(64);

  engine.start();
  engine.submit(tokamak::Submission{
      .id = "req-1",
      .prompt_token_ids = {10, 20, 30},
      .max_tokens = 3,
      .deadline = 10000ms,
      .channel = &channel,
  });

  std::vector<tokamak::TokenEvent> events;
  while (true) {
    auto ev = channel.pop();
    if (!ev)
      break;
    events.push_back(*ev);
  }

  engine.stop();

  REQUIRE(events.size() == 3);
  REQUIRE(events[0].token_id == 1);
  REQUIRE(events[1].token_id == 2);
  REQUIRE(events[2].token_id == 3);
  REQUIRE(events[0].finish_reason == tokamak::FinishReason::kNone);
  REQUIRE(events[1].finish_reason == tokamak::FinishReason::kNone);
  REQUIRE(events[2].finish_reason == tokamak::FinishReason::kLength);
}

TEST_CASE("Engine cancel closes channel", "[engine]") {
  tokamak::FakeClock clock;
  tokamak::MockBackend backend(clock, 1ms, 1ms);
  tokamak::EngineConfig config{.tick_interval = 0ms,
                               .backpressure_timeout = 5000ms};
  tokamak::Engine engine(backend, clock, config);
  tokamak::TokenChannel channel(64);

  engine.start();
  engine.submit(tokamak::Submission{
      .id = "req-cancel",
      .prompt_token_ids = {1, 2},
      .max_tokens = 100,
      .deadline = 10000ms,
      .channel = &channel,
  });

  auto first = channel.pop();
  REQUIRE(first.has_value());

  engine.cancel("req-cancel");

  while (true) {
    auto ev = channel.pop();
    if (!ev)
      break;
  }

  engine.stop();
  REQUIRE(channel.is_closed());
}

TEST_CASE("Engine handles multiple concurrent requests", "[engine]") {
  tokamak::FakeClock clock;
  tokamak::MockBackend backend(clock, 1ms, 1ms);
  tokamak::EngineConfig config{.tick_interval = 0ms,
                               .backpressure_timeout = 5000ms};
  tokamak::Engine engine(backend, clock, config);

  tokamak::TokenChannel ch1(64);
  tokamak::TokenChannel ch2(64);
  tokamak::TokenChannel ch3(64);

  engine.start();
  engine.submit(tokamak::Submission{
      .id = "r1", .prompt_token_ids = {1}, .max_tokens = 2,
      .deadline = 10000ms, .channel = &ch1});
  engine.submit(tokamak::Submission{
      .id = "r2", .prompt_token_ids = {2}, .max_tokens = 2,
      .deadline = 10000ms, .channel = &ch2});
  engine.submit(tokamak::Submission{
      .id = "r3", .prompt_token_ids = {3}, .max_tokens = 2,
      .deadline = 10000ms, .channel = &ch3});

  auto drain = [](tokamak::TokenChannel &ch) {
    std::vector<tokamak::TokenEvent> events;
    while (auto ev = ch.pop()) {
      events.push_back(*ev);
    }
    return events;
  };

  auto e1 = drain(ch1);
  auto e2 = drain(ch2);
  auto e3 = drain(ch3);

  engine.stop();

  REQUIRE(e1.size() == 2);
  REQUIRE(e2.size() == 2);
  REQUIRE(e3.size() == 2);
  REQUIRE(e1.back().finish_reason == tokamak::FinishReason::kLength);
  REQUIRE(e2.back().finish_reason == tokamak::FinishReason::kLength);
  REQUIRE(e3.back().finish_reason == tokamak::FinishReason::kLength);
}

TEST_CASE("Engine backpressure timeout cancels request", "[engine]") {
  tokamak::FakeClock clock;
  tokamak::MockBackend backend(clock, 1ms, 1ms);
  tokamak::EngineConfig config{.tick_interval = 0ms,
                               .backpressure_timeout = 0ms};
  tokamak::Engine engine(backend, clock, config);
  tokamak::TokenChannel channel(1);

  engine.start();
  engine.submit(tokamak::Submission{
      .id = "req-bp",
      .prompt_token_ids = {1, 2, 3},
      .max_tokens = 10,
      .deadline = 10000ms,
      .channel = &channel,
  });

  std::this_thread::sleep_for(50ms);

  std::vector<tokamak::TokenEvent> events;
  while (auto ev = channel.pop()) {
    events.push_back(*ev);
  }

  engine.stop();

  REQUIRE(channel.is_closed());
  REQUIRE(events.size() <= 2);
}

TEST_CASE("Engine start/stop lifecycle is safe", "[engine]") {
  tokamak::FakeClock clock;
  tokamak::MockBackend backend(clock, 1ms, 1ms);
  tokamak::EngineConfig config{.tick_interval = 0ms,
                               .backpressure_timeout = 5000ms};
  tokamak::Engine engine(backend, clock, config);

  engine.start();
  REQUIRE(engine.is_running());
  engine.stop();
  REQUIRE_FALSE(engine.is_running());

  engine.start();
  REQUIRE(engine.is_running());
  engine.stop();
  REQUIRE_FALSE(engine.is_running());

  engine.stop();
  REQUIRE_FALSE(engine.is_running());
}
