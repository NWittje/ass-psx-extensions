cmake_minimum_required(VERSION 3.24)

project(psx_hwio VERSION 1.28.4 LANGUAGES CXX RC)

get_filename_component(PSX_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

get_filename_component(PSX_SOURCE_DIRECTORY "${PSX_SOURCE_ROOT}" NAME)

if(PSX_SOURCE_DIRECTORY STREQUAL "source")

  get_filename_component(PSX_PROJECT_ROOT "${PSX_SOURCE_ROOT}/.." ABSOLUTE)

else()

  set(PSX_PROJECT_ROOT "${PSX_SOURCE_ROOT}")

endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/windows-version/VersionInfo.cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/windows-icons/WindowsIcons.cmake")

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)

  message(FATAL_ERROR "Windows x64 MSVC is required")

endif()

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(/W4 /WX /EHsc /utf-8 "$<$<CONFIG:Release>:/Zi>")

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)

add_link_options(/DEBUG:FULL /INCREMENTAL:NO)

if(NOT TARGET psx_hwio_contract)

  add_subdirectory(../../shared/hwio hwio-contract)

endif()

if(NOT TARGET psx_diagnostics)

  add_subdirectory(../../shared/diagnostics diagnostics)

endif()

set(ASS_GRAPHICS_SDK_ROOT "P:/development/ass_graphics_lib/sdk/internal/1.4.5" CACHE PATH "Qualified ASS Graphics 1.4.5 or newer matched Debug/Release SDK inputs")

find_package(AssGraphics 1.4.5 CONFIG REQUIRED PATHS "${ASS_GRAPHICS_SDK_ROOT}/Release/lib/cmake/AssGraphics" NO_DEFAULT_PATH)

set_property(TARGET ass::graphics APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)

set_target_properties(ass::graphics PROPERTIES IMPORTED_LOCATION_DEBUG "${ASS_GRAPHICS_SDK_ROOT}/Debug/bin/ass_graphics_lib.dll" IMPORTED_IMPLIB_DEBUG "${ASS_GRAPHICS_SDK_ROOT}/Debug/lib/ass_graphics_lib.lib")

if(NOT TARGET psx_network_io)

  add_subdirectory(../../shared/network-io network-io)

endif()

if(NOT TARGET psx_trim_connection)

  add_subdirectory(../../shared/hardware-connection hardware-connection)

endif()

if(NOT TARGET psx_instructor)
  add_subdirectory(../../shared/psx-instructor psx-instructor)
endif()

add_library(hwio_core STATIC src/pokeys.cpp src/hardware_config.cpp src/ethernet.cpp src/core.cpp src/catalogue.cpp src/profile.cpp src/hid.cpp src/acquisition.cpp src/usb.cpp src/transport.cpp src/runtime.cpp src/flight_display.cpp src/flight_scene.cpp src/axis_setup.cpp src/axis_panel.cpp src/storage.cpp src/button_setup.cpp src/button_catalogue.cpp src/button_panel.cpp src/tray_status.cpp)

target_include_directories(hwio_core PUBLIC include "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/windows-icons")

target_compile_definitions(hwio_core PUBLIC HWIO_VERSION="${PROJECT_VERSION}" HWIO_VERSION_WIDE=L"${PROJECT_VERSION}")

set(HWIO_COMPILE_JOBS "6" CACHE STRING "Bounded parallel native compilation jobs")

if(NOT HWIO_COMPILE_JOBS MATCHES "^[1-6]$")

  message(FATAL_ERROR "HWIO_COMPILE_JOBS must be 1 through 6")

endif()

target_compile_options(hwio_core PRIVATE /MP${HWIO_COMPILE_JOBS})

target_sources(hwio_core PRIVATE src/card_assignments.cpp src/runtime_card.cpp src/card_transfer_panel.cpp src/hardware_router.cpp src/hardware_panel.cpp src/digital_outputs.cpp src/output_panel.cpp src/cockpit.cpp src/cockpit_panel.cpp src/fds.cpp src/fds_modules.cpp src/module_panel.cpp)

target_sources(hwio_core PRIVATE src/phidgets.cpp src/fdstq.cpp src/fdstq_configuration.cpp src/fdstq_motion.cpp src/fds_trace.cpp src/fds_replay.cpp src/fds_cdu.cpp)

target_sources(hwio_core PRIVATE src/phidget_io.cpp src/phidget_session.cpp src/phidget_motor.cpp src/software_motor.cpp src/phidget_drive.cpp src/motor_feedback.cpp src/motor_autotune.cpp)

target_sources(hwio_core PRIVATE src/phidget_servo.cpp src/servo.cpp src/servo_panel.cpp)

target_sources(hwio_core PRIVATE src/vendor_library.cpp)
target_link_libraries(hwio_core PUBLIC msi)
add_executable(hwio_vendor_library_tests tests/vendor-library.cpp)
target_link_libraries(hwio_vendor_library_tests PRIVATE hwio_core)
add_test(NAME hwio_vendor_library COMMAND hwio_vendor_library_tests "${CMAKE_BINARY_DIR}/vendor-library-$<CONFIG>")
target_sources(hwio_core PRIVATE src/numeric_source.cpp src/actuator_repository.cpp src/actuator_ports.cpp src/actuators.cpp src/stepper.cpp src/phidget_stepper.cpp src/actuator_panel.cpp src/actuator_control.cpp)

target_sources(hwio_core PRIVATE src/motor_bench.cpp src/motor_waveform.cpp src/motor_panel.cpp src/motor_mapping.cpp src/motor_pipe.cpp src/motor_following_log.cpp src/motor_homing_log.cpp src/motor_mapping_panel.cpp)

target_sources(hwio_core PRIVATE src/failures.cpp src/failure_pipe.cpp)

target_sources(hwio_core PRIVATE src/situation_controls.cpp)

