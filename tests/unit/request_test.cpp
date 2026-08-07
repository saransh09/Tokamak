#include <catch2/catch_test_macros.hpp>

#include "tokamak/request/request.h"

using namespace std::chrono_literals;
using tokamak::Request;
using tokamak::RequestState;

TEST_CASE("Request stores id, priority, and output-token budget", "[request]") {
    tokamak::FakeClock clock;
    Request request("req-001", clock, 2000ms, /*max_output_tokens=*/128, /*priority=*/3);

    REQUIRE(request.id() == "req-001");
    REQUIRE(request.priority() == 3);
    REQUIRE(request.max_output_tokens() == 128);
    REQUIRE(request.output_tokens_emitted() == 0);
    REQUIRE(request.lifecycle().state() == RequestState::kReceived);
}

TEST_CASE("Request priority defaults to zero", "[request]") {
    tokamak::FakeClock clock;
    Request request("req-002", clock, 2000ms, 128);

    REQUIRE(request.priority() == 0);
}

TEST_CASE("deadline_at is fixed at construction and does not drift", "[request]") {
    tokamak::FakeClock clock;
    Request request("req-003", clock, 2000ms, 128);

    auto expected_deadline = tokamak::TimePoint{2000ms};
    REQUIRE(request.deadline_at() == expected_deadline);

    // Advancing the clock after construction must not move the deadline --
    // it was computed once, not stored as a relative duration recomputed
    // against a moving now().
    clock.advance(500ms);
    REQUIRE(request.deadline_at() == expected_deadline);
}

TEST_CASE("emit_token increments the counter and records first-token time", "[request]") {
    tokamak::FakeClock clock;
    Request request("req-004", clock, 2000ms, /*max_output_tokens=*/3);

    request.lifecycle().transition_to(RequestState::kAdmitted);
    request.lifecycle().transition_to(RequestState::kWaitingPrefill);
    request.lifecycle().transition_to(RequestState::kPrefilling);
    request.lifecycle().transition_to(RequestState::kWaitingDecode);
    request.lifecycle().transition_to(RequestState::kDecoding);

    clock.advance(7ms);
    request.emit_token();
    REQUIRE(request.output_tokens_emitted() == 1);
    REQUIRE(request.lifecycle().first_token_at() == tokamak::TimePoint{7ms});

    clock.advance(5ms);
    request.emit_token();
    REQUIRE(request.output_tokens_emitted() == 2);
    // first_token_at must not move on the second call.
    REQUIRE(request.lifecycle().first_token_at() == tokamak::TimePoint{7ms});

    clock.advance(5ms);
    request.emit_token();
    REQUIRE(request.output_tokens_emitted() == 3);
    REQUIRE(request.output_tokens_emitted() == request.max_output_tokens());
}
