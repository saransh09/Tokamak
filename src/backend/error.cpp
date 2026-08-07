#include "tokamak/backend/error.h"
#include "tokamak/common/panic.h"

namespace tokamak {

std::string_view to_string(BackendErrorCategory category) {
  switch (category) {
  case BackendErrorCategory::kInvalidRequest:
    return "Invalid Request";
  case BackendErrorCategory::kOutOfMemory:
    return "Out of Memory";
  case BackendErrorCategory::kInvalidState:
    return "Invalid State";
  case BackendErrorCategory::kUnavailable:
    return "Unavailable";
  }
  panic("Unrecognised backend category");
}
} // namespace tokamak
