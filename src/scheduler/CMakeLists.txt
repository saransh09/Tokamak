add_library(tokamak_scheduler
  fifo_scheduler.cpp
)

target_include_directories(tokamak_scheduler
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(tokamak_scheduler
  PUBLIC
    tokamak_common
  PRIVATE
    tokamak_warnings
)

target_compile_features(tokamak_scheduler PUBLIC cxx_std_23)
