cmake_minimum_required(VERSION 3.21)

project(tokamak
    VERSION 0.1.0
    DESCRIPTION "SLO-aware LLM inference server"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Feeds clangd / IDE tooling (Section 16 requirement in project.md)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Shared warning flags for OUR code only (not third-party deps)
add_library(tokamak_warnings INTERFACE)
target_compile_options(tokamak_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Werror
)

include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.6.0
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)


enable_testing()
add_subdirectory(src/common)
add_subdirectory(src/request)
add_subdirectory(src/backend)
add_subdirectory(src/scheduler)
add_subdirectory(src/telemetry)
add_subdirectory(apps/tokamak)
add_subdirectory(tests/unit)
add_subdirectory(tests/golden)
