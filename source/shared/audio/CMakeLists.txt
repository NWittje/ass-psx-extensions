cmake_minimum_required(VERSION 3.24)
add_library(psx_audio INTERFACE)
target_include_directories(psx_audio INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(psx_audio INTERFACE cxx_std_17)
