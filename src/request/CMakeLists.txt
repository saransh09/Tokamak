add_library(tokamak_request
  state.cpp
  request.cpp
)

target_include_directories(tokamak_request
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(tokamak_request
  PUBLIC
    tokamak_common
  PRIVATE
    tokamak_warnings
)

target_compile_features(tokamak_request PUBLIC cxx_std_23)
