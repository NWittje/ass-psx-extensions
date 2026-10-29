include_guard(GLOBAL)

function(psx_shared_graphics_runtime target)
  # Graphics is delay-loaded so Windows can enter our adjacent/parent resolver
  # before the first SDK call. Keep the application's own CRT and link settings.
  if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Shared graphics runtime requires the native Windows/MSVC loader")
  endif()
  get_property(enabled TARGET "${target}" PROPERTY PSX_SHARED_GRAPHICS_RUNTIME)
  if(enabled)
    return()
  endif()
  set_property(TARGET "${target}" PROPERTY PSX_SHARED_GRAPHICS_RUNTIME TRUE)
  # Supplier exports are configuration-local. Bind the paired SDK once so a
  # Debug application cannot accidentally qualify against the Release DLL.
  get_target_property(imported ass::graphics IMPORTED)
  if(imported)
    get_filename_component(sdk "${AssGraphics_DIR}/../../../.." ABSOLUTE)
    foreach(configuration IN ITEMS Debug Release)
      foreach(member IN ITEMS bin/ass_graphics_lib.dll lib/ass_graphics_lib.lib)
        if(NOT EXISTS "${sdk}/${configuration}/${member}")
          message(FATAL_ERROR "Incomplete paired graphics SDK: ${sdk}/${configuration}/${member}")
        endif()
      endforeach()
    endforeach()
    set_target_properties(ass::graphics PROPERTIES IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
      IMPORTED_LOCATION_DEBUG "${sdk}/Debug/bin/ass_graphics_lib.dll"
      IMPORTED_IMPLIB_DEBUG "${sdk}/Debug/lib/ass_graphics_lib.lib"
      IMPORTED_LOCATION_RELEASE "${sdk}/Release/bin/ass_graphics_lib.dll"
      IMPORTED_IMPLIB_RELEASE "${sdk}/Release/lib/ass_graphics_lib.lib")
  endif()
  set(PSX_RUNTIME_FONT_ROOT "$ENV{ASS_B612_ROOT}" CACHE PATH "Locally supplied B612 inputs for build/qualification, excluded from publication")
  if(PSX_RUNTIME_FONT_ROOT)
    # Build-tool internal placement only. SDK/publication payloads exclude TTFs.
    foreach(font IN ITEMS B612-Regular.ttf B612-Bold.ttf)
      if(NOT EXISTS "${PSX_RUNTIME_FONT_ROOT}/${font}")
        message(FATAL_ERROR "Missing local B612 build input: ${PSX_RUNTIME_FONT_ROOT}/${font}")
      endif()
      if(CMAKE_CONFIGURATION_TYPES)
        foreach(configuration IN LISTS CMAKE_CONFIGURATION_TYPES)
          configure_file("${PSX_RUNTIME_FONT_ROOT}/${font}" "${CMAKE_BINARY_DIR}/${configuration}/${font}" COPYONLY)
        endforeach()
      else()
        configure_file("${PSX_RUNTIME_FONT_ROOT}/${font}" "${CMAKE_BINARY_DIR}/${font}" COPYONLY)
      endif()
      add_custom_command(TARGET "${target}" POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PSX_RUNTIME_FONT_ROOT}/${font}" "$<TARGET_FILE_DIR:${target}>/${font}")
    endforeach()
  endif()
  target_sources("${target}" PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/graphics_delay_load.cpp")
  target_link_libraries("${target}" PRIVATE delayimp)
  target_link_options("${target}" PRIVATE /DELAYLOAD:ass_graphics_lib.dll)
endfunction()