target_sources(hwio_core PRIVATE src/thrust_control.cpp)

add_executable(hwio_thrust_control_tests tests/thrust-control.cpp)

target_link_libraries(hwio_thrust_control_tests PRIVATE hwio_core)

add_test(NAME hwio_thrust_control COMMAND hwio_thrust_control_tests)

set_tests_properties(hwio_thrust_control PROPERTIES TIMEOUT 15 RUN_SERIAL TRUE)

add_custom_command(TARGET hwio_thrust_control_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:hwio_thrust_control_tests>)

add_executable(hwio_situation_control_tests tests/situation-controls.cpp)

target_link_libraries(hwio_situation_control_tests PRIVATE hwio_core)

add_test(NAME hwio_situation_controls COMMAND hwio_situation_control_tests)

set_tests_properties(hwio_situation_controls PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_custom_command(TARGET hwio_situation_control_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:hwio_situation_control_tests>)

add_dependencies(hwio_situation_control_tests hwio_runtime_data)

target_sources(hwio_core PRIVATE src/input_overrides.cpp src/input_override_panel.cpp)

target_sources(hwio_core PRIVATE src/trim_displays.cpp src/cpflight_protocol.cpp src/cpflight.cpp src/cpflight_serial.cpp src/cpflight_modules.cpp)

target_sources(hwio_core PRIVATE src/output_identification.cpp src/output_identification_port.cpp src/output_atlas.cpp src/output_identification_panel.cpp)

target_link_libraries(hwio_core PUBLIC psx_trim_connection)

add_executable(hwio_trim_display_tests tests/trim-displays.cpp)

target_link_libraries(hwio_trim_display_tests PRIVATE hwio_core)

add_custom_command(TARGET hwio_trim_display_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:hwio_trim_display_tests>)

set(PHIDGET22_SDK_ROOT "D:/Phidgets/Phidget22" CACHE PATH "Installed native Phidget22 headers")

target_include_directories(hwio_core PRIVATE "${PHIDGET22_SDK_ROOT}" "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/windows-runtime/include")

target_link_libraries(hwio_core PUBLIC psx_network_io psx_hwio_contract psx_diagnostics ass::graphics xmllite shlwapi bcrypt advapi32 ws2_32 hid setupapi cfgmgr32 user32 shell32 ole32 dinput8 dxguid version comctl32)

add_executable(psx-hwio WIN32 src/main.cpp src/app.manifest)

target_link_libraries(psx-hwio PRIVATE hwio_core)

option(HWIO_PUBLISH_BINARIES "Publish standard builds to canonical bin packages" ON)

if(HWIO_PUBLISH_BINARIES)

  find_program(HWIO_POWERSHELL NAMES powershell.exe REQUIRED)

  add_custom_target(hwio_publish ALL

    COMMAND "${HWIO_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${PSX_PROJECT_ROOT}/scripts/publish-binaries.ps1"

      -Module HWIO -Configuration "$<CONFIG>" -BuildDirectory "${CMAKE_CURRENT_BINARY_DIR}"

    VERBATIM USES_TERMINAL)

  add_dependencies(hwio_publish psx-hwio)

endif()

add_executable(hwio-fds-replay src/fds_replay_main.cpp)

target_link_libraries(hwio-fds-replay PRIVATE hwio_core)

enable_testing()

add_executable(hwio-flightillusion-probe tools/flightillusion-probe.cpp)

target_link_libraries(hwio-flightillusion-probe PRIVATE hwio_core)

add_custom_command(TARGET hwio-flightillusion-probe POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:hwio-flightillusion-probe>)

target_sources(hwio_core PRIVATE src/flightillusion_model.cpp src/flightillusion_win32.cpp src/flightillusion.cpp src/flightillusion_panel.cpp src/flightillusion_bridge.cpp src/flightillusion_exporter.cpp)

target_link_libraries(hwio_core PUBLIC psx_instructor)

foreach(kind IN ITEMS flightillusion flightillusion-panel flightillusion-bridge flightillusion-exporter)

  string(REPLACE "-" "_" target "hwio_${kind}_tests")

  string(REPLACE "-" "_" test "hwio_${kind}")

  add_executable(${target} tests/${kind}.cpp)

  target_link_libraries(${target} PRIVATE hwio_core)

  add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:${target}>)

  add_test(NAME ${test} COMMAND ${target} "${CMAKE_BINARY_DIR}/${kind}-$<CONFIG>")

  set_tests_properties(${test} PROPERTIES TIMEOUT 40 RUN_SERIAL TRUE)

endforeach()

add_executable(hwio_cpflight_tests tests/cpflight.cpp)

target_link_libraries(hwio_cpflight_tests PRIVATE hwio_core)

add_executable(hwio-cpflight-probe tools/cpflight-probe.cpp)

target_link_libraries(hwio-cpflight-probe PRIVATE hwio_core)

add_custom_command(TARGET hwio-cpflight-probe POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:hwio-cpflight-probe>)

add_custom_command(TARGET hwio_cpflight_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:hwio_cpflight_tests> COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/config/catalogue.xml $<TARGET_FILE_DIR:hwio_cpflight_tests>/catalogue.xml)

add_test(NAME hwio_cpflight_codec COMMAND hwio_cpflight_tests codec)

add_test(NAME hwio_cpflight_provider COMMAND hwio_cpflight_tests provider)

add_test(NAME hwio_cpflight_modules COMMAND hwio_cpflight_tests modules "${CMAKE_BINARY_DIR}/cpflight-fixture/$<CONFIG>")

