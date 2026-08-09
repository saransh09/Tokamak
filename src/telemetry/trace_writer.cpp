#include "tokamak/telemetry/trace_writer.h"

namespace tokamak {

void TraceWriter::write(const TraceEvent &event) {
  nlohmann::json json_obj = event; // uses the to_json ADL hook
  out_ << json_obj.dump() << '\n';
}

} // namespace tokamak
