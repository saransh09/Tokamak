#include <catch2/catch_test_macros.hpp>

#include <ctime>
#include <tokamak/common/clock.h>

using namespace std::chrono_literals;

TEST_CASE("FakeClock defaults to zero time point", "[clock]") {
  tokamak::FakeClock clock;
  REQUIRE(clock.now() == tokamak::TimePoint{});
}

TEST_CASE("FakeClock starts at a given time point", "[clock]") {
  tokamak::TimePoint start{100ms};
  tokamak::FakeClock clock(start);

  REQUIRE(clock.now() == start);
}

TEST_CASE("FakeClock advances forward correctly", "[clock]") {
  tokamak::FakeClock clock;

  clock.advance(50ms);
  REQUIRE(clock.now() == tokamak::TimePoint{50ms});

  clock.advance(25ms);
  REQUIRE(clock.now() == tokamak::TimePoint{75ms});
}

TEST_CASE("Clock interface allows polymorphic use", "[clock]") {
    tokamak::FakeClock fake;
    tokamak::Clock& clock_ref = fake;

    fake.advance(10ms);
    REQUIRE(clock_ref.now() == tokamak::TimePoint{10ms});
}
