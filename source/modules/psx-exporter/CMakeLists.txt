cmake_minimum_required(VERSION 3.24)
project(psx_exporter VERSION 1.0.0 LANGUAGES CXX)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/windows-icons/WindowsIcons.cmake")
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "PSX exporter requires native MSVC Windows x64")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/W4 /WX /permissive- /EHsc /utf-8 "$<$<CONFIG:Release>:/Zi>")
add_link_options(/INCREMENTAL:NO "$<$<CONFIG:Release>:/DEBUG:FULL>" "$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")
if(NOT TARGET psx_concurrency)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/concurrency" "${CMAKE_CURRENT_BINARY_DIR}/shared-concurrency")
endif()
add_library(psx_exporter_launcher_policy STATIC src/launcher_policy.cpp)
target_include_directories(psx_exporter_launcher_policy PUBLIC include "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/display-replay/include")
# One GUI-subsystem executable supports desktop startup and redirected JSON CLI.
add_executable(psx-exporter-start WIN32 src/launcher_main.cpp src/display_collector.cpp)
psx_target_icon(psx-exporter-start CO)
# The preserved shared DPF decoder has a source-equivalent std::span overload.
target_compile_features(psx-exporter-start PRIVATE cxx_std_20)
target_include_directories(psx-exporter-start PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/windows-runtime/include")
target_compile_definitions(psx-exporter-start PRIVATE PSX_EXPORTER_GUI_ENTRY=1)
target_link_libraries(psx-exporter-start PRIVATE psx_exporter_launcher_policy bcrypt shell32 ole32 user32 advapi32)

set(PSX_EXPORTER_JAVA_HOME "" CACHE PATH "Optional Java 25+ development kit; otherwise discover installed Java")
if(NOT PSX_EXPORTER_JAVA_HOME)
  find_package(Java 25 REQUIRED COMPONENTS Development)
  # Ask the discovered compiler so Windows PATH forwarding stubs work too.
  execute_process(COMMAND "${Java_JAVAC_EXECUTABLE}" -J-XshowSettings:properties -version
    OUTPUT_VARIABLE psx_exporter_java_stdout ERROR_VARIABLE psx_exporter_java_stderr
    RESULT_VARIABLE psx_exporter_java_result TIMEOUT 10)
  if(NOT psx_exporter_java_result EQUAL 0 OR
     NOT "${psx_exporter_java_stdout}${psx_exporter_java_stderr}" MATCHES "java\\.home[ \t]*=[ \t]*([^\r\n]+)")
    message(FATAL_ERROR "Cannot discover the JDK root; supply PSX_EXPORTER_JAVA_HOME")
  endif()
  string(STRIP "${CMAKE_MATCH_1}" PSX_EXPORTER_JAVA_HOME)
endif()
option(PSX_EXPORTER_BUILD_JAVA "Build startup agent with the installed JDK" ON)
set(PSX_EXPORTER_FMC_BASELINE_AGENT "${CMAKE_CURRENT_SOURCE_DIR}/../../external/exporter-fmc-capture-0.19.0.jar" CACHE FILEPATH "Qualified0.19 capture-only component; keeps paused FMC source outside this hook build")
option(PSX_EXPORTER_NATIVE_FMC_RECORDS "Qualify the native FMC record adapter with the delivered capture/hook component" ON)
set(psx_exporter_composition_args)
if(PSX_EXPORTER_FMC_BASELINE_AGENT)
  list(APPEND psx_exporter_composition_args -HookOnly -FmcBaselineAgent "${PSX_EXPORTER_FMC_BASELINE_AGENT}")
endif()
if(PSX_EXPORTER_NATIVE_FMC_RECORDS)
  if(NOT PSX_EXPORTER_FMC_BASELINE_AGENT)
    message(FATAL_ERROR "Native FMC records require the qualified capture component")
  endif()
  list(APPEND psx_exporter_composition_args -NativeRecords)
endif()
if(PSX_EXPORTER_BUILD_JAVA)
  find_program(PSX_EXPORTER_POWERSHELL NAMES powershell.exe REQUIRED)
  file(GLOB_RECURSE psx_exporter_java_sources "${CMAKE_CURRENT_SOURCE_DIR}/java/*.java")
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/java/psx-exporter-agent.jar"
    COMMAND "${PSX_EXPORTER_POWERSHELL}" -NoProfile
      -File "${CMAKE_CURRENT_SOURCE_DIR}/java/build.ps1" -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/java"
      -JavaHome "${PSX_EXPORTER_JAVA_HOME}"
      -Version "${PROJECT_VERSION}"
      ${psx_exporter_composition_args}
    DEPENDS ${psx_exporter_java_sources} "${CMAKE_CURRENT_SOURCE_DIR}/java/build.ps1" "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    COMMENT "Building Java startup observer and scoped audio-output capability" VERBATIM)
  add_custom_target(psx_exporter_java ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/java/psx-exporter-agent.jar")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/native_runtime.cpp")
  if(NOT TARGET psx_exporter_protocol)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/exporter-protocol" "${CMAKE_CURRENT_BINARY_DIR}/exporter-protocol")
  endif()
  if(NOT TARGET psx_terrain)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/terrain" "${CMAKE_CURRENT_BINARY_DIR}/terrain")
  endif()
  if(NOT TARGET psx_aircraft_sync)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/aircraft-sync" "${CMAKE_CURRENT_BINARY_DIR}/aircraft-sync")
  endif()
  if(NOT TARGET psx_environment)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/environment" "${CMAKE_CURRENT_BINARY_DIR}/environment")
  endif()
  if(NOT TARGET psx_status)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/status" "${CMAKE_CURRENT_BINARY_DIR}/status")
  endif()
  if(NOT TARGET psx_traffic_source)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/traffic-source" "${CMAKE_CURRENT_BINARY_DIR}/traffic-source")
  endif()
  if(NOT TARGET psx_non_normal)
    set(PSX_NON_NORMAL_TESTS ON)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/non-normal" "${CMAKE_CURRENT_BINARY_DIR}/non-normal")
  endif()
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/situation" "${CMAKE_CURRENT_BINARY_DIR}/situation")
  add_library(psx-exporter-native SHARED src/situation_runtime.cpp src/native_runtime.cpp src/terrain_runtime.cpp src/display_runtime.cpp src/aircraft_runtime.cpp src/recorder_runtime.cpp src/du_timing.cpp src/environment_runtime.cpp src/status_runtime.cpp src/traffic_runtime.cpp src/traffic_worker.cpp src/reset_runtime.cpp)
  target_sources(psx-exporter-native PRIVATE src/weight_service_runtime.cpp src/ios_status_runtime.cpp)
  target_compile_features(psx-exporter-native PRIVATE cxx_std_20)
  target_include_directories(psx-exporter-native PRIVATE include "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/windows-runtime/include" "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/psx-instructor/include" "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/display-replay/include" "${PSX_EXPORTER_JAVA_HOME}/include" "${PSX_EXPORTER_JAVA_HOME}/include/win32")
  target_compile_definitions(psx-exporter-native PRIVATE UNICODE _UNICODE PSX_EXPORTER_VERSION="${PROJECT_VERSION}")
  # Runtime SPSC indices deliberately occupy separate cache lines.
  target_compile_options(psx-exporter-native PRIVATE /wd4324)
  target_link_libraries(psx-exporter-native PRIVATE psx_concurrency psx_exporter_protocol psx_terrain psx_aircraft_sync psx_environment psx_status psx_traffic_source psx_non_normal psx_situation advapi32 bcrypt ole32)
endif()

option(PSX_EXPORTER_PUBLISH "Publish standard builds to the canonical bin package" ON)
if(PSX_EXPORTER_PUBLISH AND PSX_EXPORTER_BUILD_JAVA AND TARGET psx-exporter-native)
  add_custom_target(psx_exporter_publish ALL
    COMMAND "${PSX_EXPORTER_POWERSHELL}" -NoProfile
      -File "${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/publish-binaries.ps1"
      -Module Exporter -Configuration "$<CONFIG>" -BuildDirectory "${CMAKE_CURRENT_BINARY_DIR}"
    COMMENT "Publishing exporter launcher, startup agent and native runtime to project bin"
    VERBATIM)
  add_dependencies(psx_exporter_publish psx-exporter-start psx-exporter-native psx_exporter_java)
endif()

option(PSX_EXPORTER_BUILD_TESTS "Build launcher policy and isolated process tests" ON)
if(PSX_EXPORTER_BUILD_TESTS)
  enable_testing()
  add_executable(psx_exporter_situation_client tests/situation_client.cpp)
  target_link_libraries(psx_exporter_situation_client PRIVATE psx_situation)
  add_executable(psx_exporter_fmc_source_client tests/fmc_source_client.cpp)
  target_link_libraries(psx_exporter_fmc_source_client PRIVATE psx_situation)
  if(PSX_EXPORTER_NATIVE_FMC_RECORDS)
    add_executable(psx_exporter_fmc_native_client tests/fmc_native_client.cpp)
    target_link_libraries(psx_exporter_fmc_native_client PRIVATE psx_situation)
  endif()
  add_executable(psx_exporter_du_timing_queues tests/du_timing_queues.cpp)
  target_include_directories(psx_exporter_du_timing_queues PRIVATE "${PSX_EXPORTER_JAVA_HOME}/include" "${PSX_EXPORTER_JAVA_HOME}/include/win32")
  target_link_libraries(psx_exporter_du_timing_queues PRIVATE advapi32 bcrypt)
  add_test(NAME psx_exporter_du_timing_queues COMMAND psx_exporter_du_timing_queues)
  add_executable(psx_exporter_launcher_tests tests/launcher_tests.cpp)
  target_link_libraries(psx_exporter_launcher_tests PRIVATE psx_exporter_launcher_policy)
  add_test(NAME psx_exporter_launcher_policy COMMAND psx_exporter_launcher_tests)
  add_executable(psx_exporter_prepared_file_hashes tests/prepared_file_hashes.cpp)
  target_compile_features(psx_exporter_prepared_file_hashes PRIVATE cxx_std_20)
  target_include_directories(psx_exporter_prepared_file_hashes PRIVATE include)
  add_test(NAME psx_exporter_prepared_file_hashes COMMAND psx_exporter_prepared_file_hashes)
  set_tests_properties(psx_exporter_prepared_file_hashes PROPERTIES TIMEOUT 15)
  add_executable(psx_exporter_launcher_fixture tests/launcher_fixture.cpp)
  add_library(psx_exporter_wrong_native SHARED tests/wrong_native.cpp)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  set(PSX_EXPORTER_TEST_BASELINE_LAUNCHER "" CACHE FILEPATH "Preserved prior launcher for cache inventory equivalence")
  set(psx_exporter_launcher_baseline_args)
  if(PSX_EXPORTER_TEST_BASELINE_LAUNCHER)
    list(APPEND psx_exporter_launcher_baseline_args --baseline-launcher "${PSX_EXPORTER_TEST_BASELINE_LAUNCHER}")
  endif()
  add_test(NAME psx_exporter_launcher_processes
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher_process_tests.py"
      --launcher "$<TARGET_FILE:psx-exporter-start>" --gui "$<TARGET_FILE:psx-exporter-start>" --fixture "$<TARGET_FILE:psx_exporter_launcher_fixture>" ${psx_exporter_launcher_baseline_args})
  set_tests_properties(psx_exporter_launcher_processes PROPERTIES TIMEOUT 180)
  add_test(NAME psx_exporter_launcher_discovery
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher_discovery_tests.py"
      --launcher "$<TARGET_FILE:psx-exporter-start>" --fixture "$<TARGET_FILE:psx_exporter_launcher_fixture>")
  set_tests_properties(psx_exporter_launcher_discovery PROPERTIES TIMEOUT 60)
  if(PSX_EXPORTER_BUILD_JAVA)
    set(psx_exporter_weather_vendor_args)
    if(PSX_EXPORTER_TEST_PSX_JAR)
      list(APPEND psx_exporter_weather_vendor_args -PsxJar "${PSX_EXPORTER_TEST_PSX_JAR}")
    endif()
    add_test(NAME psx_exporter_weather_change_java
      COMMAND "${PSX_EXPORTER_POWERSHELL}" -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/java/test-weather-change.ps1"
      -Agent "${CMAKE_CURRENT_BINARY_DIR}/java/psx-exporter-agent.jar" -JavaHome "${PSX_EXPORTER_JAVA_HOME}"
      -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/weather-change-$<CONFIG>" ${psx_exporter_weather_vendor_args})
    set_tests_properties(psx_exporter_weather_change_java PROPERTIES TIMEOUT 90)
    add_test(NAME psx_exporter_runtime_single_instance
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_instance_test.py"
        --native "$<TARGET_FILE:psx-exporter-native>" --launcher "$<TARGET_FILE:psx-exporter-start>"
        --java "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe"
        --agent "${CMAKE_CURRENT_BINARY_DIR}/java/psx-exporter-agent.jar"
        --evidence "${CMAKE_CURRENT_BINARY_DIR}/runtime-instance-$<CONFIG>.json")
    set_tests_properties(psx_exporter_runtime_single_instance PROPERTIES TIMEOUT 40 RUN_SERIAL TRUE)
    set(PSX_EXPORTER_TEST_PSX_JAR "" CACHE FILEPATH "Optional installed PSX JAR for read-only static transformation validation")
    set(psx_exporter_java_test_args)
    if(PSX_EXPORTER_TEST_PSX_JAR)
      list(APPEND psx_exporter_java_test_args -PsxJar "${PSX_EXPORTER_TEST_PSX_JAR}")
      add_test(NAME psx_exporter_wrong_native_rejected
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_probe_rejection.py"
          --launcher "$<TARGET_FILE:psx-exporter-start>"
          --agent "${CMAKE_CURRENT_BINARY_DIR}/java/psx-exporter-agent.jar"
          --wrong-native "$<TARGET_FILE:psx_exporter_wrong_native>"
          --psx-jar "${PSX_EXPORTER_TEST_PSX_JAR}"
          --evidence "${CMAKE_CURRENT_BINARY_DIR}/wrong-native-$<CONFIG>.json")
      set_tests_properties(psx_exporter_wrong_native_rejected PROPERTIES TIMEOUT 25)
    endif()
    add_test(NAME psx_exporter_java_observer
      COMMAND "${PSX_EXPORTER_POWERSHELL}" -NoProfile
        -File "${CMAKE_CURRENT_SOURCE_DIR}/java/build.ps1"
        -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>"
        -JavaHome "${PSX_EXPORTER_JAVA_HOME}" -RunTests ${psx_exporter_composition_args} ${psx_exporter_java_test_args})
    # The complete installed FMC frontier audit alone takes over 120 seconds.
    set_tests_properties(psx_exporter_java_observer PROPERTIES TIMEOUT 1800)
    set(psx_capture_initialization_test "psx.exporter.FmcInitializationTest")
    if(PSX_EXPORTER_FMC_BASELINE_AGENT)
      set(psx_capture_initialization_test "psx.exporter.CaptureComponentInitializationTest")
    endif()
    add_test(NAME psx_exporter_fmc_initialization
      COMMAND "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe" --enable-native-access=ALL-UNNAMED
        -cp "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes;${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        "${psx_capture_initialization_test}" "$<TARGET_FILE:psx-exporter-native>")
    set_tests_properties(psx_exporter_fmc_initialization PROPERTIES TIMEOUT 25 DEPENDS psx_exporter_java_observer)
    set(PSX_EXPORTER_TEST_OLD_DU_TIMING_NATIVE "" CACHE FILEPATH "Preserved pre-timing DLL for optional timing JNI rejection")
    set(psx_du_timing_old_args)
    if(PSX_EXPORTER_TEST_OLD_DU_TIMING_NATIVE)
      list(APPEND psx_du_timing_old_args --old-native "${PSX_EXPORTER_TEST_OLD_DU_TIMING_NATIVE}")
    endif()
    add_test(NAME psx_exporter_du_timing_isolation
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/du_timing_isolation.py"
        --java "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe" --javac "${PSX_EXPORTER_JAVA_HOME}/bin/javac.exe"
        --agent "${CMAKE_CURRENT_BINARY_DIR}/java/psx-exporter-agent.jar"
        --tests "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        --native "$<TARGET_FILE:psx-exporter-native>"
        --output "${CMAKE_CURRENT_BINARY_DIR}/du-timing-$<CONFIG>" ${psx_du_timing_old_args})
    set_tests_properties(psx_exporter_du_timing_isolation PROPERTIES TIMEOUT 120 DEPENDS psx_exporter_java_observer)
    add_test(NAME psx_exporter_status_jni
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/status_native_test.py"
        --java "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe"
        --classes "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes"
        --tests "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        --native "$<TARGET_FILE:psx-exporter-native>")
    set_tests_properties(psx_exporter_status_jni PROPERTIES TIMEOUT 25 DEPENDS psx_exporter_java_observer)
    add_test(NAME psx_exporter_ios_status_jni
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ios_status_native_test.py"
        --native "$<TARGET_FILE:psx-exporter-native>")
    set_tests_properties(psx_exporter_ios_status_jni PROPERTIES TIMEOUT 25)
    add_test(NAME psx_exporter_weight_action_jni
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/weight_native_test.py"
        --java "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe"
        --classes "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes"
        --tests "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        --native "$<TARGET_FILE:psx-exporter-native>")
    set_tests_properties(psx_exporter_weight_action_jni PROPERTIES TIMEOUT 25 DEPENDS psx_exporter_java_observer)
    add_test(NAME psx_exporter_weight_service
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/weight_native_test.py"
        --java "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe"
        --classes "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes"
        --tests "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        --native "$<TARGET_FILE:psx-exporter-native>"
        --fixture "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/hook-successor-fixtures"
        --psx "${PSX_EXPORTER_TEST_PSX_JAR}")
    set_tests_properties(psx_exporter_weight_service PROPERTIES TIMEOUT 45 DEPENDS psx_exporter_java_observer)
    add_test(NAME psx_exporter_situation_worker
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/situation_worker_test.py"
        --java "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe"
        --classes "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes"
        --tests "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        --fixture "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/situation-fixture.jar"
        --native "$<TARGET_FILE:psx-exporter-native>" --client "$<TARGET_FILE:psx_exporter_situation_client>"
        --output "${CMAKE_CURRENT_BINARY_DIR}/situation-worker-$<CONFIG>.json")
    set_tests_properties(psx_exporter_situation_worker PROPERTIES TIMEOUT 55 DEPENDS psx_exporter_java_observer)
    add_test(NAME psx_exporter_fmc_source_worker
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fmc_source_worker_test.py"
        --java "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe"
        --agent "${CMAKE_CURRENT_BINARY_DIR}/java/psx-exporter-agent.jar"
        --tests "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        --fixture "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/situation-fixture.jar"
        --native "$<TARGET_FILE:psx-exporter-native>" --client "$<TARGET_FILE:psx_exporter_fmc_source_client>"
        --work "${CMAKE_CURRENT_BINARY_DIR}/fmc-source-$<CONFIG>")
    set_tests_properties(psx_exporter_fmc_source_worker PROPERTIES TIMEOUT 55 DEPENDS psx_exporter_java_observer)
    if(PSX_EXPORTER_NATIVE_FMC_RECORDS)
      add_test(NAME psx_exporter_fmc_native_worker
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fmc_native_worker_test.py"
          --java "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe"
          --classes "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes"
          --tests "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
          --profile "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/native-profile.txt"
          --native "$<TARGET_FILE:psx-exporter-native>" --client "$<TARGET_FILE:psx_exporter_fmc_native_client>"
          --work "${CMAKE_CURRENT_BINARY_DIR}/fmc-native-$<CONFIG>")
      set_tests_properties(psx_exporter_fmc_native_worker PROPERTIES TIMEOUT 55 DEPENDS psx_exporter_java_observer)
    endif()
    add_test(NAME psx_exporter_reset_worker
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/reset_worker_tests.py"
        --java "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe"
        --classes "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes"
        --tests "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        --fixture "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/reset-fixture.jar"
        --native "$<TARGET_FILE:psx-exporter-native>")
    set_tests_properties(psx_exporter_reset_worker PROPERTIES TIMEOUT 30 DEPENDS psx_exporter_java_observer)
    add_test(NAME psx_exporter_traffic_jni
      COMMAND "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe" --enable-native-access=ALL-UNNAMED
        -cp "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes;${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        psx.exporter.TrafficNativeFixture "$<TARGET_FILE:psx-exporter-native>")
    set_tests_properties(psx_exporter_traffic_jni PROPERTIES TIMEOUT 25 DEPENDS psx_exporter_java_observer)
    add_test(NAME psx_exporter_traffic_worker
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/traffic_worker_tests.py"
        --java "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe"
        --classes "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes"
        --fixture-classes "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        --native "$<TARGET_FILE:psx-exporter-native>"
        --output "${CMAKE_CURRENT_BINARY_DIR}/traffic-worker-$<CONFIG>.json")
    set_tests_properties(psx_exporter_traffic_worker PROPERTIES TIMEOUT 55 DEPENDS psx_exporter_java_observer)
    set(PSX_EXPORTER_TEST_OLD_TRAFFIC_NATIVE "" CACHE FILEPATH "Preserved older native DLL for independent optional traffic rejection")
    if(PSX_EXPORTER_TEST_OLD_TRAFFIC_NATIVE)
      add_test(NAME psx_exporter_traffic_compatibility
        COMMAND "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe" --enable-native-access=ALL-UNNAMED
          -cp "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes;${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
          psx.exporter.TrafficCompatibilityFixture "${PSX_EXPORTER_TEST_OLD_TRAFFIC_NATIVE}")
      set_tests_properties(psx_exporter_traffic_compatibility PROPERTIES TIMEOUT 15 DEPENDS psx_exporter_java_observer)
    endif()
    add_test(NAME psx_exporter_terrain_jni
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/terrain_native_test.py"
        --java "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe"
        --classes "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes"
        --tests "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        --native "$<TARGET_FILE:psx-exporter-native>")
    set_tests_properties(psx_exporter_terrain_jni PROPERTIES TIMEOUT 25 DEPENDS psx_exporter_java_observer)
    add_test(NAME psx_exporter_display_jni
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/display_native_test.py"
        --java "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe"
        --classes "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes"
        --tests "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        --native "$<TARGET_FILE:psx-exporter-native>" --launcher "$<TARGET_FILE:psx-exporter-start>"
        --evidence "${CMAKE_CURRENT_BINARY_DIR}/display-$<CONFIG>/jni")
    set_tests_properties(psx_exporter_display_jni PROPERTIES TIMEOUT 45 DEPENDS psx_exporter_java_observer)
    add_test(NAME psx_exporter_recorder_java
      COMMAND "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe" --enable-native-access=ALL-UNNAMED -Djava.awt.headless=true
        -cp "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes;${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        psx.exporter.RecorderTests "$<TARGET_FILE:psx-exporter-native>"
        "${CMAKE_CURRENT_BINARY_DIR}/recorder-$<CONFIG>/fixtures"
        "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/recorder-fixtures"
        "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes" "${PSX_EXPORTER_TEST_PSX_JAR}")
    set_tests_properties(psx_exporter_recorder_java PROPERTIES TIMEOUT 30 DEPENDS psx_exporter_java_observer)
    add_test(NAME psx_exporter_instrument_java
      COMMAND "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe" --enable-native-access=ALL-UNNAMED -Djava.awt.headless=true
        -cp "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes;${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        psx.exporter.InstrumentTests "$<TARGET_FILE:psx-exporter-native>"
        "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/instrument-fixtures"
        "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/instrument-stub"
        "${CMAKE_CURRENT_BINARY_DIR}/instrument-$<CONFIG>/fixtures-lower" "${PSX_EXPORTER_TEST_PSX_JAR}")
    set_tests_properties(psx_exporter_instrument_java PROPERTIES TIMEOUT 45 DEPENDS psx_exporter_java_observer)
    add_test(NAME psx_exporter_instrument_jni
      COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/instrument_native_test.py"
        --java "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe"
        --classes "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes"
        --tests "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        --fixtures "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/instrument-fixtures"
        --native "$<TARGET_FILE:psx-exporter-native>"
        --evidence "${CMAKE_CURRENT_BINARY_DIR}/instrument-$<CONFIG>/jni")
    set_tests_properties(psx_exporter_instrument_jni PROPERTIES TIMEOUT 40 DEPENDS psx_exporter_instrument_java)
    add_test(NAME psx_exporter_six_recorder_java
      COMMAND "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe" --enable-native-access=ALL-UNNAMED -Djava.awt.headless=true
        -cp "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes;${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        psx.exporter.SixRecorderTest "$<TARGET_FILE:psx-exporter-native>" "$<TARGET_FILE:psx-exporter-native>"
        "${CMAKE_CURRENT_BINARY_DIR}/recorder-$<CONFIG>/six-fixtures")
    set_tests_properties(psx_exporter_six_recorder_java PROPERTIES TIMEOUT 30 DEPENDS psx_exporter_java_observer)
    if(NOT PSX_EXPORTER_FMC_BASELINE_AGENT)
    add_test(NAME psx_exporter_fmc_rendering_recorder_java
      COMMAND "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe" --enable-native-access=ALL-UNNAMED -Djava.awt.headless=true
        -cp "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes;${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        psx.exporter.FmcRenderingRecorderTest "$<TARGET_FILE:psx-exporter-native>")
    set_tests_properties(psx_exporter_fmc_rendering_recorder_java PROPERTIES TIMEOUT 30 DEPENDS psx_exporter_java_observer)
    endif()
    add_test(NAME psx_exporter_retained_recorder_java
      COMMAND "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe" --enable-native-access=ALL-UNNAMED -Djava.awt.headless=true
        -cp "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes;${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes"
        psx.exporter.RetainedTest "$<TARGET_FILE:psx-exporter-native>" "$<TARGET_FILE:psx-exporter-native>"
        "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/display-fixtures"
        "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/retained-fixtures"
        "${CMAKE_CURRENT_BINARY_DIR}/recorder-$<CONFIG>/retained.json")
    set_tests_properties(psx_exporter_retained_recorder_java PROPERTIES TIMEOUT 30 DEPENDS psx_exporter_java_observer)
    set(psx_recorder_fallback_source "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/exporter-runtime/java/psx/exporter/RecorderPreparationFallbackTest.java")
    set(psx_recorder_fallback_classes "${CMAKE_CURRENT_BINARY_DIR}/recorder-fallback-fixture")
    add_custom_command(OUTPUT "${psx_recorder_fallback_classes}/psx/exporter/RecorderPreparationFallbackTest.class"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${psx_recorder_fallback_classes}"
      COMMAND "${PSX_EXPORTER_JAVA_HOME}/bin/javac.exe" --release 25
        -cp "${CMAKE_CURRENT_BINARY_DIR}/java/psx-exporter-agent.jar" -d "${psx_recorder_fallback_classes}" "${psx_recorder_fallback_source}"
      DEPENDS psx_exporter_java "${psx_recorder_fallback_source}" VERBATIM)
    add_custom_target(psx_recorder_fallback_fixture ALL DEPENDS "${psx_recorder_fallback_classes}/psx/exporter/RecorderPreparationFallbackTest.class")
    add_test(NAME psx_exporter_recorder_preparation_fallback
      COMMAND "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe" --enable-native-access=ALL-UNNAMED -Djava.awt.headless=true
        -Xmx500m -XX:MaxDirectMemorySize=64m
        -cp "${CMAKE_CURRENT_BINARY_DIR}/java/psx-exporter-agent.jar;${psx_recorder_fallback_classes}"
        psx.exporter.RecorderPreparationFallbackTest "$<TARGET_FILE:psx-exporter-native>"
        "${CMAKE_CURRENT_BINARY_DIR}/recorder-$<CONFIG>/preparation-fallback.json")
    set_tests_properties(psx_exporter_recorder_preparation_fallback PROPERTIES TIMEOUT 30)
    set(psx_agent_fallback_source "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/exporter-runtime/java/psx/exporter/AgentRecorderFallbackTest.java")
    add_custom_command(OUTPUT "${psx_recorder_fallback_classes}/psx/exporter/AgentRecorderFallbackTest.class"
      COMMAND "${PSX_EXPORTER_JAVA_HOME}/bin/javac.exe" --release 25
        -cp "${CMAKE_CURRENT_BINARY_DIR}/java/psx-exporter-agent.jar" -d "${psx_recorder_fallback_classes}" "${psx_agent_fallback_source}"
      DEPENDS psx_exporter_java "${CMAKE_CURRENT_BINARY_DIR}/java/psx-exporter-agent.jar" "${psx_agent_fallback_source}" VERBATIM)
    add_custom_target(psx_agent_fallback_fixture ALL DEPENDS "${psx_recorder_fallback_classes}/psx/exporter/AgentRecorderFallbackTest.class")
    if(PSX_EXPORTER_TEST_PSX_JAR)
      add_test(NAME psx_exporter_agent_recorder_fallback
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/exporter-runtime/agent_recorder_fallback.py"
          --java "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe" --agent "${CMAKE_CURRENT_BINARY_DIR}/java/psx-exporter-agent.jar"
          --native "$<TARGET_FILE:psx-exporter-native>" --psx "${PSX_EXPORTER_TEST_PSX_JAR}"
          --classes "${psx_recorder_fallback_classes}" --output "${CMAKE_CURRENT_BINARY_DIR}/agent-fallback-$<CONFIG>")
      set_tests_properties(psx_exporter_agent_recorder_fallback PROPERTIES TIMEOUT 45)
    endif()
    add_test(NAME psx_exporter_recorder_jni
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/recorder_native_test.py"
        --java "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe" --classes "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/classes"
        --tests "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes" --native "$<TARGET_FILE:psx-exporter-native>"
        --launcher "$<TARGET_FILE:psx-exporter-start>" --evidence "${CMAKE_CURRENT_BINARY_DIR}/recorder-$<CONFIG>/jni")
    set_tests_properties(psx_exporter_recorder_jni PROPERTIES TIMEOUT 60 DEPENDS psx_exporter_java_observer)
    add_test(NAME psx_exporter_observer_cold
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/observer_cold_test.py"
        --java "${PSX_EXPORTER_JAVA_HOME}/bin/java.exe" --agent "${CMAKE_CURRENT_BINARY_DIR}/java/psx-exporter-agent.jar"
        --tests "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/test-classes" --fixtures "${CMAKE_CURRENT_BINARY_DIR}/java-tests/$<CONFIG>/display-fixtures"
        --native "$<TARGET_FILE:psx-exporter-native>" --launcher "$<TARGET_FILE:psx-exporter-start>"
        --evidence "${CMAKE_CURRENT_BINARY_DIR}/recorder-$<CONFIG>/cold")
    set_tests_properties(psx_exporter_observer_cold PROPERTIES TIMEOUT 35 DEPENDS psx_exporter_java_observer)
  endif()
  # Authored fixture output belongs to this bounded graph, including Python's
  # default temporary directories. This changes no installed process settings.
  set(PSX_EXPORTER_TEST_TEMPORARY_ROOT "${CMAKE_CURRENT_BINARY_DIR}/test-temporary"
      CACHE PATH "Task-owned temporary root; keep fixture cache paths within Windows path limits")
  file(MAKE_DIRECTORY "${PSX_EXPORTER_TEST_TEMPORARY_ROOT}/Debug"
                      "${PSX_EXPORTER_TEST_TEMPORARY_ROOT}/Release")
  # These fixtures deliberately acquire the same production runtime identity.
  # Separate test processes cannot bootstrap it concurrently, even with distinct
  # temporary files and pipes. This lock never controls a simulator process.
  foreach(runtime_test IN ITEMS psx_exporter_runtime_single_instance
      psx_exporter_status_jni psx_exporter_traffic_worker psx_exporter_terrain_jni
      psx_exporter_agent_recorder_fallback psx_exporter_observer_cold)
    if(TEST "${runtime_test}")
      set_tests_properties("${runtime_test}" PROPERTIES RESOURCE_LOCK psx_exporter_runtime)
    endif()
  endforeach()
  get_property(psx_exporter_tests DIRECTORY PROPERTY TESTS)
  set_tests_properties(${psx_exporter_tests} PROPERTIES ENVIRONMENT
    "PYTHONDONTWRITEBYTECODE=1;TMP=${PSX_EXPORTER_TEST_TEMPORARY_ROOT}/$<CONFIG>;TEMP=${PSX_EXPORTER_TEST_TEMPORARY_ROOT}/$<CONFIG>;TMPDIR=${PSX_EXPORTER_TEST_TEMPORARY_ROOT}/$<CONFIG>")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../shared/windows-version/VersionInfo.cmake")
psx_stamp_directory("${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_VERSION}")
