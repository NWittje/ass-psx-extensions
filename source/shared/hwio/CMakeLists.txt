cmake_minimum_required(VERSION 3.24)
add_library(psx_hwio_contract INTERFACE)
target_include_directories(psx_hwio_contract INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(psx_hwio_contract INTERFACE cxx_std_20)
