add_executable(tokamak_golden_tests golden_trace_test.cpp)
target_link_libraries(tokamak_golden_tests
  PRIVATE
  tokamak_app Catch2::Catch2WithMain
)
target_compile_definitions(tokamak_golden_tests
  PRIVATE TOKAMAK_GOLDEN_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
include(Catch)
catch_discover_tests(tokamak_golden_tests)
