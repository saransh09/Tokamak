#include <catch2/catch_test_macros.hpp>

#include "tokamak/common/clock.h"
#include "tokamak/request/state.h"

using namespace std::chrono_literals;
using tokamak::RequestState;

TEST_CASE("to_string produces readable names", "[request_state]") {
    REQUIRE(tokamak::to_string(RequestState::kReceived) == "Received");
    REQUIRE(tokamak::to_string(RequestState::kCompleted) == "Completed");
}

TEST_CASE("terminal states are correctly classified", "[request_state]") {
    REQUIRE(tokamak::is_terminal(RequestState::kCompleted));
    REQUIRE(tokamak::is_terminal(RequestState::kRejected));
    REQUIRE(tokamak::is_terminal(RequestState::kCancelled));
    REQUIRE(tokamak::is_terminal(RequestState::kFailed));

    REQUIRE_FALSE(tokamak::is_terminal(RequestState::kReceived));
    REQUIRE_FALSE(tokamak::is_terminal(RequestState::kWaitingPrefill));
    REQUIRE_FALSE(tokamak::is_terminal(RequestState::kDecoding));
}

TEST_CASE("valid transitions follow the documented lifecycle", "[request_state]") {
    using tokamak::is_valid_transition;

    REQUIRE(is_valid_transition(RequestState::kReceived, RequestState::kAdmitted));
    REQUIRE(is_valid_transition(RequestState::kReceived, RequestState::kRejected));
    REQUIRE(is_valid_transition(RequestState::kAdmitted, RequestState::kWaitingPrefill));
    REQUIRE(is_valid_transition(RequestState::kWaitingPrefill, RequestState::kPrefilling));
    REQUIRE(is_valid_transition(RequestState::kPrefilling, RequestState::kWaitingDecode));
    REQUIRE(is_valid_transition(RequestState::kWaitingDecode, RequestState::kDecoding));
    REQUIRE(is_valid_transition(RequestState::kDecoding, RequestState::kCompleted));
}

TEST_CASE("cancellation is reachable from every non-terminal state", "[request_state]") {
    using tokamak::is_valid_transition;

    REQUIRE(is_valid_transition(RequestState::kReceived, RequestState::kCancelled));
    REQUIRE(is_valid_transition(RequestState::kAdmitted, RequestState::kCancelled));
    REQUIRE(is_valid_transition(RequestState::kWaitingPrefill, RequestState::kCancelled));
    REQUIRE(is_valid_transition(RequestState::kPrefilling, RequestState::kCancelled));
    REQUIRE(is_valid_transition(RequestState::kWaitingDecode, RequestState::kCancelled));
    REQUIRE(is_valid_transition(RequestState::kDecoding, RequestState::kCancelled));
}

TEST_CASE("terminal states have no outgoing transitions", "[request_state]") {
    using tokamak::is_valid_transition;

    const RequestState all_states[] = {
        RequestState::kReceived,      RequestState::kAdmitted,
        RequestState::kWaitingPrefill, RequestState::kPrefilling,
        RequestState::kWaitingDecode,  RequestState::kDecoding,
        RequestState::kCompleted,      RequestState::kRejected,
        RequestState::kCancelled,      RequestState::kFailed,
    };
    const RequestState terminal_states[] = {
        RequestState::kCompleted, RequestState::kRejected,
        RequestState::kCancelled, RequestState::kFailed,
    };

    for (auto terminal : terminal_states) {
        for (auto target : all_states) {
            REQUIRE_FALSE(is_valid_transition(terminal, target));
        }
    }
}

TEST_CASE("RequestLifecycle starts in Received", "[request_state]") {
    tokamak::FakeClock clock;
    tokamak::RequestLifecycle lifecycle(clock);

    REQUIRE(lifecycle.state() == RequestState::kReceived);
    REQUIRE_FALSE(lifecycle.is_terminal());
    REQUIRE_FALSE(lifecycle.admitted_at().has_value());
}

