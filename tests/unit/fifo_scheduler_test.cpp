#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

#include "tokamak/backend/error.h"
#include "tokamak/backend/mock_backend.h"
#include "tokamak/common/clock.h"
#include "tokamak/request/request.h"
#include "tokamak/request/state.h"
#include "tokamak/scheduler/fifo_scheduler.h"
#include "tokamak/scheduler/tick_report.h"

using namespace std::chrono_literals;
using tokamak::BackendError;
using tokamak::BackendErrorCategory;
using tokamak::FakeClock;
using tokamak::FifoScheduler;
using tokamak::MockBackend;
using tokamak::Request;
using tokamak::RequestState;
using tokamak::SequenceHandle;
using tokamak::TickReport;


TEST_CASE(
    "tick() promotes a single request through prefill to decode in the same tick",
    "[fifo_scheduler]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);
  FifoScheduler scheduler(backend, clock);

  Request &req = scheduler.submit(std::make_unique<Request>(
      "req-1", clock, 1000ms, /*max_output_tokens=*/5,
      std::vector<std::uint32_t>{1, 2, 3}));

  scheduler.tick();

  REQUIRE(req.lifecycle().state() == RequestState::kDecoding);
  REQUIRE(req.output_tokens_emitted() == 1);
  REQUIRE(scheduler.waiting_count() == 0);
  REQUIRE(scheduler.decoding_count() == 1);
}

TEST_CASE(
    "tick() promotes the entire waiting queue in one tick, not one at a time",
    "[fifo_scheduler]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);
  FifoScheduler scheduler(backend, clock);

  Request &req1 = scheduler.submit(std::make_unique<Request>(
      "req-1", clock, 1000ms, 5, std::vector<std::uint32_t>{1}));
  Request &req2 = scheduler.submit(std::make_unique<Request>(
      "req-2", clock, 1000ms, 5, std::vector<std::uint32_t>{1}));

  scheduler.tick();

  REQUIRE(req1.lifecycle().state() == RequestState::kDecoding);
  REQUIRE(req2.lifecycle().state() == RequestState::kDecoding);
  REQUIRE(scheduler.waiting_count() == 0);
  REQUIRE(scheduler.decoding_count() == 2);
}

TEST_CASE("request completes once max_output_tokens is reached",
          "[fifo_scheduler]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);
  FifoScheduler scheduler(backend, clock);

  Request &req = scheduler.submit(std::make_unique<Request>(
      "req-1", clock, 1000ms, /*max_output_tokens=*/3,
      std::vector<std::uint32_t>{1}));

  scheduler.tick(); // prefill + first decode -> 1 token
  REQUIRE(req.output_tokens_emitted() == 1);
  REQUIRE(req.lifecycle().state() == RequestState::kDecoding);

  scheduler.tick(); // decode -> 2 tokens
  REQUIRE(req.output_tokens_emitted() == 2);
  REQUIRE(req.lifecycle().state() == RequestState::kDecoding);

  scheduler.tick(); // decode -> 3 tokens, exhausted -> Completed
  REQUIRE(req.output_tokens_emitted() == 3);
  REQUIRE(req.lifecycle().state() == RequestState::kCompleted);
  REQUIRE(scheduler.decoding_count() == 0);
}

TEST_CASE("request completes on EOS before max_output_tokens is reached",
          "[fifo_scheduler]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);
  FifoScheduler scheduler(backend, clock);

  Request &req = scheduler.submit(std::make_unique<Request>(
      "req-1", clock, 1000ms, /*max_output_tokens=*/10,
      std::vector<std::uint32_t>{1}));

  scheduler.tick(); // prefill mints handle 0, decode emits token 1
  REQUIRE(req.output_tokens_emitted() == 1);
  REQUIRE(req.lifecycle().state() == RequestState::kDecoding);

  // Only sequence ever prefilled -> deterministically gets handle 0
  // (MockBackend mints ids in prefill-call order starting at 0).
  backend.configure_eos(SequenceHandle(0), 2);

  scheduler.tick(); // decode emits token 2, backend reports finished=true
  REQUIRE(req.output_tokens_emitted() == 2);
  REQUIRE(req.lifecycle().state() == RequestState::kCompleted);
}

TEST_CASE(
    "cancel() while still waiting removes the request before any backend call",
    "[fifo_scheduler]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);
  FifoScheduler scheduler(backend, clock);

  Request &req = scheduler.submit(std::make_unique<Request>(
      "req-1", clock, 1000ms, 5, std::vector<std::uint32_t>{1}));

  scheduler.cancel("req-1");

  REQUIRE(req.lifecycle().state() == RequestState::kCancelled);
  REQUIRE(scheduler.waiting_count() == 0);

  scheduler.tick(); // should be a no-op -- nothing left to promote
  REQUIRE(scheduler.decoding_count() == 0);
}

