#include "tokamak/common/clock.h"

namespace tokamak {

TimePoint SystemClock::now() const { return std::chrono::steady_clock::now(); }

FakeClock::FakeClock(TimePoint start) : current_(start) {}

TimePoint FakeClock::now() const { return current_; }

void FakeClock::advance(Duration d) {
  assert(d >= Duration::zero() &&
         "Fake must advance monotonically (non-negative duration");
  current_ += d;
}
} // namespace tokamak
