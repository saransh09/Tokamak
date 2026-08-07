add_executable(tokamak_unit_tests
    sanity_test.cpp
    clock_test.cpp
    request_state_test.cpp
    request_test.cpp
)

target_link_libraries(tokamak_unit_tests
    PRIVATE
        tokamak_warnings
        tokamak_common
        tokamak_request
        Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(tokamak_unit_tests)