TEST_CASE("cancel() while decoding releases the backend handle",
          "[fifo_scheduler]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);
  FifoScheduler scheduler(backend, clock);

  Request &req1 = scheduler.submit(std::make_unique<Request>(
      "req-1", clock, 1000ms, 5, std::vector<std::uint32_t>{1}));
  scheduler.tick(); // promotes req1 to Decoding, mints handle 0

  scheduler.cancel("req-1");
  REQUIRE(req1.lifecycle().state() == RequestState::kCancelled);
  REQUIRE(scheduler.decoding_count() == 0);

  // If req1's handle had leaked (not released), a fresh request driven
  // through a full lifecycle should still behave correctly -- no stale
  // handle collision, no panic() from MockBackend's unknown-handle checks.
  Request &req2 = scheduler.submit(std::make_unique<Request>(
      "req-2", clock, 1000ms, 1, std::vector<std::uint32_t>{1}));
  scheduler.tick();
  REQUIRE(req2.lifecycle().state() == RequestState::kCompleted);
}

TEST_CASE("cancel() on an unknown or already-terminal id is a no-op",
          "[fifo_scheduler]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);
  FifoScheduler scheduler(backend, clock);

  Request &req = scheduler.submit(std::make_unique<Request>(
      "req-1", clock, 1000ms, 5, std::vector<std::uint32_t>{1}));

  scheduler.cancel("does-not-exist");
  REQUIRE(req.lifecycle().state() == RequestState::kWaitingPrefill);
  REQUIRE(scheduler.waiting_count() == 1);

  scheduler.cancel("req-1");
  REQUIRE(req.lifecycle().state() == RequestState::kCancelled);

  // Cancelling an already-terminal id again must not panic.
  scheduler.cancel("req-1");
  REQUIRE(req.lifecycle().state() == RequestState::kCancelled);
}

TEST_CASE(
    "tick() expires a request whose deadline has already passed before touching the backend",
    "[fifo_scheduler]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);
  FifoScheduler scheduler(backend, clock);

  Request &req = scheduler.submit(std::make_unique<Request>(
      "req-1", clock, 0ms, 5, std::vector<std::uint32_t>{1}));

  clock.advance(1ms); // now strictly past deadline_at (== 0ms)

  scheduler.tick();

  REQUIRE(req.lifecycle().state() == RequestState::kFailed);
  REQUIRE(scheduler.waiting_count() == 0);
  REQUIRE(scheduler.decoding_count() == 0);
}

TEST_CASE("tick() expires a request whose deadline passes while decoding",
          "[fifo_scheduler]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);
  FifoScheduler scheduler(backend, clock);

  // deadline_at = 6ms: exactly prefill(1ms) + decode(5ms) from one tick,
  // so it survives tick 1 but is expired by the time tick 2 checks it.
  Request &req = scheduler.submit(std::make_unique<Request>(
      "req-1", clock, 6ms, 5, std::vector<std::uint32_t>{1}));

  scheduler.tick(); // clock: 0 -> 1ms (prefill) -> 6ms (decode)
  REQUIRE(req.lifecycle().state() == RequestState::kDecoding);

  clock.advance(1ms); // clock: 7ms, now past deadline_at (6ms)

  scheduler.tick(); // expire_deadlines() catches it before decode_phase()
  REQUIRE(req.lifecycle().state() == RequestState::kFailed);
  REQUIRE(scheduler.decoding_count() == 0);
}

TEST_CASE("BackendError during prefill retires the request as Failed",
          "[fifo_scheduler]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);
  FifoScheduler scheduler(backend, clock);

  Request &failing = scheduler.submit(std::make_unique<Request>(
      "req-fail", clock, 1000ms, 5, std::vector<std::uint32_t>{1}));
  Request &ok = scheduler.submit(std::make_unique<Request>(
      "req-ok", clock, 1000ms, 5, std::vector<std::uint32_t>{1}));

  backend.fail_next_prefill(BackendError{
      .category = BackendErrorCategory::kInvalidRequest,
      .message = "synthetic prefill failure",
  });

  scheduler.tick();

  REQUIRE(failing.lifecycle().state() == RequestState::kFailed);
  REQUIRE(ok.lifecycle().state() == RequestState::kDecoding);
  REQUIRE(scheduler.waiting_count() == 0);
  REQUIRE(scheduler.decoding_count() == 1);
}

TEST_CASE(
    "BackendError during decode retires the request as Failed and releases its handle",
    "[fifo_scheduler]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);
  FifoScheduler scheduler(backend, clock);

  Request &req = scheduler.submit(std::make_unique<Request>(
      "req-1", clock, 1000ms, 5, std::vector<std::uint32_t>{1}));

  scheduler.tick(); // promotes to Decoding, emits token 1 successfully
  REQUIRE(req.lifecycle().state() == RequestState::kDecoding);
  REQUIRE(req.output_tokens_emitted() == 1);

  backend.fail_next_decode(BackendError{
      .category = BackendErrorCategory::kUnavailable,
      .message = "synthetic decode failure",
  });

  scheduler.tick();

  REQUIRE(req.lifecycle().state() == RequestState::kFailed);
  REQUIRE(req.output_tokens_emitted() == 1); // unchanged: failed call must not increment
  REQUIRE(scheduler.decoding_count() == 0);
}

