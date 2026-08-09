add_library(tokamak_telemetry
  trace_event.cpp
  trace_writer.cpp
)

target_include_directories(tokamak_telemetry
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(tokamak_telemetry
  PUBLIC
    tokamak_scheduler
    nlohmann_json::nlohmann_json
  PRIVATE
    tokamak_warnings
)

target_compile_features(tokamak_telemetry PUBLIC cxx_std_23)
