cmake_minimum_required(VERSION 3.30)
project(psx_cdu_client VERSION 0.1.11 LANGUAGES CXX)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/windows-version/VersionInfo.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/windows-icons/WindowsIcons.cmake")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
find_package(AssGraphics 1.4.4 EXACT CONFIG REQUIRED)
enable_testing()
# The receiver composes CDU foundations directly; it has no DU host runtime.
add_subdirectory(../../shared/cdu cdu)
add_subdirectory(../../shared/diagnostics diagnostics)
add_library(cdu_client_core STATIC src/config.cpp src/runtime.cpp src/shader.cpp)
target_compile_definitions(cdu_client_core PRIVATE PSX_CDU_CLIENT_VERSION="${PROJECT_VERSION}")
target_include_directories(cdu_client_core PUBLIC include PRIVATE ../../shared/cdu/shaders/generated)
target_link_libraries(cdu_client_core PUBLIC psx_cdu_transport psx_cdu_render psx_diagnostics ass::graphics bcrypt ole32)
add_library(cdu_client_ui STATIC src/ui.cpp)
target_include_directories(cdu_client_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/windows-icons")
target_link_libraries(cdu_client_ui PUBLIC cdu_client_core)
add_executable(psx-cdu-client WIN32 src/main.cpp app.manifest)
psx_target_icon(psx-cdu-client DU)
target_link_libraries(psx-cdu-client PRIVATE cdu_client_ui shell32 advapi32)
foreach(t cdu_client_core cdu_client_ui psx-cdu-client)
 target_compile_options(${t} PRIVATE /W4 /Zi /utf-8 /EHsc)
 target_compile_definitions(${t} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endforeach()
target_link_options(psx-cdu-client PRIVATE /DEBUG /INCREMENTAL:NO)
file(GLOB CDU_RUNTIME_SHADERS CONFIGURE_DEPENDS
 "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/cdu/shaders/generated/*.cso"
 "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/cdu/shaders/generated/*.rgba")
list(LENGTH CDU_RUNTIME_SHADERS CDU_RUNTIME_SHADER_COUNT)
if(NOT CDU_RUNTIME_SHADER_COUNT EQUAL 30)
 message(FATAL_ERROR "CDU runtime requires 24 compiled shaders and six lookup textures")
endif()
set(CDU_NORM_RUNTIME "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/norm/vendor/norm/runtime")
function(cdu_stage_runtime target)
 add_custom_command(TARGET ${target} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ass::graphics>"
   "${CDU_NORM_RUNTIME}/Norm.dll" "${CDU_NORM_RUNTIME}/msvcp140.dll"
   "${CDU_NORM_RUNTIME}/vcruntime140.dll" "${CDU_NORM_RUNTIME}/vcruntime140_1.dll"
   "$<TARGET_FILE_DIR:${target}>"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CDU_RUNTIME_SHADERS} "$<TARGET_FILE_DIR:${target}>/shaders"
  VERBATIM)
endfunction()
cdu_stage_runtime(psx-cdu-client)
enable_testing()

add_executable(cdu_client_test tests/client_test.cpp app.manifest)
psx_target_icon(cdu_client_test DU)
target_link_libraries(cdu_client_test PRIVATE cdu_client_ui windowscodecs d3dcompiler dxguid psapi advapi32)
target_compile_options(cdu_client_test PRIVATE /W4 /Zi /utf-8 /EHsc)
target_compile_definitions(cdu_client_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/cdu/fixtures")
target_link_options(cdu_client_test PRIVATE /DEBUG /INCREMENTAL:NO)
cdu_stage_runtime(cdu_client_test)
foreach(operation config shader runtime ui rates blackout shader_options)
 add_test(NAME cdu_client_${operation} COMMAND cdu_client_test ${operation} "${CMAKE_BINARY_DIR}/evidence/$<CONFIG>/client-${operation}")
 set_tests_properties(cdu_client_${operation} PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
endforeach()

find_program(PWSH pwsh REQUIRED)
get_filename_component(CDU_GRAPHICS_SDK_ROOT "${AssGraphics_DIR}/../../../../" ABSOLUTE)
add_test(NAME cdu_client_graphics144_contract COMMAND "${PWSH}" -NoProfile -File
 "${CMAKE_CURRENT_SOURCE_DIR}/tools/graphics144-inputs.ps1" -SdkRoot "${CDU_GRAPHICS_SDK_ROOT}")
set_tests_properties(cdu_client_graphics144_contract PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
add_test(NAME cdu_client_processes COMMAND "${PWSH}" -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/processes.ps1" -ClientTest "$<TARGET_FILE:cdu_client_test>" -Transport "$<TARGET_FILE:psx-cdu-transport>" -Records "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/cdu/fixtures/client-complete.psx" -Output "${CMAKE_BINARY_DIR}/evidence/$<CONFIG>/client-processes")
set_tests_properties(cdu_client_processes PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_test(NAME cdu_client_loopback_processes COMMAND "${PWSH}" -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/processes.ps1" -ClientTest "$<TARGET_FILE:cdu_client_test>" -Transport "$<TARGET_FILE:psx-cdu-transport>" -Records "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/cdu/fixtures/client-complete.psx" -Output "${CMAKE_BINARY_DIR}/evidence/$<CONFIG>/client-loopback-processes" -Loopback)
set_tests_properties(cdu_client_loopback_processes PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

psx_stamp_directory("${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_VERSION}")
