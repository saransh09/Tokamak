#pragma once

#include <cstdint>
#include <string>
#include <string_view>
namespace tokamak {

// Stable error categories for backend operations. These map onto the
// "expected, recoverable" side of the ADR-002 boundary -- a BackendError
// means the backend itself is reporting that it could not service a
// request, not that Tokamak's own bookkeeping is corrupted (that case is
// tokamak::panic(), not this type). See project.md Section 14 for the
// full error-category list and Section 8.4 for "backend errors carry
// stable categories."
enum class BackendErrorCategory : std::uint8_t {
  kInvalidRequest,
  kOutOfMemory,
  kInvalidState,
  kUnavailable,
};

std::string_view to_string(BackendErrorCategory category);

struct BackendError {
  BackendErrorCategory category;

  // Human-readable details for logs. Never used as a metric label and
  // never contains prompt/output content
  std::string message;
};
} // namespace tokamak
