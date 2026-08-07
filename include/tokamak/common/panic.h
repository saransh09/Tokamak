#pragma once

#include <string_view>

namespace tokamak {

// Aborts the process unconditionally, in all build types (unlike assert(),
// which is compiled out under NDEBUG). Use this only for invariant
// violations that indicate corrupted internal state -- never for expected,
// recoverable runtime errors (see project.md Section 14 for the
// distinction between the two).
[[noreturn]] void panic(std::string_view message);

} // namespace tokamak