set_tests_properties(hwio_cpflight_codec hwio_cpflight_provider hwio_cpflight_modules PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_library(hwio_servo_mock SHARED tests/servo-mock.cpp)

add_library(hwio_stepper_mock SHARED tests/stepper-mock.cpp)

add_executable(hwio_actuator_tests tests/actuators.cpp)

target_link_libraries(hwio_actuator_tests PRIVATE hwio_core)

add_test(NAME hwio_actuator COMMAND hwio_actuator_tests "${CMAKE_BINARY_DIR}/actuator-fixture/$<CONFIG>")

add_executable(hwio_actuator_panel_tests tests/actuator-panel.cpp)

target_link_libraries(hwio_actuator_panel_tests PRIVATE hwio_core)

add_test(NAME hwio_actuator_panel COMMAND hwio_actuator_panel_tests "${CMAKE_BINARY_DIR}/actuator-panel-fixture/$<CONFIG>")

set_tests_properties(hwio_actuator hwio_actuator_panel PROPERTIES TIMEOUT 40 RUN_SERIAL TRUE)

add_executable(hwio_stepper_tests tests/stepper.cpp)

target_link_libraries(hwio_stepper_tests PRIVATE hwio_core)

add_dependencies(hwio_stepper_tests hwio_stepper_mock)

add_test(NAME hwio_stepper COMMAND hwio_stepper_tests $<TARGET_FILE:hwio_stepper_mock> "${CMAKE_BINARY_DIR}/stepper-fixture/$<CONFIG>")

set_tests_properties(hwio_stepper PROPERTIES TIMEOUT 40 RUN_SERIAL TRUE)

add_executable(hwio_actuator_geometry_tests tests/actuator-geometry.cpp)

target_include_directories(hwio_actuator_geometry_tests PRIVATE include)

add_test(NAME hwio_actuator_geometry COMMAND hwio_actuator_geometry_tests)

add_executable(hwio-phidget-inventory EXCLUDE_FROM_ALL tools/phidget-inventory.cpp)
target_link_libraries(hwio-phidget-inventory PRIVATE hwio_core)
target_include_directories(hwio-phidget-inventory PRIVATE "${PHIDGET22_SDK_ROOT}")
add_executable(hwio_servo_tests tests/servo.cpp)

target_link_libraries(hwio_servo_tests PRIVATE hwio_core)

add_dependencies(hwio_servo_tests hwio_servo_mock)

add_test(NAME hwio_servo COMMAND hwio_servo_tests $<TARGET_FILE:hwio_servo_mock> "${CMAKE_BINARY_DIR}/servo-fixture/$<CONFIG>")

add_executable(hwio_servo_panel_tests tests/servo-panel.cpp)

target_link_libraries(hwio_servo_panel_tests PRIVATE hwio_core)

add_test(NAME hwio_servo_panel COMMAND hwio_servo_panel_tests "${CMAKE_BINARY_DIR}/servo-panel-fixture/$<CONFIG>")

set_tests_properties(hwio_servo hwio_servo_panel PROPERTIES TIMEOUT 40 RUN_SERIAL TRUE)

foreach(target IN ITEMS hwio_servo_tests hwio_servo_panel_tests hwio_stepper_tests hwio_actuator_tests hwio_actuator_panel_tests)

 add_custom_command(TARGET ${target} POST_BUILD

 COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:${target}>

 COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/config/catalogue.xml $<TARGET_FILE_DIR:${target}>/catalogue.xml)

endforeach()

add_library(hwio_phidget_io_mock SHARED tests/phidget-io-mock.cpp)

add_executable(hwio_phidget_io_tests tests/phidget-io.cpp)

target_link_libraries(hwio_phidget_io_tests PRIVATE hwio_core)

add_dependencies(hwio_phidget_io_tests hwio_phidget_io_mock)

add_test(NAME hwio_phidget_io COMMAND hwio_phidget_io_tests $<TARGET_FILE:hwio_phidget_io_mock>)

set_tests_properties(hwio_phidget_io PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_executable(hwio_runtime_phidget_tests tests/runtime-phidgets.cpp)

target_link_libraries(hwio_runtime_phidget_tests PRIVATE hwio_core)

set_target_properties(hwio_runtime_phidget_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime-phidget/$<CONFIG>")

add_dependencies(hwio_runtime_phidget_tests hwio_phidget_io_mock)

add_custom_command(TARGET hwio_runtime_phidget_tests POST_BUILD

  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:hwio_phidget_io_mock> $<TARGET_FILE_DIR:hwio_runtime_phidget_tests>/phidget22.dll

  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_PDB_FILE:hwio_phidget_io_mock> $<TARGET_FILE_DIR:hwio_runtime_phidget_tests>/phidget22.pdb

  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:hwio_runtime_phidget_tests>/ass_graphics_lib.dll

  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/config/catalogue.xml $<TARGET_FILE_DIR:hwio_runtime_phidget_tests>/catalogue.xml)

add_test(NAME hwio_runtime_phidgets COMMAND hwio_runtime_phidget_tests)

set_tests_properties(hwio_runtime_phidgets PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)



add_executable(hwio_output_identification_panel_tests tests/output-identification-panel.cpp)

target_link_libraries(hwio_output_identification_panel_tests PRIVATE hwio_core)

add_custom_command(TARGET hwio_output_identification_panel_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:hwio_output_identification_panel_tests>)

add_test(NAME hwio_output_identification_panel COMMAND hwio_output_identification_panel_tests "${CMAKE_CURRENT_BINARY_DIR}/output-identification-panel-$<CONFIG>")

set_tests_properties(hwio_output_identification_panel PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

foreach(kind IN ITEMS identification identification-port atlas)

  string(REPLACE "-" "_" test_name "output-${kind}")

  add_executable(hwio_${test_name}_tests tests/output-${kind}.cpp)

  target_link_libraries(hwio_${test_name}_tests PRIVATE hwio_core)

  if(kind STREQUAL "atlas")

    add_test(NAME hwio_${test_name} COMMAND hwio_${test_name}_tests "${CMAKE_CURRENT_BINARY_DIR}/output-atlas-$<CONFIG>.xml")

  else()

    add_test(NAME hwio_${test_name} COMMAND hwio_${test_name}_tests)

  endif()

  add_custom_command(TARGET hwio_${test_name}_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:hwio_${test_name}_tests>)

  set_tests_properties(hwio_${test_name} PROPERTIES TIMEOUT 20)

endforeach()

add_test(NAME hwio_trim_displays COMMAND hwio_trim_display_tests "${CMAKE_CURRENT_BINARY_DIR}/trim-displays-$<CONFIG>")

set_tests_properties(hwio_trim_displays PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_executable(hwio_nav_course_tests tests/nav-course.cpp)

target_link_libraries(hwio_nav_course_tests PRIVATE hwio_core)

add_test(NAME hwio_nav_course COMMAND hwio_nav_course_tests "${CMAKE_CURRENT_BINARY_DIR}/nav-course-$<CONFIG>")

set_tests_properties(hwio_nav_course PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_executable(hwio_motor_diagnostics_tests tests/motor-diagnostics.cpp)

target_link_libraries(hwio_motor_diagnostics_tests PRIVATE hwio_core)

add_test(NAME hwio_motor_diagnostics COMMAND hwio_motor_diagnostics_tests "${CMAKE_CURRENT_BINARY_DIR}/motor-diagnostics-$<CONFIG>")

add_executable(hwio_motor_following_tests tests/motor-following.cpp)

target_include_directories(hwio_motor_following_tests PRIVATE include)

add_test(NAME hwio_motor_following COMMAND hwio_motor_following_tests)

foreach(kind IN ITEMS alignment correction)

  add_executable(hwio_motor_${kind}_tests tests/motor-${kind}.cpp)

  target_include_directories(hwio_motor_${kind}_tests PRIVATE include)

  add_test(NAME hwio_motor_${kind} COMMAND hwio_motor_${kind}_tests)

endforeach()

add_executable(hwio_motor_disconnect_tests tests/motor-disconnect.cpp)

target_link_libraries(hwio_motor_disconnect_tests PRIVATE hwio_core)

add_test(NAME hwio_motor_disconnect COMMAND hwio_motor_disconnect_tests)

add_executable(hwio_input_override_tests tests/input-overrides.cpp src/app.manifest)

target_link_libraries(hwio_input_override_tests PRIVATE hwio_core)

add_dependencies(hwio_input_override_tests hwio_runtime_data)

add_test(NAME hwio_input_overrides COMMAND hwio_input_override_tests "${CMAKE_CURRENT_BINARY_DIR}/input-overrides-$<CONFIG>")

set_tests_properties(hwio_input_overrides PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_executable(hwio_motor_motion_tests tests/motor-motion.cpp)

target_link_libraries(hwio_motor_motion_tests PRIVATE hwio_core)

add_test(NAME hwio_motor_motion COMMAND hwio_motor_motion_tests)

add_library(hwio_phidget_drive_mock SHARED tests/phidget-drive-mock.cpp)

add_executable(hwio_phidget_drive_tests tests/phidget-drive.cpp)

target_link_libraries(hwio_phidget_drive_tests PRIVATE hwio_core)

add_dependencies(hwio_phidget_drive_tests hwio_phidget_drive_mock)

add_test(NAME hwio_phidget_drive COMMAND hwio_phidget_drive_tests $<TARGET_FILE:hwio_phidget_drive_mock>)

set_tests_properties(hwio_phidget_drive PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)

add_custom_command(TARGET hwio_phidget_drive_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:hwio_phidget_drive_tests>)

add_executable(hwio_motor_software_tests tests/motor-software.cpp)

add_executable(hwio_motor_backlash_tests tests/motor-backlash.cpp)

target_link_libraries(hwio_motor_backlash_tests PRIVATE hwio_core)

add_test(NAME hwio_motor_backlash COMMAND hwio_motor_backlash_tests)

set_tests_properties(hwio_motor_backlash PROPERTIES TIMEOUT 10)

add_executable(hwio_motor_autotune_tests tests/motor-autotune.cpp)

add_executable(hwio_motor_tuning_model_tests tests/motor-autotune.cpp src/motor_autotune.cpp src/motor_waveform.cpp)

target_include_directories(hwio_motor_tuning_model_tests PRIVATE include)

foreach(tuning_target IN ITEMS hwio_motor_autotune_tests hwio_motor_tuning_model_tests)

  target_compile_definitions(${tuning_target} PRIVATE HWIO_AUTOTUNE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/motor-tuning-short-travel.csv")

  target_compile_definitions(${tuning_target} PRIVATE HWIO_AUTOTUNE_POT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/motor-tuning-pot-short-travel.csv")

endforeach()

target_compile_definitions(hwio_motor_tuning_model_tests PRIVATE AUTOTUNE_MODEL_ONLY)

add_test(NAME hwio_motor_tuning_model COMMAND hwio_motor_tuning_model_tests)

set_tests_properties(hwio_motor_tuning_model PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

target_link_libraries(hwio_motor_autotune_tests PRIVATE hwio_core)

add_test(NAME hwio_motor_autotune COMMAND hwio_motor_autotune_tests)

set_tests_properties(hwio_motor_autotune PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

add_custom_command(TARGET hwio_motor_autotune_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:hwio_motor_autotune_tests>)

target_link_libraries(hwio_motor_software_tests PRIVATE hwio_core)

add_test(NAME hwio_motor_software COMMAND hwio_motor_software_tests "${CMAKE_CURRENT_BINARY_DIR}/motor-software-$<CONFIG>")

set_tests_properties(hwio_motor_software PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_custom_command(TARGET hwio_motor_software_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:hwio_motor_software_tests>)

add_executable(hwio_motor_bench_tests tests/motor-bench.cpp)

target_link_libraries(hwio_motor_bench_tests PRIVATE hwio_core)

option(HWIO_BUILD_MOTOR_LIVE "Build manually invoked, externally guarded motor qualification tool (never CTest)" OFF)

if(HWIO_BUILD_MOTOR_LIVE)

  add_executable(hwio_motor_autotune_live tests/motor-autotune-live.cpp)

  add_executable(hwio_motor_autotune_replay tests/motor-autotune-replay.cpp src/motor_waveform.cpp)

  target_include_directories(hwio_motor_autotune_replay PRIVATE include src)

  target_compile_features(hwio_motor_autotune_replay PRIVATE cxx_std_20)

  target_compile_options(hwio_motor_autotune_replay PRIVATE /W4 /WX /EHsc /utf-8)

  target_link_libraries(hwio_motor_autotune_live PRIVATE hwio_core)

  add_custom_command(TARGET hwio_motor_autotune_live POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different

    "$<TARGET_FILE:ass::graphics>" "$<TARGET_FILE_DIR:hwio_motor_autotune_live>/ass_graphics_lib.dll")

  add_executable(hwio_motor_live tests/motor-live.cpp)

  target_link_libraries(hwio_motor_live PRIVATE hwio_core)

  add_custom_command(TARGET hwio_motor_live POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different

    "$<TARGET_FILE:ass::graphics>" "$<TARGET_FILE_DIR:hwio_motor_live>/ass_graphics_lib.dll")

endif()

add_test(NAME hwio_motor_bench COMMAND hwio_motor_bench_tests "${CMAKE_CURRENT_BINARY_DIR}/motor-bench-$<CONFIG>")

add_executable(hwio_motor_homing_tests tests/motor-homing.cpp)

target_sources(hwio_motor_homing_tests PRIVATE src/motor_homing_log.cpp)

target_include_directories(hwio_motor_homing_tests PRIVATE include)

add_test(NAME hwio_motor_homing COMMAND hwio_motor_homing_tests "${CMAKE_CURRENT_BINARY_DIR}/motor-homing-$<CONFIG>")

set_tests_properties(hwio_motor_homing PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

add_executable(hwio_motor_mapping_tests tests/motor-mapping.cpp)

target_link_libraries(hwio_motor_mapping_tests PRIVATE hwio_core)

add_test(NAME hwio_motor_mapping COMMAND hwio_motor_mapping_tests "${CMAKE_CURRENT_BINARY_DIR}/motor-mapping-$<CONFIG>")

set_tests_properties(hwio_motor_mapping PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

add_executable(hwio_motor_continuity_tests tests/motor-continuity.cpp)

target_link_libraries(hwio_motor_continuity_tests PRIVATE hwio_core)

add_test(NAME hwio_motor_continuity COMMAND hwio_motor_continuity_tests "${CMAKE_CURRENT_BINARY_DIR}/motor-continuity-$<CONFIG>")

set_tests_properties(hwio_motor_continuity PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

add_executable(hwio_motor_panel_tests tests/motor-panel.cpp)

target_link_libraries(hwio_motor_panel_tests PRIVATE hwio_core comctl32)

add_test(NAME hwio_motor_panel COMMAND hwio_motor_panel_tests "${CMAKE_CURRENT_BINARY_DIR}/motor-panel-$<CONFIG>")

add_test(NAME hwio_motor_panel_offscreen COMMAND hwio_motor_panel_tests "${CMAKE_CURRENT_BINARY_DIR}/motor-panel-offscreen-$<CONFIG>" --offscreen)

set_tests_properties(hwio_motor_panel_offscreen PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

set_tests_properties(hwio_motor_bench hwio_motor_panel PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_executable(hwio_failure_tests tests/failures.cpp)

target_link_libraries(hwio_failure_tests PRIVATE hwio_core)

add_test(NAME hwio_failures COMMAND hwio_failure_tests)

set_tests_properties(hwio_failures PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)

add_library(hwio_fdstq_mock SHARED tests/fdstq-mock.cpp)

add_executable(hwio_fdstq_tests tests/fdstq.cpp)

target_link_libraries(hwio_fdstq_tests PRIVATE hwio_core)

add_dependencies(hwio_fdstq_tests hwio_fdstq_mock)

add_test(NAME hwio_fdstq COMMAND hwio_fdstq_tests $<TARGET_FILE:hwio_fdstq_mock>)

set_tests_properties(hwio_fdstq PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_executable(hwio_fds_cdu_tests tests/fds-cdu.cpp)

add_executable(hwio_fds_cdu_check tools/fds-cdu-check.cpp)

target_link_libraries(hwio_fds_cdu_check PRIVATE hwio_core)

target_link_libraries(hwio_fds_cdu_tests PRIVATE hwio_core)

add_test(NAME hwio_fds_cdu_bitmap COMMAND hwio_fds_cdu_tests)

add_library(hwio_phidget_mock SHARED tests/phidgets-mock.cpp)

add_executable(hwio_phidget_tests tests/phidgets.cpp)

target_link_libraries(hwio_phidget_tests PRIVATE hwio_core)

add_dependencies(hwio_phidget_tests hwio_phidget_mock)

add_test(NAME hwio_phidgets COMMAND hwio_phidget_tests $<TARGET_FILE:hwio_phidget_mock>)

add_executable(hwio_fds_diagnostics_tests tests/fds-diagnostics.cpp)

target_link_libraries(hwio_fds_diagnostics_tests PRIVATE hwio_core)

add_test(NAME hwio_fds_diagnostics COMMAND hwio_fds_diagnostics_tests)

set_tests_properties(hwio_phidgets hwio_fds_diagnostics PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_library(hwio_phidget_motor_mock SHARED tests/phidget-motor-mock.cpp)

add_executable(hwio_phidget_motor_tests tests/phidget-motor.cpp)

target_link_libraries(hwio_phidget_motor_tests PRIVATE hwio_core)

add_dependencies(hwio_phidget_motor_tests hwio_phidget_motor_mock hwio_fdstq_mock)

add_test(NAME hwio_phidget_motor COMMAND hwio_phidget_motor_tests $<TARGET_FILE:hwio_phidget_motor_mock> $<TARGET_FILE:hwio_fdstq_mock>)

set_tests_properties(hwio_phidget_motor PROPERTIES TIMEOUT 25 RUN_SERIAL TRUE)

add_executable(hwio_fds_tests tests/fds.cpp)

target_link_libraries(hwio_fds_tests PRIVATE hwio_core)

add_test(NAME hwio_fds COMMAND hwio_fds_tests)

add_library(hwio_fds_mock SHARED tests/fds-mock.cpp tests/fds-mock.rc)

add_executable(hwio_fds_provider_tests tests/fds-provider.cpp)

add_executable(hwio_fds_mcp_options_tests tests/fds-mcp-options.cpp)

target_link_libraries(hwio_fds_mcp_options_tests PRIVATE bcrypt)

add_test(NAME hwio_fds_mcp_options COMMAND hwio_fds_mcp_options_tests)

set_tests_properties(hwio_fds_mcp_options PROPERTIES TIMEOUT 10 RUN_SERIAL TRUE)

add_dependencies(hwio_fds_provider_tests hwio_fds_mock)

target_link_libraries(hwio_fds_provider_tests PRIVATE hwio_core)

add_test(NAME hwio_fds_provider COMMAND hwio_fds_provider_tests $<TARGET_FILE:hwio_fds_mock>)

set_tests_properties(hwio_fds_provider PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_executable(hwio_module_panel_tests tests/module-panel.cpp)

target_link_libraries(hwio_module_panel_tests PRIVATE hwio_core)

add_test(NAME hwio_module_panel COMMAND hwio_module_panel_tests "${CMAKE_CURRENT_BINARY_DIR}/module-panel-renders-$<CONFIG>")

# Native popup/layout and transactional assignment fixtures share one process;
# allow cold graphics/storage startup as well as the additional CDU save checks.
set_tests_properties(hwio_module_panel PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

set_tests_properties(hwio_fds PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_executable(hwio_cockpit_tests tests/cockpit.cpp)

target_link_libraries(hwio_cockpit_tests PRIVATE hwio_core comctl32)

add_test(NAME hwio_cockpit COMMAND hwio_cockpit_tests "${CMAKE_CURRENT_BINARY_DIR}/cockpit-renders-$<CONFIG>")

set_tests_properties(hwio_cockpit PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

add_executable(hwio_output_tests tests/digital-outputs.cpp)

target_link_libraries(hwio_output_tests PRIVATE hwio_core comctl32)

add_test(NAME hwio_outputs COMMAND hwio_output_tests)

set_tests_properties(hwio_outputs PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_executable(hwio_ethernet_tests tests/ethernet.cpp)

target_link_libraries(hwio_ethernet_tests PRIVATE hwio_core)

add_test(NAME hwio_ethernet COMMAND hwio_ethernet_tests)

set_tests_properties(hwio_ethernet PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_executable(hwio_hardware_panel_tests tests/hardware-panel.cpp)

target_link_libraries(hwio_hardware_panel_tests PRIVATE hwio_core)

add_test(NAME hwio_hardware_panel COMMAND hwio_hardware_panel_tests "${CMAKE_CURRENT_BINARY_DIR}/hardware-panel-renders")

set_tests_properties(hwio_hardware_panel PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

add_executable(hwio_tests tests/tests.cpp)

target_link_libraries(hwio_tests PRIVATE hwio_core)

target_compile_definitions(hwio_tests PRIVATE HWIO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../shared/hwio/fixtures")

add_test(NAME hwio_core COMMAND hwio_tests)

add_test(NAME hwio_transport COMMAND hwio_tests --transport)

add_test(NAME hwio_gui COMMAND psx-hwio --ui-smoke-test "${CMAKE_CURRENT_BINARY_DIR}/ui-smoke.txt")

add_test(NAME hwio_startup COMMAND psx-hwio --ui-startup-check "${CMAKE_CURRENT_BINARY_DIR}/startup-$<CONFIG>.txt")

set_tests_properties(hwio_startup PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_executable(hwio_delivery_tests tests/delivery.cpp)

target_link_libraries(hwio_delivery_tests PRIVATE hwio_core)

add_test(NAME hwio_delivery COMMAND hwio_delivery_tests)

add_executable(hwio_acquisition_tests tests/acquisition.cpp)

target_link_libraries(hwio_acquisition_tests PRIVATE hwio_core)

add_test(NAME hwio_acquisition COMMAND hwio_acquisition_tests)

add_executable(hwio_flight_tests tests/flight-display.cpp)

target_link_libraries(hwio_flight_tests PRIVATE hwio_core)

add_test(NAME hwio_flight COMMAND hwio_flight_tests "${CMAKE_CURRENT_BINARY_DIR}/flight-renders")

add_executable(hwio_axis_tests tests/axis-setup.cpp)

target_link_libraries(hwio_axis_tests PRIVATE hwio_core)

add_test(NAME hwio_axes COMMAND hwio_axis_tests)

add_executable(hwio_card_assignment_tests tests/card-assignments.cpp)

target_link_libraries(hwio_card_assignment_tests PRIVATE hwio_core)

add_test(NAME hwio_card_assignments COMMAND hwio_card_assignment_tests "${CMAKE_BINARY_DIR}/card-assignments-$<CONFIG>")

set_tests_properties(hwio_card_assignments PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_executable(hwio_storage_tests tests/storage.cpp)

target_link_libraries(hwio_storage_tests PRIVATE hwio_core)

add_test(NAME hwio_storage COMMAND hwio_storage_tests)

add_executable(hwio_tray_tests tests/tray-status.cpp)

target_link_libraries(hwio_tray_tests PRIVATE hwio_core)

add_test(NAME hwio_tray COMMAND hwio_tray_tests)

add_executable(hwio_button_tests tests/buttons.cpp)

target_link_libraries(hwio_button_tests PRIVATE hwio_core)

add_test(NAME hwio_buttons COMMAND hwio_button_tests)

add_executable(hwio_button_panel_tests tests/button-panel.cpp)

target_link_libraries(hwio_button_panel_tests PRIVATE hwio_core)

add_test(NAME hwio_button_panel COMMAND hwio_button_panel_tests "${CMAKE_CURRENT_BINARY_DIR}/button-panel-renders")

# Full native font/layout audit: every catalogue row at three widths. This is a
# bounded harness allowance, separate from the reported interactive timing target
# and every production communication/actuator deadline.
set_tests_properties(hwio_button_panel PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE)
add_test(NAME hwio_button_names COMMAND hwio_button_panel_tests "${CMAKE_CURRENT_BINARY_DIR}/button-name-renders" --button-names)
set_tests_properties(hwio_button_names PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
add_test(NAME hwio_button_navigation COMMAND hwio_button_panel_tests "${CMAKE_CURRENT_BINARY_DIR}/button-navigation-$<CONFIG>" --benchmark-only)
set_tests_properties(hwio_button_navigation PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

add_executable(hwio_axis_panel_tests tests/axis-panel.cpp)

target_link_libraries(hwio_axis_panel_tests PRIVATE hwio_core)

add_test(NAME hwio_axis_panel COMMAND hwio_axis_panel_tests "${CMAKE_CURRENT_BINARY_DIR}/axis-panel-renders")

set_tests_properties(hwio_axis_panel PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

set_tests_properties(hwio_acquisition PROPERTIES TIMEOUT 15 RUN_SERIAL TRUE)

set_tests_properties(hwio_delivery PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

add_executable(hwio_usb_inspect tests/usb-inspect.cpp)

target_link_libraries(hwio_usb_inspect PRIVATE setupapi)

add_executable(hwio_hid_compare tests/hid-compare.cpp)

target_link_libraries(hwio_hid_compare PRIVATE hwio_core)

add_executable(hwio_raw_observe tests/raw-observe.cpp)

target_link_libraries(hwio_raw_observe PRIVATE user32)

set_tests_properties(hwio_core hwio_transport hwio_gui PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_executable(hwio_catalogue_tests tests/catalogue.cpp)

target_link_libraries(hwio_catalogue_tests PRIVATE hwio_core)

target_compile_definitions(hwio_catalogue_tests PRIVATE HWIO_MIGRATION_EVIDENCE="${PSX_SOURCE_ROOT}/tests/hwio/catalogue")

add_test(NAME hwio_catalogue COMMAND hwio_catalogue_tests "${CMAKE_CURRENT_BINARY_DIR}/catalogue-fixtures-$<CONFIG>")

set_tests_properties(hwio_catalogue PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/output-atlas/atlas.xml")

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/resources/output-atlas/atlas.xml" HWIO_ATLAS_IMAGE_LINES REGEX "<image ")

set(HWIO_ATLAS_IMAGES)

foreach(line IN LISTS HWIO_ATLAS_IMAGE_LINES)

  string(REGEX MATCH "file=\"(images/[a-z0-9-]+\\.png)\"" match "${line}")

  if(NOT match)

    message(FATAL_ERROR "Invalid output atlas image path")

  endif()

  list(APPEND HWIO_ATLAS_IMAGES "${CMAKE_CURRENT_SOURCE_DIR}/resources/output-atlas/${CMAKE_MATCH_1}")

endforeach()

add_custom_target(hwio_runtime_data

  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"

  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/config/catalogue.xml" "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/catalogue.xml"

  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/config/fds-modules.xml" "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/fds-modules.xml"

  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/output-atlas/images"

  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/resources/output-atlas/atlas.xml" "${CMAKE_CURRENT_SOURCE_DIR}/resources/output-atlas/NOTICE.txt" "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/output-atlas"

  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${HWIO_ATLAS_IMAGES} "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/output-atlas/images"

  VERBATIM)

foreach(target IN ITEMS hwio_card_assignment_tests hwio_motor_disconnect_tests hwio_input_override_tests hwio_motor_continuity_tests hwio_motor_homing_tests hwio_motor_mapping_tests hwio_motor_bench_tests hwio_motor_panel_tests hwio_failure_tests hwio_fdstq_tests hwio_phidget_tests hwio_phidget_motor_tests hwio_fds_diagnostics_tests hwio-fds-replay hwio_module_panel_tests hwio_fds_provider_tests hwio_fds_tests hwio_cockpit_tests hwio_output_tests hwio_hardware_panel_tests hwio_ethernet_tests hwio_catalogue_tests psx-hwio hwio_tests hwio_delivery_tests hwio_acquisition_tests hwio_flight_tests hwio_axis_tests hwio_storage_tests hwio_button_tests hwio_button_panel_tests hwio_axis_panel_tests hwio_hid_compare hwio_tray_tests)

  add_dependencies(${target} hwio_runtime_data)

  add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:${target}>)

endforeach()



add_subdirectory(tools/fds-trace fds-trace)



add_executable(hwio_instance_tests tests/application-instance.cpp)

target_link_libraries(hwio_instance_tests PRIVATE hwio_core)

add_dependencies(hwio_instance_tests psx-hwio hwio_runtime_data)

# Build-internal relocation fixture: ordinary product in another executable directory.

add_custom_command(TARGET hwio_instance_tests POST_BUILD

  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/singleton-$<CONFIG>/relocated"

  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:psx-hwio> "${CMAKE_CURRENT_BINARY_DIR}/singleton-$<CONFIG>/relocated/psx-hwio.exe"

  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> "${CMAKE_CURRENT_BINARY_DIR}/singleton-$<CONFIG>/relocated/ass_graphics_lib.dll"

  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/config/catalogue.xml" "${CMAKE_CURRENT_BINARY_DIR}/singleton-$<CONFIG>/relocated/catalogue.xml"

  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:hwio_instance_tests>

  VERBATIM)

add_test(NAME hwio_single_instance COMMAND hwio_instance_tests

  "$<TARGET_FILE:psx-hwio>" "${CMAKE_CURRENT_BINARY_DIR}/$<IF:$<CONFIG:Debug>,Release,Debug>/psx-hwio.exe"

  "${CMAKE_CURRENT_BINARY_DIR}/singleton-$<CONFIG>/relocated/psx-hwio.exe"

  "${CMAKE_CURRENT_BINARY_DIR}/singleton-$<CONFIG>/evidence")

set_tests_properties(hwio_single_instance PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)



add_executable(hwio_analogue_layout_tests tests/analogue-layout.cpp)

target_link_libraries(hwio_analogue_layout_tests PRIVATE hwio_core)

add_dependencies(hwio_analogue_layout_tests hwio_runtime_data)

add_custom_command(TARGET hwio_analogue_layout_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ass::graphics> $<TARGET_FILE_DIR:hwio_analogue_layout_tests>)

add_test(NAME hwio_analogue_layout COMMAND hwio_analogue_layout_tests "${CMAKE_BINARY_DIR}/analogue-layout-$<CONFIG>")

set_tests_properties(hwio_analogue_layout PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)



# The SDK fixture's separately qualified ABI version remains 2.0.1.0.

set_property(TARGET hwio_fds_mock PROPERTY PSX_VERSION_STAMPED TRUE)

psx_stamp_directory("${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_VERSION}")



# All executable targets in this directory are owned hardware applications or tools.

get_property(hwio_icon_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)

foreach(target IN LISTS hwio_icon_targets)

  get_target_property(type "${target}" TYPE)

  if(type STREQUAL "EXECUTABLE")

    psx_target_icon("${target}" HW)

  endif()

endforeach()



# Windows TEMP/TMP are inherited by native fixture APIs and owned child processes.

# Keep all authored output inside this bounded graph, including tests using

# std::filesystem::temp_directory_path(). No machine/user environment is changed.

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-temp/Debug" "${CMAKE_BINARY_DIR}/test-temp/Release")

function(hwio_test_storage directory)

  get_property(tests DIRECTORY "${directory}" PROPERTY TESTS)

  foreach(test IN LISTS tests)

    set_property(TEST "${test}" DIRECTORY "${directory}" APPEND PROPERTY ENVIRONMENT

      "TEMP=${CMAKE_BINARY_DIR}/test-temp/$<CONFIG>" "TMP=${CMAKE_BINARY_DIR}/test-temp/$<CONFIG>")

  endforeach()

  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)

  foreach(child IN LISTS children)

    hwio_test_storage("${child}")

  endforeach()

endfunction()

hwio_test_storage("${CMAKE_CURRENT_SOURCE_DIR}")





# Card properties/portable assignment qualification without unrelated actuator work.

add_custom_target(hwio_card_transfer_checks DEPENDS psx-hwio hwio-fds-replay

  hwio_card_assignment_tests hwio_hardware_panel_tests hwio_module_panel_tests

  hwio_storage_tests hwio_cockpit_tests hwio_axis_tests hwio_button_tests

  hwio_output_tests hwio_catalogue_tests hwio_phidget_tests

  hwio_runtime_phidget_tests hwio_phidget_io_tests)

add_custom_target(hwio_analogue_checks DEPENDS psx-hwio hwio-fds-replay

  hwio_analogue_layout_tests hwio_actuator_tests hwio_actuator_panel_tests

  hwio_stepper_tests hwio_servo_tests hwio_servo_panel_tests hwio_motor_following_tests

  hwio_motor_mapping_tests hwio_motor_panel_tests hwio_motor_motion_tests

  hwio_motor_homing_tests hwio_motor_continuity_tests hwio_motor_bench_tests

  hwio_motor_software_tests hwio_storage_tests hwio_catalogue_tests

  hwio_card_assignment_tests hwio_module_panel_tests hwio_runtime_phidget_tests hwio_trim_display_tests)

add_custom_target(hwio_motor_control_checks DEPENDS psx-hwio hwio-fds-replay

  hwio_thrust_control_tests hwio_situation_control_tests

  hwio_motor_following_tests hwio_motor_alignment_tests hwio_motor_correction_tests

  hwio_motor_mapping_tests hwio_motor_homing_tests hwio_motor_motion_tests

  hwio_motor_continuity_tests hwio_motor_bench_tests hwio_motor_software_tests

  hwio_motor_disconnect_tests hwio_motor_diagnostics_tests hwio_motor_backlash_tests

  hwio_motor_autotune_tests hwio_phidget_motor_tests hwio_phidget_drive_tests

  hwio_fdstq_tests hwio_phidget_tests hwio_runtime_phidget_tests

  hwio_actuator_tests hwio_stepper_tests hwio_servo_tests)

add_custom_target(hwio_stepper_control_checks DEPENDS psx-hwio hwio-fds-replay

  hwio_situation_control_tests hwio_axis_tests hwio_button_tests hwio_cockpit_tests

  hwio_stepper_tests hwio_actuator_tests hwio_actuator_panel_tests

  hwio_motor_control_checks hwio_phidget_io_tests hwio_analogue_layout_tests)


include("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/windows-runtime/GraphicsRuntime.cmake")
psx_shared_graphics_runtime(psx-hwio)
