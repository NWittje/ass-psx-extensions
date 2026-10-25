cmake_minimum_required(VERSION 3.22)
add_library(psx_hardware_link STATIC src/link.cpp)
target_include_directories(psx_hardware_link PUBLIC include)
target_compile_features(psx_hardware_link PUBLIC cxx_std_20)
