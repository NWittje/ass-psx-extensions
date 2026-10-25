cmake_minimum_required(VERSION 3.24)
add_library(psx_motion INTERFACE)
target_include_directories(psx_motion INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/../aircraft-state/include")
target_compile_features(psx_motion INTERFACE cxx_std_17)