TEST_CASE(
    "FIFO scheduler completes multiple in-flight requests in submission order at expected tick boundaries",
    "[fifo_scheduler]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);
  FifoScheduler scheduler(backend, clock);

  Request &req1 = scheduler.submit(std::make_unique<Request>(
      "req-1", clock, 1000ms, /*max_output_tokens=*/1,
      std::vector<std::uint32_t>{1}));
  Request &req2 = scheduler.submit(std::make_unique<Request>(
      "req-2", clock, 1000ms, /*max_output_tokens=*/2,
      std::vector<std::uint32_t>{1}));
  Request &req3 = scheduler.submit(std::make_unique<Request>(
      "req-3", clock, 1000ms, /*max_output_tokens=*/3,
      std::vector<std::uint32_t>{1}));

  scheduler.tick(); // all three promoted + decode once
  REQUIRE(req1.lifecycle().state() == RequestState::kCompleted); // 1/1
  REQUIRE(req2.lifecycle().state() == RequestState::kDecoding);  // 1/2
  REQUIRE(req3.lifecycle().state() == RequestState::kDecoding);  // 1/3
  REQUIRE(scheduler.decoding_count() == 2);

  scheduler.tick();
  REQUIRE(req2.lifecycle().state() == RequestState::kCompleted); // 2/2
  REQUIRE(req3.lifecycle().state() == RequestState::kDecoding);  // 2/3
  REQUIRE(scheduler.decoding_count() == 1);

  scheduler.tick();
  REQUIRE(req3.lifecycle().state() == RequestState::kCompleted); // 3/3
  REQUIRE(scheduler.decoding_count() == 0);
  REQUIRE(scheduler.waiting_count() == 0);
}

TEST_CASE("tick() reports accurate prefill and decode counts on success",
          "[fifo_scheduler]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);
  FifoScheduler scheduler(backend, clock);

  scheduler.submit(std::make_unique<Request>(
      "req-1", clock, 1000ms, /*max_output_tokens=*/5,
      std::vector<std::uint32_t>{1, 2, 3}));
  scheduler.submit(std::make_unique<Request>(
      "req-2", clock, 1000ms, /*max_output_tokens=*/5,
      std::vector<std::uint32_t>{1}));

  TickReport report = scheduler.tick();

  REQUIRE(report.expired_count == 0);
  REQUIRE(report.prefill_attempted == 2);
  REQUIRE(report.prefill_succeeded == 2);
  REQUIRE(report.prefill_failed == 0);
  REQUIRE(report.prefill_tokens == 4); // 3 + 1
  REQUIRE(report.decode_attempted == 2);
  REQUIRE(report.decode_tokens == 2); // one token attempted per sequence
  REQUIRE(report.decode_completed == 0); // max_output_tokens(5) not yet reached
  REQUIRE(report.decode_failed == 0);
}

TEST_CASE("tick() reports expired_count and skips prefill/decode for an "
          "already-expired waiting request",
          "[fifo_scheduler]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);
  FifoScheduler scheduler(backend, clock);

  scheduler.submit(std::make_unique<Request>(
      "req-1", clock, 0ms, 5, std::vector<std::uint32_t>{1}));
  clock.advance(1ms); // now strictly past deadline_at (== 0ms)

  TickReport report = scheduler.tick();

  REQUIRE(report.expired_count == 1);
  REQUIRE(report.prefill_attempted == 0); // expired before ever reaching prefill
  REQUIRE(report.decode_attempted == 0);
}

TEST_CASE("tick() reports prefill_failed and decode_failed via injected "
          "BackendError",
          "[fifo_scheduler]") {
  FakeClock clock;
  MockBackend backend(clock, 1ms, 5ms);
  FifoScheduler scheduler(backend, clock);

  scheduler.submit(std::make_unique<Request>(
      "req-fail", clock, 1000ms, 5, std::vector<std::uint32_t>{1}));
  scheduler.submit(std::make_unique<Request>(
      "req-ok", clock, 1000ms, 5, std::vector<std::uint32_t>{1}));

  backend.fail_next_prefill(BackendError{
      .category = BackendErrorCategory::kInvalidRequest,
      .message = "synthetic prefill failure",
  });

  TickReport report1 = scheduler.tick();
  REQUIRE(report1.prefill_attempted == 2);
  REQUIRE(report1.prefill_succeeded == 1);
  REQUIRE(report1.prefill_failed == 1);
  REQUIRE(report1.decode_attempted == 1); // only "req-ok" survived to decode

  backend.fail_next_decode(BackendError{
      .category = BackendErrorCategory::kUnavailable,
      .message = "synthetic decode failure",
  });

  TickReport report2 = scheduler.tick();
  REQUIRE(report2.prefill_attempted == 0); // waiting_ is empty by now
  REQUIRE(report2.decode_attempted == 1);
  REQUIRE(report2.decode_failed == 1);
  REQUIRE(report2.decode_completed == 0);
}
