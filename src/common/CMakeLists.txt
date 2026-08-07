add_library(tokamak_common
    clock.cpp
    panic.cpp
)

target_include_directories(tokamak_common
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(tokamak_common
    PRIVATE
        tokamak_warnings
)

target_compile_features(tokamak_common PUBLIC cxx_std_23)
