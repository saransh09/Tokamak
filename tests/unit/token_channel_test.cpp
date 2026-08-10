#include "tokamak/http/token_channel.h"
#include <catch2/catch_test_macros.hpp>
#include <thread>

using namespace tokamak;
using namespace std::chrono_literals;

TEST_CASE("TokenChannel: push and pop maintain FIFO order", "[token_channel]") {
    TokenChannel ch(4);
    REQUIRE(ch.try_push({1, FinishReason::kNone}));
    REQUIRE(ch.try_push({2, FinishReason::kNone}));
    REQUIRE(ch.try_push({3, FinishReason::kStop}));

    auto e1 = ch.pop();
    auto e2 = ch.pop();
    auto e3 = ch.pop();

    REQUIRE(e1.has_value());
    REQUIRE(e1->token_id == 1);
    REQUIRE(e1->finish_reason == FinishReason::kNone);

    REQUIRE(e2.has_value());
    REQUIRE(e2->token_id == 2);

    REQUIRE(e3.has_value());
    REQUIRE(e3->token_id == 3);
    REQUIRE(e3->finish_reason == FinishReason::kStop);
}

TEST_CASE("TokenChannel: try_push returns false when full", "[token_channel]") {
    TokenChannel ch(2);
    REQUIRE(ch.try_push({1, FinishReason::kNone}));
    REQUIRE(ch.try_push({2, FinishReason::kNone}));
    REQUIRE_FALSE(ch.try_push({3, FinishReason::kNone}));
    REQUIRE(ch.size() == 2);
}

TEST_CASE("TokenChannel: pop blocks until push", "[token_channel]") {
    TokenChannel ch(4);
    std::optional<TokenEvent> result;

    std::thread consumer([&] { result = ch.pop(); });

    std::this_thread::sleep_for(10ms);
    REQUIRE(ch.size() == 0);
    ch.try_push({42, FinishReason::kNone});

    consumer.join();
    REQUIRE(result.has_value());
    REQUIRE(result->token_id == 42);
}

TEST_CASE("TokenChannel: close wakes blocked consumer", "[token_channel]") {
    TokenChannel ch(4);
    std::optional<TokenEvent> result;

    std::thread consumer([&] { result = ch.pop(); });

    std::this_thread::sleep_for(10ms);
    ch.close();

    consumer.join();
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("TokenChannel: try_push after close returns false", "[token_channel]") {
    TokenChannel ch(4);
    ch.close();
    REQUIRE_FALSE(ch.try_push({1, FinishReason::kNone}));
}

TEST_CASE("TokenChannel: drain remaining after close", "[token_channel]") {
    TokenChannel ch(4);
    ch.try_push({1, FinishReason::kNone});
    ch.try_push({2, FinishReason::kNone});
    ch.try_push({3, FinishReason::kLength});
    ch.close();

    auto e1 = ch.pop();
    auto e2 = ch.pop();
    auto e3 = ch.pop();
    auto e4 = ch.pop();

    REQUIRE(e1.has_value());
    REQUIRE(e2.has_value());
    REQUIRE(e3.has_value());
    REQUIRE(e3->finish_reason == FinishReason::kLength);
    REQUIRE_FALSE(e4.has_value());
}

TEST_CASE("TokenChannel: finish reason propagates correctly", "[token_channel]") {
    TokenChannel ch(4);
    ch.try_push({10, FinishReason::kStop});
    ch.try_push({20, FinishReason::kLength});
    ch.try_push({30, FinishReason::kNone});

    auto e1 = ch.pop();
    auto e2 = ch.pop();
    auto e3 = ch.pop();

    REQUIRE(e1->finish_reason == FinishReason::kStop);
    REQUIRE(e2->finish_reason == FinishReason::kLength);
    REQUIRE(e3->finish_reason == FinishReason::kNone);
}
