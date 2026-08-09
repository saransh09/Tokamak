add_library(tokamak_app workload.cpp simulation.cpp analysis.cpp)
target_include_directories(tokamak_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tokamak_app PUBLIC
    tokamak_warnings tokamak_common tokamak_request
    tokamak_backend tokamak_scheduler tokamak_telemetry
    nlohmann_json::nlohmann_json)

add_executable(tokamak main.cpp)
target_link_libraries(tokamak PRIVATE tokamak_app)
