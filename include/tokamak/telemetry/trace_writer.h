#pragma once

#include <ostream>

#include "tokamak/telemetry/trace_event.h"

namespace tokamak {
// Appends TraceEvents to an output stream, one JSON object per line
// (JSONL). Writes to whatever std::ostream& the caller provides -- stdout,
// a file, or (in tests) an std::ostringstream -- so this class has no
// opinion about where a trace ends up
class TraceWriter {
public:
  explicit TraceWriter(std::ostream &out) : out_(out) {}
  void write(const TraceEvent &event);

private:
  std::ostream &out_;
};

} // namespace tokamak
