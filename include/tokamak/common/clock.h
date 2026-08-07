#pragma once

#include <cassert>
#include <chrono>

namespace tokamak {
using TimePoint = std::chrono::steady_clock::time_point;
using Duration = std::chrono::steady_clock::duration;

// Abstract time source. Production code depends on this interface, never on
// std::chrono::steady_clock directly, so tests can substitute a FakeClock.
class Clock {
public:
  virtual ~Clock() = default;
  virtual TimePoint now() const = 0;
};

// Real wall-of-time clock, backed by std::chrono::steady_clock.
class SystemClock final : public Clock {
public:
  TimePoint now() const override;
};

// Deterministic clock for Tests and simulation. Time only moves forward
// and only when advance() is called explicitly.
class FakeClock final : public Clock {
public:
  explicit FakeClock(TimePoint start = TimePoint{});
  TimePoint now() const override;

  // Moves the clock forward by a non-negative duration
  void advance(Duration d);

private:
  TimePoint current_;
};

} // namespace tokamak
