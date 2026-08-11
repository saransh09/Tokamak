add_library(tokamak_http
  token_channel.cpp
  engine.cpp
  server.cpp
)

target_include_directories(tokamak_http
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(tokamak_http
  PUBLIC
    tokamak_common
    tokamak_request
    tokamak_scheduler
    tokamak_backend
    Boost::asio
    Boost::beast
  PRIVATE
    tokamak_warnings
)

target_compile_features(tokamak_http PUBLIC cxx_std_23)
