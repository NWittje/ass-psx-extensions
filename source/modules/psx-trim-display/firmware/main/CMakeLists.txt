get_filename_component(MODULE "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(SHARED "${MODULE}/../../shared/hardware-link" ABSOLUTE)
idf_component_register(SRCS
  "app_main.cpp" "../platform/runtime.cpp"
  "${MODULE}/core/protocol.cpp" "${MODULE}/core/state.cpp" "${MODULE}/core/application.cpp" "${MODULE}/core/scene.cpp"
  "${SHARED}/src/link.cpp" "${TRIM_ASSET_CPP}"
  INCLUDE_DIRS "${MODULE}/include" "${SHARED}/include" "../platform"
  REQUIRES esp_driver_uart esp_driver_spi esp_driver_gpio esp_driver_ledc esp_lcd esp_timer heap esp_hw_support esp_system)
target_compile_definitions(${COMPONENT_LIB} PRIVATE TRIM_BUILD_ROLE=${TRIM_ROLE})
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