TEST_CASE("RequestLifecycle records admitted_at on transition", "[request_state]") {
    tokamak::FakeClock clock;
    tokamak::RequestLifecycle lifecycle(clock);

    clock.advance(10ms);
    lifecycle.transition_to(RequestState::kAdmitted);

    REQUIRE(lifecycle.state() == RequestState::kAdmitted);
    REQUIRE(lifecycle.admitted_at() == tokamak::TimePoint{10ms});
    REQUIRE(lifecycle.entered_at() == tokamak::TimePoint{10ms});
}

TEST_CASE("RequestLifecycle happy path records TTFT/E2E milestones", "[request_state]") {
    tokamak::FakeClock clock;
    tokamak::RequestLifecycle lifecycle(clock);

    clock.advance(5ms);
    lifecycle.transition_to(RequestState::kAdmitted);

    clock.advance(2ms);
    lifecycle.transition_to(RequestState::kWaitingPrefill);

    clock.advance(20ms);
    lifecycle.transition_to(RequestState::kPrefilling);

    clock.advance(15ms);
    lifecycle.transition_to(RequestState::kWaitingDecode);

    clock.advance(1ms);
    lifecycle.transition_to(RequestState::kDecoding);

    clock.advance(8ms);
    lifecycle.record_first_token();

    clock.advance(50ms);
    lifecycle.transition_to(RequestState::kCompleted);

    REQUIRE(lifecycle.state() == RequestState::kCompleted);
    REQUIRE(lifecycle.is_terminal());

    REQUIRE(lifecycle.admitted_at() == tokamak::TimePoint{5ms});
    REQUIRE(lifecycle.first_token_at() == tokamak::TimePoint{51ms});
    REQUIRE(lifecycle.completed_at() == tokamak::TimePoint{101ms});

    // TTFT and E2E per project.md Section 5.
    auto ttft = *lifecycle.first_token_at() - *lifecycle.admitted_at();
    auto e2e  = *lifecycle.completed_at() - *lifecycle.admitted_at();
    REQUIRE(ttft == 46ms);
    REQUIRE(e2e == 96ms);
}

TEST_CASE("cancel() is idempotent", "[request_state]") {
    tokamak::FakeClock clock;
    tokamak::RequestLifecycle lifecycle(clock);

    lifecycle.transition_to(RequestState::kAdmitted);
    lifecycle.transition_to(RequestState::kWaitingPrefill);

    lifecycle.cancel();
    REQUIRE(lifecycle.state() == RequestState::kCancelled);

    auto cancelled_at = lifecycle.entered_at();

    clock.advance(100ms);
    lifecycle.cancel();  // Calling again must be a no-op.

    REQUIRE(lifecycle.state() == RequestState::kCancelled);
    REQUIRE(lifecycle.entered_at() == cancelled_at);  // Unchanged.
}

TEST_CASE("cancel() is a no-op once a request reaches any terminal state", "[request_state]") {
    tokamak::FakeClock clock;
    tokamak::RequestLifecycle lifecycle(clock);

    lifecycle.transition_to(RequestState::kRejected);
    auto rejected_at = lifecycle.entered_at();

    clock.advance(100ms);
    lifecycle.cancel();

    REQUIRE(lifecycle.state() == RequestState::kRejected);  // Still Rejected, not Cancelled.
    REQUIRE(lifecycle.entered_at() == rejected_at);
}

TEST_CASE("record_first_token only records the first call", "[request_state]") {
    tokamak::FakeClock clock;
    tokamak::RequestLifecycle lifecycle(clock);

    lifecycle.transition_to(RequestState::kAdmitted);
    lifecycle.transition_to(RequestState::kWaitingPrefill);
    lifecycle.transition_to(RequestState::kPrefilling);
    lifecycle.transition_to(RequestState::kWaitingDecode);
    lifecycle.transition_to(RequestState::kDecoding);

    clock.advance(5ms);
    lifecycle.record_first_token();
    auto first = lifecycle.first_token_at();

    clock.advance(5ms);
    lifecycle.record_first_token();  // Second call should not move the timestamp.

    REQUIRE(lifecycle.first_token_at() == first);
}
