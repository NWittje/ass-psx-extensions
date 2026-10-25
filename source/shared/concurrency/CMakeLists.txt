cmake_minimum_required(VERSION 3.24)
add_library(psx_concurrency INTERFACE)
target_include_directories(psx_concurrency INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(psx_concurrency INTERFACE cxx_std_17)
