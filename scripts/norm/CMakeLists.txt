cmake_minimum_required(VERSION 3.30)
project(psx_norm_runtime LANGUAGES C CXX RC)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build NORM with native Windows x64 MSVC")
endif()
# This first-party recipe builds the omitted external runtime without pulling
# application datasets, graphics, simulator SDKs or test tools into the graph.
set(PSX_NORM_RUNTIME_ONLY ON)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../source/shared/sync-network/vendor" norm-vendor)
